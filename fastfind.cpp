/*
 * fastfind — an Everything-style instant file-name search tool for Windows.
 * (GUI sibling: fastfindui.cpp — same core with a tray-resident window UI.)
 *
 * Mechanism (the same core idea as voidtools Everything):
 *   1. Build the index by reading the raw NTFS $MFT directly from each fixed
 *      NTFS volume: boot sector -> geometry, MFT record 0 -> $DATA run list,
 *      then sector-aligned reads of every run + USA fixups + $FILE_NAME
 *      parsing. FSCTL_ENUM_USN_DATA is NOT used for indexing: on current
 *      Windows builds it silently skips a large share of records (observed
 *      56k of 1.6M files, including C:\Windows itself).
 *   2. Keep a sorted, lower-cased in-memory array of full paths.
 *   3. Answer queries with an in-memory substring scan (instant, no disk I/O).
 *   4. Optionally track live changes through the NTFS USN journal (--watch).
 *
 * Fallback: without volume read rights, fall back to a FindFirstFile walk.
 *
 * Build:  windres fastfindui.rc fastfindui_rc.o   (icon resource, shared)
 *         g++ -O2 -std=c++17 -Wall fastfind.cpp fastfindui_rc.o -o fastfind.exe
 * Usage:  fastfind --build [--fresh]     build/rebuild the index cache
 *         fastfind <words...>            one-shot search (all words must match)
 *         fastfind -i [--watch]          interactive search (--watch: live updates)
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

// ---------------------------------------------------------------- FSCTL data

#ifndef FSCTL_READ_USN_JOURNAL
#define FSCTL_READ_USN_JOURNAL 0x000900bb
#endif
#ifndef FSCTL_QUERY_USN_JOURNAL
#define FSCTL_QUERY_USN_JOURNAL 0x000900f4
#endif
#ifndef FSCTL_GET_NTFS_VOLUME_DATA
#define FSCTL_GET_NTFS_VOLUME_DATA 0x00090064
#endif

#ifndef USN_REASON_FILE_CREATE
#define USN_REASON_FILE_CREATE      0x00000100
#endif
#ifndef USN_REASON_FILE_DELETE
#define USN_REASON_FILE_DELETE      0x00000200
#endif
#ifndef USN_REASON_RENAME_OLD_NAME
#define USN_REASON_RENAME_OLD_NAME  0x00001000
#endif
#ifndef USN_REASON_RENAME_NEW_NAME
#define USN_REASON_RENAME_NEW_NAME  0x00002000
#endif

#pragma pack(push, 1)
struct ReadUsnJournalDataV0 {
    uint64_t StartUsn;
    uint32_t ReasonMask;
    uint32_t ReturnOnlyOnClose;
    uint64_t Timeout;
    uint64_t BytesToWaitFor;
    uint64_t UsnJournalID;
};
struct UsnJournalDataV0 {
    uint64_t UsnJournalID;
    uint64_t FirstUsn;
    uint64_t NextUsn;
    uint64_t LowestValidUsn;
    uint64_t MaxUsn;
    uint64_t MaximumSize;
    uint64_t AllocationDelta;
};
struct NtfsFileRecordHeader {
    uint32_t signature;          // 'FILE'
    uint16_t usaOffset;
    uint16_t usaCount;
    uint64_t usn;
    uint16_t sequenceNumber;
    uint16_t referenceCount;
    uint16_t firstAttributeOffset;
    uint16_t flags;              // bit 0 = in use
    uint32_t bytesInUse;
    uint32_t bytesAllocated;
};
struct NtfsAttributeHeader {
    uint32_t type;               // 0x30 = $FILE_NAME
    uint32_t length;
    uint8_t  nonResident;
    uint8_t  nameLength;
    uint16_t nameOffset;
    uint16_t flagsAttr;
    uint16_t attributeId;
    // resident payload descriptor at +0x10: valueLength(u32), valueOffset(u16)
};
struct NtfsFileNameAttribute {
    uint64_t parentFrn;
    uint64_t creationTime, modificationTime, mftModificationTime, readTime;
    uint64_t allocatedSize, realSize;
    uint32_t flagsFn;            // 0x10000000 = directory
    uint32_t reparse;
    uint8_t  nameLength;         // characters
    uint8_t  namespaceId;        // 0 POSIX, 1 Win32, 2 DOS, 3 Win32+DOS
    // WCHAR name[] follows
};
#pragma pack(pop)

static const uint64_t ROOT_FRN = 5;   // NTFS root directory MFT record number

// ---------------------------------------------------------------- index data

struct Node {
    uint64_t parent;
    uint32_t attrs;
    std::vector<std::wstring> names;  // primary name (+ alias when present)
};

struct VolumeIndex {
    wchar_t letter = 0;
    DWORD serial = 0;
    std::unordered_map<uint64_t, Node> nodes;
};

struct Hit {
    std::wstring path;    // display path, e.g. C:\Windows\System32\cmd.exe
    std::wstring lower;   // lower-cased copy used for matching
};

static std::vector<VolumeIndex> g_volumes;
static std::vector<Hit> g_hits;             // the flat, sorted index
static bool g_dirty = false;                // index needs rebuild after updates
static uint64_t g_totalNames = 0;

static std::wstring toLower(std::wstring s) {
    // ASCII fast path is enough for matching; keeps CJK intact.
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c += 32;
    return s;
}

// ------------------------------------------------------- direct $MFT reading

/* This is the mechanism voidtools Everything documents: read the raw $MFT
   file directly from the volume instead of crawling directories. FSCTL-based
   enumeration is not used because on current Windows builds it silently skips
   a large share of records (observed 56k of 1.6M files on a Win11 26100
   system, including C:\Windows itself).

   Steps: boot sector -> geometry; MFT record 0 -> $MFT's own $DATA run list;
   then every run is read raw and parsed record by record. The run list is
   mandatory because real-world $MFTs are fragmented (observed 10 runs here). */

static bool readAligned(HANDLE h, uint64_t byteOffset, void* dst, uint32_t bytes) {
    LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)byteOffset;
    DWORD got = 0;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
    while (bytes > 0) {
        if (!ReadFile(h, dst, bytes, &got, nullptr) || got == 0) return false;
        dst = (char*)dst + got; bytes -= got;
    }
    return true;
}

struct DataRun { uint64_t lcn; uint64_t clusters; };

// Apply the Update Sequence Array fixups to one FILE record in place.
static bool applyUsaFixups(uint8_t* rec, uint32_t recLen) {
    NtfsFileRecordHeader* hdr = (NtfsFileRecordHeader*)rec;
    if (hdr->signature != 0x454C4946) return false;     // "FILE"
    uint16_t usaOff = hdr->usaOffset, usaCnt = hdr->usaCount;
    if (usaOff < 48 || usaCnt == 0 || (uint32_t)usaOff + (uint32_t)usaCnt * 2u > recLen) return false;
    const uint16_t* usa = (const uint16_t*)(rec + usaOff);
    const uint32_t sectorSize = 512;                    // NTFS hard requirement
    if ((uint32_t)(usaCnt - 1) != recLen / sectorSize) return false;
    for (uint32_t s = 0; s + 1 < usaCnt; s++) {
        uint16_t* tail = (uint16_t*)(rec + (s + 1) * sectorSize - 2);
        if (*tail != usa[0]) return false;              // stale/corrupt sector
        *tail = usa[s + 1];
    }
    return true;
}

struct Harvested { uint64_t parent; bool isDir; std::vector<std::wstring> names; };

// Extract every $FILE_NAME (parent + name) from one fixed-up FILE record.
static bool harvestRecordNames(uint8_t* rec, uint32_t recLen, Harvested& out) {
    NtfsFileRecordHeader* hdr = (NtfsFileRecordHeader*)rec;
    if (!(hdr->flags & 0x0001)) return false;           // deleted / free slot
    out.parent = 0; out.isDir = false; out.names.clear();
    uint32_t off = hdr->firstAttributeOffset;
    for (int guard = 0; guard < 64; guard++) {
        if (off + 0x18 > recLen) break;
        NtfsAttributeHeader* at = (NtfsAttributeHeader*)(rec + off);
        if (at->type == 0xFFFFFFFF) break;              // end-of-attributes marker
        if (at->type == 0x10) out.isDir = true;         // presence of $STANDARD_INFORMATION
        if (at->length < 0x18 || off + at->length > recLen) break;
        if (at->type == 0x30 && !at->nonResident && at->nameLength == 0) {
            uint32_t valueLen; uint16_t valueOff;
            memcpy(&valueLen, rec + off + 0x10, 4);
            memcpy(&valueOff, rec + off + 0x14, 2);
            uint32_t attrEnd = off + at->length;
            if (valueOff >= 0x18 && off + valueOff + sizeof(NtfsFileNameAttribute) <= attrEnd) {
                NtfsFileNameAttribute* fn = (NtfsFileNameAttribute*)(rec + off + valueOff);
                if (fn->namespaceId == 2) { off += at->length; continue; }  // pure DOS alias
                const wchar_t* name = (const wchar_t*)((uint8_t*)fn + sizeof(NtfsFileNameAttribute));
                size_t nlen = fn->nameLength;
                if (nlen && (uint8_t*)name + nlen * 2 <= rec + attrEnd) {
                    out.parent = fn->parentFrn & 0xFFFFFFFFFFFFULL;
                    if (fn->flagsFn & 0x10000000) out.isDir = true;
                    out.names.emplace_back(name, nlen);
                }
            }
        }
        off += at->length;
    }
    return !out.names.empty();
}

// Parse the sparse data-run list of a non-resident $DATA attribute.
static bool parseDataRuns(const uint8_t* attr, uint32_t attrLen, std::vector<DataRun>& runs) {
    uint16_t runListOffset;
    memcpy(&runListOffset, attr + 0x20, 2);             // non-resident header field
    if (runListOffset == 0 || runListOffset >= attrLen) return false;
    const uint8_t* p = attr + runListOffset;
    const uint8_t* end = attr + attrLen;
    uint64_t prevLcn = 0;
    while (p < end && *p) {
        uint8_t hdrB = *p++;
        int offBytes = hdrB >> 4, lenBytes = hdrB & 0xF;  // high nibble = offset size, low = length size
        if (lenBytes == 0 || lenBytes > 8 || offBytes > 8) return false;
        if (p + lenBytes + offBytes > end) return false;
        uint64_t clusters = 0;
        memcpy(&clusters, p, lenBytes); p += lenBytes;
        int64_t delta = 0;
        memcpy(&delta, p, offBytes); p += offBytes;     // signed little-endian
        if (offBytes < 8 && (p[-1] & 0x80)) delta -= (1LL << (offBytes * 8));
        prevLcn = delta >= 0 ? prevLcn + (uint64_t)delta
                             : (uint64_t)((int64_t)prevLcn + delta);
        runs.push_back({ prevLcn, clusters });
    }
    return !runs.empty();
}

static bool readVolumeMftDirect(HANDLE h, VolumeIndex& vol) {
    static uint64_t recordCounter = 0;                 // global MFT record number
    recordCounter = 0;
    auto& g_recordCounter = recordCounter;
    // 1. boot sector geometry
    uint8_t bs[512];
    if (!readAligned(h, 0, bs, sizeof bs)) return false;
    if (memcmp(bs + 3, "NTFS", 4) != 0) return false;
    uint16_t bytesPerSector; memcpy(&bytesPerSector, bs + 0x0B, 2);
    uint8_t sectorsPerCluster = bs[0x0D];
    uint64_t mftLcn; memcpy(&mftLcn, bs + 0x30, 8);
    int8_t clustersPerRecord = (int8_t)bs[0x40];        // negative => 2^(-v) bytes
    uint32_t clusterSize = (uint32_t)bytesPerSector * sectorsPerCluster;
    uint32_t recLen = clustersPerRecord < 0 ? (1u << -clustersPerRecord)
                                            : (uint32_t)clustersPerRecord * clusterSize;
    if (bytesPerSector != 512 || recLen < 256 || recLen > 8192 || clusterSize == 0) return false;

    // 2. read MFT record 0 and take its $DATA run list
    std::vector<uint8_t> rec0(recLen);
    if (!readAligned(h, mftLcn * clusterSize, rec0.data(), recLen)) return false;
    if (!applyUsaFixups(rec0.data(), recLen)) return false;
    std::vector<DataRun> runs;
    {
        NtfsFileRecordHeader* hdr = (NtfsFileRecordHeader*)rec0.data();
        uint32_t off = hdr->firstAttributeOffset;
        for (int guard = 0; guard < 64; guard++) {
            if (off + 0x18 > recLen) break;
            NtfsAttributeHeader* at = (NtfsAttributeHeader*)(rec0.data() + off);
            if (at->type == 0xFFFFFFFF) break;
            if (at->length < 0x18 || off + at->length > recLen) break;
            if (at->type == 0x80 && at->nonResident) {
                if (!parseDataRuns(rec0.data() + off, at->length, runs)) return false;
                break;
            }
            off += at->length;
        }
    }
    if (runs.empty()) return false;

    // 3. walk every run, parsing records; record number == FRN
    const uint32_t chunkBytes = 8 << 20;                // 8 MiB, 512-aligned
    std::vector<uint8_t> chunk(chunkBytes);
    Harvested h2;
    for (auto& run : runs) {
        uint64_t runBytes = run.clusters * clusterSize;
        for (uint64_t done = 0; done < runBytes; ) {
            uint32_t want = (uint32_t)std::min<uint64_t>({ (uint64_t)chunkBytes, runBytes - done });
            want -= want % recLen;                      // keep record boundaries
            if (want == 0) break;
            if (!readAligned(h, run.lcn * clusterSize + done, chunk.data(), want)) break;
            for (uint64_t o = 0; o + recLen <= want; o += recLen) {
                uint8_t* rec = chunk.data() + (size_t)o;
                if (!applyUsaFixups(rec, recLen)) continue;
                if (harvestRecordNames(rec, recLen, h2)) {
                    Node node{ h2.parent, h2.isDir ? FILE_ATTRIBUTE_DIRECTORY : 0u, {} };
                    node.names = std::move(h2.names);
                    vol.nodes.emplace(g_recordCounter + o / recLen, std::move(node));
                }
            }
            g_recordCounter += want / recLen;
            done += want;
        }
    }
    return !vol.nodes.empty();
}

// --------------------------------------------------------- USN journal watch

/* Parsed view of one USN record, tolerant of the two field layouts seen in
   the wild: classic V2 (FRN@12, parent@20, names@60/62) and the V4-shaped
   records current Win11 NTFS returns (FRN@8, parent@16, names@56/58,
   FileNameOffset always 60). */
struct ParsedRecord {
    uint64_t frn, parent;
    uint32_t reason, attrs;
    const wchar_t* name;
    size_t nameLen;
};

static bool parseRecord(const BYTE* p, const BYTE* end, ParsedRecord& out) {
    if (p + 60 > end) return false;
    uint32_t rl = *(const uint32_t*)p;
    if (rl < 60 || p + rl > end) return false;
    uint16_t noff = *(const uint16_t*)(p + 58);
    if (noff == 60) {                          // V4-shaped
        out.frn    = *(const uint64_t*)(p + 8)  & 0xFFFFFFFFFFFFULL;
        out.parent = *(const uint64_t*)(p + 16) & 0xFFFFFFFFFFFFULL;
        out.reason = *(const uint32_t*)(p + 40);
        out.attrs  = *(const uint32_t*)(p + 52);
        out.nameLen = *(const uint16_t*)(p + 56) / sizeof(wchar_t);
    } else {                                   // classic USN_RECORD_V2
        out.frn    = *(const uint64_t*)(p + 12) & 0xFFFFFFFFFFFFULL;
        out.parent = *(const uint64_t*)(p + 20) & 0xFFFFFFFFFFFFULL;
        out.reason = *(const uint32_t*)(p + 44);
        out.attrs  = *(const uint32_t*)(p + 56);
        out.nameLen = *(const uint16_t*)(p + 60) / sizeof(wchar_t);
        noff = *(const uint16_t*)(p + 62);
    }
    out.name = (const wchar_t*)(p + noff);
    return out.nameLen > 0 && noff + out.nameLen * sizeof(wchar_t) <= rl;
}

struct WatchState { HANDLE h = INVALID_HANDLE_VALUE; size_t volIdx = 0; uint64_t journalId = 0, nextUsn = 0; };
static std::vector<WatchState> g_watch;

static void startWatch() {
    for (size_t vi = 0; vi < g_volumes.size(); vi++) {
        auto& vol = g_volumes[vi];
        std::wstring vp = std::wstring(L"\\\\.\\") + vol.letter + L":";
        HANDLE h = CreateFileW(vp.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        UsnJournalDataV0 jd{}; DWORD br;
        if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof jd, &br, nullptr)) {
            CloseHandle(h); continue;
        }
        g_watch.push_back({ h, vi, jd.UsnJournalID, jd.NextUsn });
    }
}

// Drain pending journal records and apply create/rename/delete to the map.
static void pumpWatch() {
    const size_t bufSz = 1 << 18;
    BYTE* buf = (BYTE*)_aligned_malloc(bufSz, 4096);
    for (auto& w : g_watch) {
        for (;;) {
            ReadUsnJournalDataV0 rq{ w.nextUsn, 0xFFFFFFFFu, 0, 0, 0, w.journalId };
            DWORD br = 0;
            if (!DeviceIoControl(w.h, FSCTL_READ_USN_JOURNAL, &rq, sizeof rq,
                                 buf, (DWORD)bufSz, &br, nullptr)) break;
            if (br < 16) break;
            w.nextUsn = *(uint64_t*)buf;
            BYTE* p = buf + 8; BYTE* end = buf + br;
            while (p + 8 <= end) {
                uint32_t rl = *(uint32_t*)p;
                if (!rl || p + rl > end) break;
                ParsedRecord r;
                if (parseRecord(p, end, r)) {
                    VolumeIndex& vol = g_volumes[w.volIdx];
                    if (r.reason & (USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME)) {
                        if (vol.nodes.erase(r.frn)) g_dirty = true;
                    }
                    if (r.reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME)) {
                        Node& node = vol.nodes[r.frn];
                        node.parent = r.parent;
                        node.attrs = r.attrs;
                        node.names.clear();
                        node.names.emplace_back(r.name, r.nameLen);
                        g_dirty = true;
                    }
                }
                p += rl;
            }
            if (br <= 8) break;
        }
    }
    _aligned_free(buf);
}

// Portable fallback: recursive directory walk (no admin needed, much slower).
static void walkDir(const std::wstring& dir, VolumeIndex& vol, uint64_t parentFrn) {
    static uint64_t fakeFrn = 0x100000000ULL;           // synthetic FRNs
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        uint64_t frn = fakeFrn++;
        Node& node = vol.nodes[frn];
        node.parent = parentFrn;
        node.attrs  = fd.dwFileAttributes;
        node.names.emplace_back(fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            walkDir(dir + fd.cFileName + L"\\", vol, frn);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ------------------------------------------------------------ path building

static std::unordered_map<uint64_t, std::wstring> g_pathCache;
static const std::wstring& parentPath(VolumeIndex& vol, uint64_t frn) {
    auto it = g_pathCache.find(frn);
    if (it != g_pathCache.end()) return it->second;
    std::vector<uint64_t> chain;
    uint64_t f = frn;
    while (f != ROOT_FRN) {
        auto n = vol.nodes.find(f);
        if (n == vol.nodes.end()) break;                // orphan: unknown parent
        chain.push_back(f);
        f = n->second.parent;
        if (chain.size() > 512) break;
    }
    std::wstring acc;
    if (f == ROOT_FRN) {
        acc = std::wstring(1, vol.letter) + L":";
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
            Node& n = vol.nodes[*rit];
            acc += L"\\";
            acc += n.names.empty() ? L"?" : n.names[0];
            g_pathCache[*rit] = acc;
        }
    }
    return g_pathCache.emplace(frn, acc).first->second;
}

// Rebuild the flat sorted index from the node maps.
static void rebuildFlatIndex() {
    g_hits.clear();
    g_pathCache.clear();
    g_totalNames = 0;
    for (auto& vol : g_volumes) {
        if (vol.nodes.empty()) continue;
        std::wstring rootName = std::wstring(1, vol.letter) + L":";
        if (vol.nodes.find(ROOT_FRN) == vol.nodes.end()) {
            Node root{ 0, FILE_ATTRIBUTE_DIRECTORY, { rootName } };
            vol.nodes[ROOT_FRN] = root;
        } else {
            vol.nodes[ROOT_FRN].names[0] = rootName;
        }
        g_hits.push_back({ rootName, toLower(rootName) });
        g_totalNames++;
        for (auto& kv : vol.nodes) {
            if (kv.first == ROOT_FRN) continue;
            for (auto& name : kv.second.names) {
                const std::wstring& base = parentPath(vol, kv.second.parent);
                if (base.empty()) break;                // orphaned chain
                std::wstring full = base + L"\\" + name;
                g_hits.push_back({ full, toLower(full) });
                g_totalNames++;
            }
        }
    }
    std::sort(g_hits.begin(), g_hits.end(),
              [](const Hit& a, const Hit& b) { return a.lower < b.lower; });
    g_dirty = false;
}

// ------------------------------------------------------------------ cache IO

static std::wstring cacheFile() {
    const char* la = getenv("LOCALAPPDATA");
    std::string base = la ? la : ".";
    std::string s = base + "\\fastfind-index.bin";
    wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, MAX_PATH);
    return w;
}

static bool saveCache() {
    std::ofstream f(cacheFile().c_str(), std::ios::binary);
    if (!f) return false;
    const char magic[8] = { 'F','F','I','D','X','0','0','2' };
    f.write(magic, 8);
    uint32_t nv = (uint32_t)g_volumes.size();
    f.write((char*)&nv, 4);
    for (auto& vol : g_volumes) {
        f.write((char*)&vol.serial, 4);
        uint32_t n = (uint32_t)vol.nodes.size();
        f.write((char*)&n, 4);
        for (auto& kv : vol.nodes) {
            uint64_t frn = kv.first, par = kv.second.parent;
            uint32_t at = kv.second.attrs;
            f.write((char*)&frn, 8); f.write((char*)&par, 8); f.write((char*)&at, 4);
            uint16_t cnt = (uint16_t)kv.second.names.size();
            f.write((char*)&cnt, 2);
            for (auto& nm : kv.second.names) {
                uint16_t len = (uint16_t)nm.size();
                f.write((char*)&len, 2);
                f.write((char*)nm.c_str(), len * sizeof(wchar_t));
            }
        }
    }
    return (bool)f;
}

static bool loadCache(const std::vector<std::pair<wchar_t, DWORD>>& live) {
    std::ifstream f(cacheFile().c_str(), std::ios::binary);
    if (!f) return false;
    char magic[8]; f.read(magic, 8);
    if (memcmp(magic, "FFIDX002", 8) != 0) return false;
    uint32_t nv; f.read((char*)&nv, 4);
    if (nv != live.size()) return false;
    g_volumes.clear();
    for (uint32_t i = 0; i < nv; i++) {
        DWORD serial; f.read((char*)&serial, 4);
        if (serial != live[i].second) return false;     // volume changed
        VolumeIndex vol; vol.letter = live[i].first; vol.serial = serial;
        uint32_t n; f.read((char*)&n, 4);
        for (uint32_t k = 0; k < n; k++) {
            uint64_t frn, par; uint32_t at; uint16_t cnt;
            f.read((char*)&frn, 8); f.read((char*)&par, 8); f.read((char*)&at, 4); f.read((char*)&cnt, 2);
            Node node{ par, at, {} };
            for (uint16_t j = 0; j < cnt; j++) {
                uint16_t len; f.read((char*)&len, 2);
                std::wstring nm(len, L'\0');
                f.read((char*)&nm[0], len * sizeof(wchar_t));
                node.names.push_back(std::move(nm));
            }
            vol.nodes.emplace(frn, std::move(node));
        }
        g_volumes.push_back(std::move(vol));
    }
    return (bool)f;
}

// --------------------------------------------------------------- index build

static wchar_t g_onlyDrive = 0;                        // optional single-volume filter

static std::vector<std::pair<wchar_t, DWORD>> ntfsDrives() {
    std::vector<std::pair<wchar_t, DWORD>> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t fs[64]; DWORD serial = 0;
        if (!GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, fs, 64)) continue;
        if (_wcsicmp(fs, L"NTFS") == 0 && (!g_onlyDrive || g_onlyDrive == (wchar_t)(L'A' + i)))
            out.emplace_back((wchar_t)(L'A' + i), serial);
    }
    return out;
}

using clk = std::chrono::steady_clock;

static void buildIndex(bool fresh) {
    auto t0 = clk::now();
    auto drives = ntfsDrives();
    printf("NTFS volumes:");
    for (auto& d : drives) printf(" %c:", d.first);
    printf("\n");

    if (!fresh && loadCache(drives)) {
        rebuildFlatIndex();
        printf("loaded cache: %llu names in %lld ms\n",
               (unsigned long long)g_totalNames,
               (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count());
        return;
    }

    g_volumes.clear();
    bool anyMft = false, anyFallback = false;
    for (auto& d : drives) {
        VolumeIndex vol; vol.letter = d.first; vol.serial = d.second;
        std::wstring vp = std::wstring(L"\\\\.\\") + d.first + L":";
        HANDLE h = CreateFileW(vp.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
        bool ok = false;
        auto tv = clk::now();
        if (h != INVALID_HANDLE_VALUE) {
            ok = readVolumeMftDirect(h, vol);
            if (!ok) printf("%c: raw $MFT read failed (err %lu), falling back to directory walk\n",
                            (char)d.first, GetLastError());
            CloseHandle(h);
        } else {
            printf("%c: volume open failed (err %lu, admin?), falling back to directory walk\n",
                   (char)d.first, GetLastError());
        }
        if (ok) {
            anyMft = true;
            printf("%c: $MFT scan %zu records in %lld ms\n", (char)d.first, vol.nodes.size(),
                   (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - tv).count());
        }
        else {
            anyFallback = true;
            walkDir(std::wstring(1, d.first) + L":\\", vol, ROOT_FRN);
        }
        g_volumes.push_back(std::move(vol));
    }
    rebuildFlatIndex();
    saveCache();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
    printf("indexed %llu names from %zu volume(s) in %lld ms (%s)\n",
           (unsigned long long)g_totalNames, g_volumes.size(), ms,
           anyMft && !anyFallback ? "raw $MFT scan" : "directory-walk fallback");
    if (anyMft) printf("cache saved: %ls\n", cacheFile().c_str());
}

// ------------------------------------------------------------------ search

static void search(const std::string& utf8Query, size_t limit) {
    if (g_dirty) {
        auto t = clk::now();
        rebuildFlatIndex();
        printf("[index updated in %lld ms]\n",
               (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t).count());
    }
    std::vector<std::wstring> tokens;
    {
        std::wstring wq(4096, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8Query.c_str(), -1, &wq[0], 4096);
        std::wstring lower = toLower(wq.c_str());
        wchar_t* ctx = nullptr;
        for (wchar_t* t = wcstok(&lower[0], L" \t", &ctx); t; t = wcstok(nullptr, L" \t", &ctx))
            tokens.push_back(t);
    }
    if (tokens.empty()) return;
    auto t0 = clk::now();
    size_t found = 0;
    for (auto& hit : g_hits) {
        bool all = true;
        for (auto& tk : tokens)
            if (hit.lower.find(tk) == std::wstring::npos) { all = false; break; }
        if (!all) continue;
        printf("%ls\n", hit.path.c_str());
        if (++found >= limit) { printf("...\n"); break; }
    }
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t0).count();
    printf("%zu result(s) (limit %zu) in %lld.%03lld ms\n",
           found, limit, us / 1000, us % 1000);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    bool fresh = false, interactive = false, watch = false, buildOnly = false;
    std::vector<std::string> words;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--build") buildOnly = true;
        else if (a == "--fresh") fresh = true;
        else if (a == "--drive" && i + 1 < argc) g_onlyDrive = toupper(argv[++i][0]);
        else if (a.rfind("--drive=", 0) == 0) g_onlyDrive = toupper(a[8]);
        else if (a == "-i" || a == "--interactive") interactive = true;
        else if (a == "--watch") interactive = true, watch = true;
        else words.push_back(a);
    }

    if (words.empty() && !interactive && !buildOnly) {
        printf("fastfind — Everything-style instant search (raw $MFT index)\n\n"
               "  fastfind --build [--fresh]   build the index cache\n"
               "  fastfind <words...>          search (all words must match)\n"
               "  fastfind -i [--watch]        interactive (live updates with --watch)\n");
        return 0;
    }

    buildIndex(fresh);
    if (buildOnly) return 0;
    if (watch) startWatch();

    if (!interactive) {
        std::string q;
        for (size_t i = 0; i < words.size(); i++) { if (i) q += " "; q += words[i]; }
        search(q, 1000);
        return 0;
    }

    printf("interactive search — type words ('q' quits)%s\n",
           watch ? ", USN journal watch on" : "");
    std::string line;
    while (true) {
        printf("> "); fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        if (line == "q" || line == "quit") break;
        if (line.empty()) continue;
        if (watch) pumpWatch();
        search(line, 100);
    }
    return 0;
}
