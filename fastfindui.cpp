/*
 * fastfindui — Everything-style GUI file-name search for Windows.
 * (Console sibling: fastfind.cpp — same $MFT core without the UI.)
 *
 * UI: search box on top, virtual list view (name/folder/size/date/type)
 * below, status bar. Index building and USN watching run on background
 * threads so the window stays responsive; files created/renamed while the
 * app runs appear in results within seconds.
 *
 * Tray mode: closing the window hides it to the system tray — the index
 * stays in memory and the USN watch keeps it current, so reopening is
 * instant (no rescan). Left-click the tray icon to restore the window,
 * right-click for the menu (show / rebuild index / exit). A single-instance
 * guard prevents a second copy from doubling memory.
 *
 * Build: windres fastfindui.rc fastfindui_rc.o
 *        g++ -O2 -std=c++17 -municode -mwindows fastfindui.cpp fastfindui_rc.o \
 *            -o fastfindui.exe -lcomctl32 -lshell32 -lcomdlg32 -ladvapi32 -pthread
 * The .rc embeds fastfind.ico and a comctl32 v6 manifest (themed controls,
 * header sort arrows). Set FASTFINDUI_LOG=<file> to append a diagnostic log.
 */

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>
#include <commdlg.h>
#include <lmcons.h>
#include <cwctype>
#include <cstdio>
#include "version.h"
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>

using namespace std;

// ---------------------------------------------------------------- FSCTL data

#ifndef FSCTL_READ_USN_JOURNAL
#define FSCTL_READ_USN_JOURNAL 0x000900bb
#endif
#ifndef FSCTL_QUERY_USN_JOURNAL
#define FSCTL_QUERY_USN_JOURNAL 0x000900f4
#endif
#ifndef FSCTL_CREATE_USN_JOURNAL
#define FSCTL_CREATE_USN_JOURNAL 0x000900e7
#endif
#ifndef USN_REASON_FILE_CREATE
#define USN_REASON_FILE_CREATE     0x00000100
#endif
#ifndef USN_REASON_FILE_DELETE
#define USN_REASON_FILE_DELETE     0x00000200
#endif
#ifndef USN_REASON_RENAME_OLD_NAME
#define USN_REASON_RENAME_OLD_NAME 0x00001000
#endif
#ifndef USN_REASON_RENAME_NEW_NAME
#define USN_REASON_RENAME_NEW_NAME 0x00002000
#endif

#pragma pack(push, 1)
struct ReadUsnJournalDataV0 {
    uint64_t StartUsn; uint32_t ReasonMask; uint32_t ReturnOnlyOnClose;
    uint64_t Timeout; uint64_t BytesToWaitFor; uint64_t UsnJournalID;
};
struct UsnJournalDataV0 {
    uint64_t UsnJournalID, FirstUsn, NextUsn, LowestValidUsn, MaxUsn, MaximumSize, AllocationDelta;
};
struct CreateUsnJournalData { uint64_t MaximumSize, AllocationDelta; };
struct NtfsFileRecordHeader {
    uint32_t signature; uint16_t usaOffset; uint16_t usaCount; uint64_t usn;
    uint16_t sequenceNumber; uint16_t referenceCount;
    uint16_t firstAttributeOffset; uint16_t flags;
    uint32_t bytesInUse; uint32_t bytesAllocated;
};
struct NtfsAttributeHeader {
    uint32_t type; uint32_t length; uint8_t nonResident; uint8_t nameLength;
    uint16_t nameOffset; uint16_t flagsAttr; uint16_t attributeId;
};
struct NtfsFileNameAttribute {
    uint64_t parentFrn;
    uint64_t creationTime, modificationTime, mftModificationTime, readTime;
    uint64_t allocatedSize, realSize;
    uint32_t flagsFn; uint32_t reparse;
    uint8_t nameLength; uint8_t namespaceId;
};
#pragma pack(pop)

static const uint64_t ROOT_FRN = 5;

// ---------------------------------------------------------------- index data

struct Node {
    uint64_t parent;
    uint32_t attrs;
    uint64_t size = 0;
    int64_t  mtime = 0;
    vector<wstring> names;
};

struct VolumeIndex {
    wchar_t letter = 0;
    DWORD serial = 0;
    bool fromMft = false;
    unordered_map<uint64_t, Node> nodes;
};

struct Hit {
    wstring path;
    wstring lower;
    uint64_t frnKey = 0;    // (volIdx<<56)|frn
    uint64_t size = 0;
    int64_t  mtime = 0;
    bool     isDir = false;
};

struct LiveSnap {                       // USN deltas since the last rebuild
    vector<Hit> recent;                 // created/renamed-in entries
    unordered_set<uint64_t> dead;       // removed frnKeys
};

static vector<VolumeIndex> g_volumes;                 // node maps
static mutex g_csData;                                // guards g_volumes
static shared_ptr<const vector<Hit>> g_master;        // full index snapshot
static shared_ptr<const LiveSnap> g_live;
static mutex g_csMaster, g_csLive;
static atomic<bool> g_ready{false}, g_rebuilding{false}, g_quit{false}, g_dirtyLive{false};
/* index startup phase for the status bar: 0 = loading from the saved index,
   1 = full $MFT scan; g_replaying counts volumes still catching up on the
   changes that happened while the program was closed */
static atomic<int> g_idxPhase{0};
static atomic<int> g_replaying{0};
static atomic<uint64_t> g_objectCount{0};

static FILE* g_log = nullptr;
static void logf_(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(ap);
}

static wstring toLowerW(wstring s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c += 32;
    return s;
}

// ------------------------------------------------------- direct $MFT reading

static bool readAligned(HANDLE h, uint64_t off, void* dst, uint32_t bytes) {
    LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    while (bytes) {
        if (!ReadFile(h, dst, bytes, &got, nullptr) || !got) return false;
        dst = (char*)dst + got; bytes -= got;
    }
    return true;
}

static bool applyUsaFixups(uint8_t* rec, uint32_t recLen) {
    NtfsFileRecordHeader* hdr = (NtfsFileRecordHeader*)rec;
    if (hdr->signature != 0x454C4946) return false;
    uint16_t usaOff = hdr->usaOffset, usaCnt = hdr->usaCount;
    if (usaOff < 48 || !usaCnt || (uint32_t)usaOff + (uint32_t)usaCnt * 2u > recLen) return false;
    const uint16_t* usa = (const uint16_t*)(rec + usaOff);
    if ((uint32_t)(usaCnt - 1) != recLen / 512) return false;
    for (uint32_t s = 0; s + 1 < usaCnt; s++) {
        uint16_t* tail = (uint16_t*)(rec + (s + 1) * 512 - 2);
        if (*tail != usa[0]) return false;
        *tail = usa[s + 1];
    }
    return true;
}

struct Harvested { uint64_t parent; bool isDir; uint64_t size; int64_t mtime; vector<wstring> names; };

static bool harvestRecordNames(uint8_t* rec, uint32_t recLen, Harvested& out) {
    NtfsFileRecordHeader* hdr = (NtfsFileRecordHeader*)rec;
    if (!(hdr->flags & 1)) return false;
    /* directory flag lives in the record header (bit 1); attribute type
       0x10 is $STANDARD_INFORMATION, which EVERY record has */
    out.parent = 0; out.isDir = (hdr->flags & 0x02) != 0;
    out.size = 0; out.mtime = 0; out.names.clear();
    uint32_t off = hdr->firstAttributeOffset;
    for (int guard = 0; guard < 64; guard++) {
        if (off + 0x18 > recLen) break;
        NtfsAttributeHeader* at = (NtfsAttributeHeader*)(rec + off);
        if (at->type == 0xFFFFFFFF) break;
        if (at->length < 0x18 || off + at->length > recLen) break;
        if (at->type == 0x30 && !at->nonResident && !at->nameLength) {
            uint16_t valueOff; memcpy(&valueOff, rec + off + 0x14, 2);
            uint32_t attrEnd = off + at->length;
            if (valueOff >= 0x18 && off + valueOff + sizeof(NtfsFileNameAttribute) <= attrEnd) {
                NtfsFileNameAttribute* fn = (NtfsFileNameAttribute*)(rec + off + valueOff);
                if (fn->namespaceId == 2) { off += at->length; continue; }
                const wchar_t* name = (const wchar_t*)((uint8_t*)fn + sizeof(NtfsFileNameAttribute));
                size_t nlen = fn->nameLength;
                if (nlen && (uint8_t*)name + nlen * 2 <= rec + attrEnd) {
                    out.parent = fn->parentFrn & 0xFFFFFFFFFFFFULL;
                    out.isDir = out.isDir || (fn->flagsFn & 0x10000000);
                    out.size = fn->realSize; out.mtime = fn->modificationTime;
                    out.names.emplace_back(name, nlen);
                }
            }
        }
        off += at->length;
    }
    return !out.names.empty();
}

struct DataRun { uint64_t lcn, clusters; };

static bool parseDataRuns(const uint8_t* attr, uint32_t attrLen, vector<DataRun>& runs) {
    uint16_t runOff; memcpy(&runOff, attr + 0x20, 2);
    if (!runOff || runOff >= attrLen) return false;
    const uint8_t* p = attr + runOff, * end = attr + attrLen;
    uint64_t prev = 0;
    while (p < end && *p) {
        uint8_t hb = *p++;
        int offBytes = hb >> 4, lenBytes = hb & 0xF;   // high=offset size, low=length size
        if (!lenBytes || lenBytes > 8 || offBytes > 8) return false;
        if (p + lenBytes + offBytes > end) return false;
        uint64_t clusters = 0; memcpy(&clusters, p, lenBytes); p += lenBytes;
        int64_t delta = 0;
        if (offBytes) {
            memcpy(&delta, p, offBytes); p += offBytes;
            if (offBytes < 8 && (p[-1] & 0x80)) delta -= (1LL << (offBytes * 8));
        }
        prev = (uint64_t)((int64_t)prev + delta);
        runs.push_back({ prev, clusters });
    }
    return !runs.empty();
}

static bool readVolumeMftDirect(HANDLE h, VolumeIndex& vol) {
    uint8_t bs[512];
    if (!readAligned(h, 0, bs, sizeof bs) || memcmp(bs + 3, "NTFS", 4)) return false;
    uint16_t bps; memcpy(&bps, bs + 0x0B, 2);
    uint8_t spc = bs[0x0D];
    uint64_t mftLcn; memcpy(&mftLcn, bs + 0x30, 8);
    int8_t cpr = (int8_t)bs[0x40];
    uint32_t clusterSize = (uint32_t)bps * spc;
    uint32_t recLen = cpr < 0 ? (1u << -cpr) : (uint32_t)cpr * clusterSize;
    if (bps != 512 || recLen < 256 || recLen > 8192) return false;

    vector<uint8_t> rec0(recLen);
    if (!readAligned(h, mftLcn * clusterSize, rec0.data(), recLen)) return false;
    if (!applyUsaFixups(rec0.data(), recLen)) return false;
    vector<DataRun> runs;
    {
        auto* hdr = (NtfsFileRecordHeader*)rec0.data();
        uint32_t off = hdr->firstAttributeOffset;
        for (int guard = 0; guard < 64; guard++) {
            if (off + 0x18 > recLen) break;
            auto* at = (NtfsAttributeHeader*)(rec0.data() + off);
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

    const uint32_t chunkBytes = 8 << 20;
    vector<uint8_t> chunk(chunkBytes);
    Harvested h2;
    uint64_t recNo = 0;
    for (auto& run : runs) {
        uint64_t runBytes = run.clusters * clusterSize;
        for (uint64_t done = 0; done < runBytes;) {
            uint32_t want = (uint32_t)min<uint64_t>({ (uint64_t)chunkBytes, runBytes - done });
            want -= want % recLen;
            if (!want) break;
            if (!readAligned(h, run.lcn * clusterSize + done, chunk.data(), want)) return false;
            for (uint64_t o = 0; o + recLen <= want; o += recLen) {
                uint8_t* rec = chunk.data() + (size_t)o;
                if (applyUsaFixups(rec, recLen) && harvestRecordNames(rec, recLen, h2)) {
                    Node node{ h2.parent, h2.isDir ? FILE_ATTRIBUTE_DIRECTORY : 0u,
                               h2.size, h2.mtime, {} };
                    node.names = move(h2.names);
                    vol.nodes.emplace(recNo + o / recLen, move(node));
                }
            }
            recNo += want / recLen;
            done += want;
        }
    }
    return !vol.nodes.empty();
}

static void walkDir(const wstring& dir, VolumeIndex& vol, uint64_t parentFrn) {
    static uint64_t fakeFrn = 0x100000000ULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        uint64_t frn = fakeFrn++;
        Node& node = vol.nodes[frn];
        node.parent = parentFrn;
        node.attrs = fd.dwFileAttributes;
        ULARGE_INTEGER sz = { fd.nFileSizeLow, (DWORD)fd.nFileSizeHigh };
        node.size = sz.QuadPart;
        node.mtime = ((int64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        node.names.emplace_back(fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            walkDir(dir + fd.cFileName + L"\\", vol, frn);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ------------------------------------------------------------- path building

static wstring resolveFullPath(VolumeIndex& vol, uint64_t parentFrn,
                               unordered_map<uint64_t, wstring>* cache) {
    vector<uint64_t> chain;
    uint64_t f = parentFrn;
    while (f != ROOT_FRN) {
        if (cache) {
            auto it = cache->find(f);
            if (it != cache->end()) {
                wstring acc = it->second;
                for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
                    Node& n = vol.nodes[*rit];
                    acc += L"\\"; acc += n.names.empty() ? L"?" : n.names[0];
                    (*cache)[*rit] = acc;
                }
                return acc;
            }
        }
        auto n = vol.nodes.find(f);
        if (n == vol.nodes.end()) break;
        chain.push_back(f);
        f = n->second.parent;
        if (chain.size() > 512) break;
    }
    if (f != ROOT_FRN) return L"";
    wstring acc(1, vol.letter); acc += L":";
    for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
        Node& n = vol.nodes[*rit];
        acc += L"\\"; acc += n.names.empty() ? L"?" : n.names[0];
        if (cache) (*cache)[*rit] = acc;
    }
    return acc;
}

static vector<Hit> buildMasterLocked(vector<VolumeIndex>& vols) {
    vector<Hit> hits;
    for (size_t vi = 0; vi < vols.size(); vi++) {
        auto& vol = vols[vi];
        if (vol.nodes.empty()) continue;
        wstring rootName(1, vol.letter); rootName += L":";
        if (!vol.nodes.count(ROOT_FRN)) {
            Node root{ 0, FILE_ATTRIBUTE_DIRECTORY, 0, 0, { rootName } };
            vol.nodes[ROOT_FRN] = root;
        } else {
            vol.nodes[ROOT_FRN].names[0] = rootName;
        }
        hits.push_back({ rootName, toLowerW(rootName), (uint64_t)vi << 56 | ROOT_FRN, 0, 0, true });
        unordered_map<uint64_t, wstring> cache;
        for (auto& kv : vol.nodes) {
            if (kv.first == ROOT_FRN) continue;
            const wstring base = resolveFullPath(vol, kv.second.parent, &cache);
            if (base.empty()) continue;
            for (auto& name : kv.second.names) {
                wstring full = base + L"\\" + name;
                Hit h{ move(full), wstring(), (uint64_t)vi << 56 | kv.first,
                       kv.second.size, kv.second.mtime,
                       (kv.second.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 };
                h.lower = toLowerW(h.path);
                hits.push_back(move(h));
            }
        }
    }
    return hits;
}

// ------------------------------------------------------------------ cache IO

/* per-volume USN journal position, persisted in the cache: the next start
   replays everything that changed while the program was not running.
   g_usnPos is advanced only AFTER records are applied to the node maps, so a
   saved position can only lag the saved nodes (safe: replays are idempotent),
   never lead them (which would silently drop changes). */
struct UsnPos { uint64_t journalId = 0, nextUsn = 0; bool valid = false; };
static vector<UsnPos> g_usnPos;         // indexed like g_volumes
static mutex g_csUsnPos;

static wstring cacheFile() {
    const char* la = getenv("LOCALAPPDATA");
    string s = (la ? la : ".");
    s += "\\fastfind-ui-index.bin";
    wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, MAX_PATH);
    return w;
}

static bool saveCache() {
    /* write to a temp file and atomically replace: a crash or kill mid-save
       must never leave a truncated cache (the previous good one survives) */
    wstring finalPath = cacheFile();
    wstring tmpPath = finalPath + L".tmp";
    FILE* f = _wfopen(tmpPath.c_str(), L"wb");
    if (!f) return false;
    fwrite("FFIDX005", 1, 8, f);
    uint32_t nv = (uint32_t)g_volumes.size();
    fwrite(&nv, 4, 1, f);
    vector<UsnPos> pos;
    { lock_guard<mutex> lk(g_csUsnPos); pos = g_usnPos; }
    pos.resize(max(pos.size(), (size_t)nv), UsnPos{});
    lock_guard<mutex> lk(g_csData);
    for (auto& vol : g_volumes) {
        fwrite(&vol.serial, 4, 1, f);
        uint32_t n = (uint32_t)vol.nodes.size();
        fwrite(&n, 4, 1, f);
        for (auto& kv : vol.nodes) {
            fwrite(&kv.first, 8, 1, f); fwrite(&kv.second.parent, 8, 1, f);
            fwrite(&kv.second.attrs, 4, 1, f);
            fwrite(&kv.second.size, 8, 1, f); fwrite(&kv.second.mtime, 8, 1, f);
            uint16_t cnt = (uint16_t)kv.second.names.size();
            fwrite(&cnt, 2, 1, f);
            for (auto& nm : kv.second.names) {
                uint16_t len = (uint16_t)nm.size();
                fwrite(&len, 2, 1, f);
                fwrite(nm.c_str(), 2, len, f);
            }
        }
        uint64_t jid = 0, un = 0;
        size_t vi = (size_t)(&vol - &g_volumes[0]);
        if (vi < pos.size() && pos[vi].valid) { jid = pos[vi].journalId; un = pos[vi].nextUsn; }
        fwrite(&jid, 8, 1, f);
        fwrite(&un, 8, 1, f);
    }
    fclose(f);
    MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    return true;
}

static bool loadCache(const vector<pair<wchar_t, DWORD>>& live) {
    FILE* f = _wfopen(cacheFile().c_str(), L"rb");
    if (!f) { logf_("CACHE: no file"); return false; }
    char magic[8]; if (fread(magic, 1, 8, f) != 8) { fclose(f); logf_("CACHE: short header"); return false; }
    if (memcmp(magic, "FFIDX005", 8)) {
        magic[7] = 0; logf_("CACHE: magic mismatch (%s)", magic);
        fclose(f); return false; }   // v3 has no USN positions
    uint32_t nv; if (fread(&nv, 4, 1, f) != 1 || nv != live.size()) {
        fclose(f); logf_("CACHE: volume count %u != %zu", nv, live.size()); return false; }
    vector<VolumeIndex> vols;
    vector<UsnPos> pos;
    for (uint32_t i = 0; i < nv; i++) {
        DWORD serial; if (fread(&serial, 4, 1, f) != 1) break;
        if (serial != live[i].second) {
            fclose(f); logf_("CACHE: volume %u serial mismatch", i); return false;
        }
        VolumeIndex vol; vol.letter = live[i].first; vol.serial = serial; vol.fromMft = true;
        uint32_t n; if (fread(&n, 4, 1, f) != 1) { fclose(f); logf_("CACHE: bad node count"); return false; }
        for (uint32_t k = 0; k < n; k++) {
            uint64_t frn, par, size; int64_t mtime; uint32_t at; uint16_t cnt;
            if (fread(&frn, 8, 1, f) != 1) { fclose(f); logf_("CACHE: bad record header"); return false; }
            if (fread(&par, 8, 1, f) != 1 || fread(&at, 4, 1, f) != 1 ||
                fread(&size, 8, 1, f) != 1 || fread(&mtime, 8, 1, f) != 1 ||
                fread(&cnt, 2, 1, f) != 1) { fclose(f); logf_("CACHE: bad record fields"); return false; }
            Node node{ par, at, size, mtime, {} };
            for (uint16_t j = 0; j < cnt; j++) {
                uint16_t len; if (fread(&len, 2, 1, f) != 1) { fclose(f); logf_("CACHE: bad name length"); return false; }
                wstring nm(len, L'\0');
                if (len && fread(&nm[0], 2, len, f) != (size_t)len) { fclose(f); logf_("CACHE: bad name data"); return false; }
                node.names.push_back(move(nm));
            }
            vol.nodes.emplace(frn, move(node));
        }
        uint64_t jid = 0, un = 0;
        if (fread(&jid, 8, 1, f) != 1 || fread(&un, 8, 1, f) != 1) { fclose(f); logf_("CACHE: bad usn position"); return false; }
        pos.push_back(UsnPos{ jid, un, jid != 0 });
        vols.push_back(move(vol));
    }
    fclose(f);
    if (vols.size() != live.size()) { logf_("CACHE: incomplete load"); return false; }
    logf_("CACHE: loaded %zu volumes", vols.size());
    lock_guard<mutex> lk(g_csData);
    g_volumes = move(vols);
    { lock_guard<mutex> lu(g_csUsnPos); g_usnPos = move(pos); }
    return true;
}

static vector<pair<wchar_t, DWORD>> ntfsDrives() {
    vector<pair<wchar_t, DWORD>> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t fs[64]; DWORD serial = 0;
        if (!GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, fs, 64)) continue;
        if (!_wcsicmp(fs, L"NTFS")) out.emplace_back((wchar_t)(L'A' + i), serial);
    }
    return out;
}

/* current journal identity + read position of a volume (creates the journal
   when the volume has none); used to baseline positions before a scan */
static bool queryJournalPos(wchar_t letter, uint64_t& journalId, uint64_t& nextUsn) {
    wstring vp = wstring(L"\\\\.\\") + letter + L":";
    HANDLE h = CreateFileW(vp.c_str(), GENERIC_READ, 7, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    UsnJournalDataV0 jd{}; DWORD br;
    BOOL ok = DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof jd, &br, nullptr);
    if (!ok) {
        CreateUsnJournalData cj{ 32ULL << 20, 8ULL << 20 };
        ok = DeviceIoControl(h, FSCTL_CREATE_USN_JOURNAL, &cj, sizeof cj, nullptr, 0, &br, nullptr) &&
             DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof jd, &br, nullptr);
    }
    CloseHandle(h);
    if (!ok) return false;
    journalId = jd.UsnJournalID;
    nextUsn = jd.NextUsn;
    return true;
}

// ------------------------------------------------------------ USN watch side

struct UsnUpdate {
    size_t volIdx; uint64_t frn, parent; uint32_t reason, attrs; int64_t ts; wstring name;
    uint64_t usn = 0;                    // journal position AFTER the batch this record came from
};
static vector<UsnUpdate> g_pend;
static mutex g_csPend;

struct ParsedRecord {
    uint64_t frn, parent; uint32_t reason, attrs; int64_t ts;
    const wchar_t* name; size_t nameLen;
};
static bool parseRecord(const BYTE* p, const BYTE* end, ParsedRecord& out) {
    if (p + 60 > end) return false;
    uint32_t rl = *(const uint32_t*)p;
    if (rl < 60 || p + rl > end) return false;
    uint16_t noff = *(const uint16_t*)(p + 58);
    if (noff == 60) {                                    // V4-shaped (current Win11)
        out.frn    = *(const uint64_t*)(p + 8)  & 0xFFFFFFFFFFFFULL;
        out.parent = *(const uint64_t*)(p + 16) & 0xFFFFFFFFFFFFULL;
        out.reason = *(const uint32_t*)(p + 40);
        out.attrs  = *(const uint32_t*)(p + 52);
        out.ts     = *(const int64_t*)(p + 32);
        out.nameLen = *(const uint16_t*)(p + 56) / 2;
    } else {                                             // classic USN_RECORD_V2
        out.frn    = *(const uint64_t*)(p + 12) & 0xFFFFFFFFFFFFULL;
        out.parent = *(const uint64_t*)(p + 20) & 0xFFFFFFFFFFFFULL;
        out.reason = *(const uint32_t*)(p + 44);
        out.attrs  = *(const uint32_t*)(p + 56);
        out.ts     = *(const int64_t*)(p + 36);
        out.nameLen = *(const uint16_t*)(p + 60) / 2;
        noff = *(const uint16_t*)(p + 62);
    }
    out.name = (const wchar_t*)(p + noff);
    return out.nameLen && noff + out.nameLen * 2 <= rl;
}

struct Watch { HANDLE h; size_t volIdx; uint64_t journalId, nextUsn, target; bool catching; };

static void watchThread() {
    vector<Watch> watches;
    {
        lock_guard<mutex> lk(g_csData);
        lock_guard<mutex> lu(g_csUsnPos);
        g_usnPos.resize(g_volumes.size(), UsnPos{});
        for (size_t vi = 0; vi < g_volumes.size(); vi++) {
            if (!g_volumes[vi].fromMft) continue;
            wstring vp = wstring(L"\\\\.\\") + g_volumes[vi].letter + L":";
            HANDLE h = CreateFileW(vp.c_str(), GENERIC_READ, 7, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            UsnJournalDataV0 jd{}; DWORD br;
            if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof jd, &br, nullptr)) {
                CreateUsnJournalData cj{ 32ULL << 20, 8ULL << 20 };
                if (!DeviceIoControl(h, FSCTL_CREATE_USN_JOURNAL, &cj, sizeof cj, nullptr, 0, &br, nullptr) ||
                    !DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof jd, &br, nullptr)) {
                    CloseHandle(h); continue;
                }
                logf_("USN journal created on %c:", (char)g_volumes[vi].letter);
            }
            /* replay from the SAVED position so changes made while the program
               was closed are caught up; otherwise baseline at the current
               position and mark it valid (advancing only by journal reads) */
            uint64_t start = jd.NextUsn;
            bool replay = vi < g_usnPos.size() && g_usnPos[vi].valid &&
                          g_usnPos[vi].journalId == jd.UsnJournalID &&
                          g_usnPos[vi].nextUsn >= jd.LowestValidUsn &&
                          g_usnPos[vi].nextUsn <= jd.NextUsn;
            if (replay) {
                start = g_usnPos[vi].nextUsn;
                logf_("catching up %c: from %#llx to %#llx (%llu bytes of journal)",
                      (char)g_volumes[vi].letter,
                      (unsigned long long)start, (unsigned long long)jd.NextUsn,
                      (unsigned long long)(jd.NextUsn - start));
            }
            watches.push_back({ h, vi, jd.UsnJournalID, start, jd.NextUsn, replay });
            if (replay) g_replaying++;
            if (vi < g_usnPos.size()) {
                if (g_usnPos[vi].valid)
                    g_usnPos[vi].journalId = jd.UsnJournalID;   // keep applied nextUsn
                else
                    g_usnPos[vi] = UsnPos{ jd.UsnJournalID, jd.NextUsn, true };
            }
            logf_("watching %c: journalId=%#llx nextUsn=%#llx",
                  (char)g_volumes[vi].letter, (unsigned long long)jd.UsnJournalID,
                  (unsigned long long)jd.NextUsn);
        }
    }
    const size_t bufSz = 1 << 18;
    BYTE* buf = (BYTE*)_aligned_malloc(bufSz, 4096);
    while (!g_quit.load()) {
        bool any = false;
        for (auto& w : watches) {
            for (;;) {
                ReadUsnJournalDataV0 rq{ w.nextUsn, 0xFFFFFFFFu, 0, 0, 0, w.journalId };
                DWORD br = 0;
                if (!DeviceIoControl(w.h, FSCTL_READ_USN_JOURNAL, &rq, sizeof rq,
                                     buf, (DWORD)bufSz, &br, nullptr)) break;
                if (br >= 8) {
                    w.nextUsn = *(uint64_t*)buf;
                    if (w.catching && w.nextUsn >= w.target) {
                        w.catching = false;      // initial replay of the gap done
                        g_replaying--;
                        logf_("caught up %c:", (char)g_volumes[w.volIdx].letter);
                    }
                }
                if (br < 16) break;
                BYTE* p = buf + 8, * end = buf + br;
                while (p + 8 <= end) {
                    uint32_t rl = *(uint32_t*)p;
                    if (!rl || p + rl > end) break;
                    ParsedRecord r;
                    if (parseRecord(p, end, r)) {
                        lock_guard<mutex> lk(g_csPend);
                        g_pend.push_back({ w.volIdx, r.frn, r.parent, r.reason, r.attrs, r.ts,
                                           wstring(r.name, r.nameLen), w.nextUsn });
                        any = true;
                    }
                    p += rl;
                }
                if (br <= 8) break;
            }
        }
        Sleep(any ? 50 : 250);
    }
    _aligned_free(buf);
    for (auto& w : watches) CloseHandle(w.h);
}

// ------------------------------------------------------------- UI drain/apply

static atomic<bool> g_forceRebuild{false};
static atomic<DWORD> g_lastRebuildTick{0};

static void applyPending() {
    vector<UsnUpdate> ups;
    {
        lock_guard<mutex> lk(g_csPend);
        if (g_pend.empty()) return;
        ups.swap(g_pend);
    }
    struct LiveDelta { bool add; uint64_t key; Hit hit; };
    vector<LiveDelta> deltas;
    deltas.reserve(ups.size());
    {
        /* try_lock: a background rebuild may hold the maps for seconds; the
           UI thread must NEVER wait for it — defer this batch to the next
           drain tick instead of freezing the window */
        unique_lock<mutex> lk(g_csData, try_to_lock);
        if (!lk.owns_lock()) {
            lock_guard<mutex> pb(g_csPend);
            g_pend.insert(g_pend.end(), ups.begin(), ups.end());   // put them back
            return;
        }
        for (auto& u : ups) {
            VolumeIndex& vol = g_volumes[u.volIdx];
            uint64_t key = (uint64_t)u.volIdx << 56 | u.frn;
            if (u.reason & (USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME)) {
                vol.nodes.erase(u.frn);
                deltas.push_back({ false, key, Hit{} });
            }
            if (u.reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME)) {
                wstring base = resolveFullPath(vol, u.parent, nullptr);
                Node node{ u.parent, u.attrs, 0, u.ts, { u.name } };
                vol.nodes[u.frn] = move(node);
                if (!base.empty()) {
                    wstring full = base + L"\\" + u.name;
                    Hit h{ move(full), wstring(), key, 0, u.ts,
                           (u.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 };
                    h.lower = toLowerW(h.path);
                    deltas.push_back({ true, key, move(h) });
                }
            }
        }
    }
    /* advance the persisted per-volume position to the newest CONSUMED record —
       only applied state may advance, so a saved position can only lag the
       saved nodes (safe: replays are idempotent), never lead them (which
       would silently drop changes). Locks are taken one at a time (never
       nested) to keep a fixed order with watchThread. Runs before the empty
       early-return: records with no delta (close/write) still count. */
    {
        size_t nvol;
        { lock_guard<mutex> lk(g_csData); nvol = g_volumes.size(); }
        vector<uint64_t> maxPos(nvol, 0);
        for (auto& u : ups)
            if (u.volIdx < maxPos.size() && u.usn > maxPos[u.volIdx])
                maxPos[u.volIdx] = u.usn;
        lock_guard<mutex> lk(g_csUsnPos);
        if (g_usnPos.size() < nvol) g_usnPos.resize(nvol, UsnPos{});
        for (size_t vi = 0; vi < nvol; vi++)
            if (maxPos[vi]) {
                g_usnPos[vi].nextUsn = maxPos[vi];
                g_usnPos[vi].valid = true;
            }
    }
    if (deltas.empty()) return;
    bool overflow = false;
    {
        /* ONE copy-on-write per drained batch (not per record) — per-record
           copying made the UI stutter while USN churn was active */
        lock_guard<mutex> lv(g_csLive);
        auto live = make_shared<LiveSnap>(*g_live);
        for (auto& d : deltas) {
            live->dead.erase(d.key);            // NTFS reuses record numbers
            if (!d.add) {
                live->dead.insert(d.key);
            } else {
                /* one file creation yields several USN records (create/write/
                   close); update the existing recent entry instead of dupes */
                bool updated = false;
                for (auto& r : live->recent) {
                    if (r.frnKey == d.key) { r = d.hit; updated = true; break; }
                }
                if (!updated) live->recent.push_back(move(d.hit));
            }
        }
        if (live->recent.size() > 20000) {      // cap; the rebuild refills from node maps
            live->recent.erase(live->recent.begin(),
                                live->recent.begin() + (live->recent.size() - 20000));
            overflow = true;
        }
        g_live = move(live);
        g_dirtyLive = true;
    }
    logf_("APPLY %zu records", ups.size());
    if (overflow) g_forceRebuild = true;
}

static void rebuildThread(HWND hwnd) {
    g_rebuilding = true;
    PostMessage(hwnd, WM_APP + 2, 0, 0);
    vector<Hit> fresh;
    {
        lock_guard<mutex> lk(g_csData);
        fresh = buildMasterLocked(g_volumes);
    }
    {
        lock_guard<mutex> lm(g_csMaster);
        g_master = make_shared<const vector<Hit>>(move(fresh));
        lock_guard<mutex> lv(g_csLive);
        g_live = make_shared<const LiveSnap>();
        g_dirtyLive = false;
    }
    g_objectCount = (uint64_t)g_master->size();
    g_rebuilding = false;
    g_lastRebuildTick = GetTickCount();
    PostMessage(hwnd, WM_APP + 2, 0, 0);
}

/* true "rebuild": a fresh $MFT scan of every volume (the menu item), replacing
   the node maps in place so watchThread's volume indexes stay valid. Records
   still pending in g_pend are re-applied afterwards — they may overlap with
   what the fresh scan already contains, which is harmless (idempotent), and
   advancing the saved USN position over them stays consistent. */
static void fullRescanThread(HWND hwnd) {
    g_rebuilding = true;
    PostMessage(hwnd, WM_APP + 2, 0, 0);
    {
        lock_guard<mutex> lk(g_csData);
        for (auto& vol : g_volumes) {
            VolumeIndex fresh; fresh.letter = vol.letter; fresh.serial = vol.serial;
            wstring vp = wstring(L"\\\\.\\") + vol.letter + L":";
            HANDLE h = CreateFileW(vp.c_str(), GENERIC_READ, 7, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                fresh.fromMft = readVolumeMftDirect(h, fresh);
                CloseHandle(h);
            }
            if (!fresh.fromMft)
                walkDir(wstring(1, vol.letter) + L":\\", fresh, ROOT_FRN);
            vol = move(fresh);
        }
    }
    vector<Hit> fresh;
    {
        lock_guard<mutex> lk(g_csData);
        fresh = buildMasterLocked(g_volumes);
    }
    {
        lock_guard<mutex> lm(g_csMaster);
        g_master = make_shared<const vector<Hit>>(move(fresh));
        lock_guard<mutex> lv(g_csLive);
        g_live = make_shared<const LiveSnap>();
        g_dirtyLive = false;
    }
    g_objectCount = (uint64_t)g_master->size();
    applyPending();                       // re-digest queued USN records, advance positions
    saveCache();
    g_rebuilding = false;
    g_lastRebuildTick = GetTickCount();
    PostMessage(hwnd, WM_APP + 2, 0, 0);
    PostMessage(hwnd, WM_APP + 1, 0, 0);
}

/* ---- cache persistence while running / on exit ---------------------------- */

static atomic<bool> g_saveBusy{false};
static void saveCacheBg() {               // periodic flush, never on the UI thread
    if (g_saveBusy.exchange(true)) return;
    thread([] {
        saveCache();
        g_saveBusy = false;
    }).detach();
}
static void saveCacheAndWait() {          // exit path: finish any flush, then save sync
    DWORD t0 = GetTickCount();
    while (g_saveBusy.load() && GetTickCount() - t0 < 30000) Sleep(20);
    saveCache();
    logf_("CACHE saved on exit");
}

/* full $MFT scan of every NTFS volume into fresh node maps */
static void scanAllVolumes(const vector<pair<wchar_t, DWORD>>& drives) {
    vector<VolumeIndex> vols;
    vector<UsnPos> basePos;
    for (auto& d : drives) {
        /* baseline the journal position BEFORE scanning: the scan reflects the
           MFT at time T, everything journaled after this position is replayed
           on top (duplicates are idempotent), so even a volume with no file
           activity ends up with a valid, catchable position */
        UsnPos bp{};
        bool havePos = queryJournalPos(d.first, bp.journalId, bp.nextUsn);
        bp.valid = havePos;
        VolumeIndex vol; vol.letter = d.first; vol.serial = d.second;
        wstring vp = wstring(L"\\\\.\\") + d.first + L":";
        HANDLE h = CreateFileW(vp.c_str(), GENERIC_READ, 7, nullptr,
                               OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            vol.fromMft = readVolumeMftDirect(h, vol);
            CloseHandle(h);
        }
        if (!vol.fromMft)
            walkDir(wstring(1, d.first) + L":\\", vol, ROOT_FRN);
        vols.push_back(move(vol));
        basePos.push_back(bp);
    }
    {
        lock_guard<mutex> lk(g_csData);
        g_volumes = move(vols);
        lock_guard<mutex> lu(g_csUsnPos);
        g_usnPos = move(basePos);
    }
}

/* Is the cached index catchable via the USN journal? True when every volume's
   saved journal id still matches and the saved position has not been wrapped
   away by the ring buffer (saved >= LowestValidUsn). */
static bool usnCatchable() {
    vector<UsnPos> pos;
    { lock_guard<mutex> lk(g_csUsnPos); pos = g_usnPos; }
    if (pos.empty()) { logf_("CATCHUP: no positions"); return false; }
    for (size_t vi = 0; vi < pos.size(); vi++) {
        if (!pos[vi].valid) { logf_("CATCHUP: vol %zu position invalid", vi); return false; }
        wchar_t letter;
        { lock_guard<mutex> lk(g_csData);
          if (vi >= g_volumes.size()) { logf_("CATCHUP: vol %zu missing", vi); return false; }
          letter = g_volumes[vi].letter; }
        wstring vp = wstring(L"\\\\.\\") + letter + L":";
        HANDLE h = CreateFileW(vp.c_str(), GENERIC_READ, 7, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { logf_("CATCHUP: vol %c open failed", (char)letter); return false; }
        UsnJournalDataV0 jd{}; DWORD br;
        BOOL ok = DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                                  &jd, sizeof jd, &br, nullptr);
        CloseHandle(h);
        if (!ok) { logf_("CATCHUP: vol %c journal query failed", (char)letter); return false; }
        if (jd.UsnJournalID != pos[vi].journalId) {
            logf_("CATCHUP: vol %c journalId changed", (char)letter);
            return false;
        }
        if (pos[vi].nextUsn < jd.LowestValidUsn || pos[vi].nextUsn > jd.NextUsn) {
            logf_("CATCHUP: vol %c position %#llx outside [%#llx..%#llx]", (char)letter,
                  (unsigned long long)pos[vi].nextUsn,
                  (unsigned long long)jd.LowestValidUsn, (unsigned long long)jd.NextUsn);
            return false;
        }
        logf_("CATCHUP: vol %c ok", (char)letter);
    }
    return true;
}

static void buildThread(HWND hwnd) {
    auto t0 = chrono::steady_clock::now();
    auto drives = ntfsDrives();
    g_idxPhase = 0;                                  // loading the saved index
    bool fromCache = loadCache(drives) && usnCatchable();
    if (!fromCache) {
        g_idxPhase = 1;                              // full $MFT scan
        scanAllVolumes(drives);
        saveCache();
    }
    vector<Hit> master;
    {
        lock_guard<mutex> lk(g_csData);
        master = buildMasterLocked(g_volumes);
    }
    {
        lock_guard<mutex> lm(g_csMaster);
        g_master = make_shared<const vector<Hit>>(move(master));
        lock_guard<mutex> lv(g_csLive);
        g_live = make_shared<const LiveSnap>();
    }
    g_objectCount = (uint64_t)g_master->size();
    auto ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();
    logf_("READY objects=%llu in %lld ms (cache=%d)",
          (unsigned long long)g_objectCount.load(), ms, (int)fromCache);
    thread wt(watchThread); wt.detach();
    g_ready = true;
    PostMessage(hwnd, WM_APP + 1, 0, 0);
}

// ------------------------------------------------------------------ GUI

static HWND g_hMain, g_hEdit, g_hList, g_hStatus;
static vector<const Hit*> g_results;                 // filtered view (UI thread)
static shared_ptr<const vector<Hit>> g_resultMaster; // keeps pointers alive
static shared_ptr<const LiveSnap> g_resultLive;
static DWORD g_lastType = 0;
static HFONT g_font = nullptr;

enum { IDT_DRAIN = 1, IDT_IDLE = 2, IDT_SEARCH = 3, IDT_SELSTAT = 4,
       IDM_TRAY_SHOW = 2001, IDM_TRAY_EXIT = 2002, IDM_TRAY_REBUILD = 2003,
       IDM_INSTALL_MENU = 2004, IDM_UNINSTALL_MENU = 2005,
       IDM_OPEN = 2006, IDM_OPENPATH = 2007, IDM_COPY_PATH = 2008, IDM_PROPS = 2009,
       IDM_COPY_NAME = 2010,

       /* main menu bar commands; menu entries for features this build does
          not support exist but are grayed */
       IDM_FILE_NEWWINDOW = 21001, IDM_FILE_EXIT = 21002, IDM_FILE_CLOSE = 21003,
       IDM_FILE_EXPORT = 21004,
       IDM_EDIT_COPY = 21011, IDM_EDIT_PASTE = 21012, IDM_EDIT_SELECTALL = 21013,
       IDM_EDIT_INVERT = 21014,
       IDM_VIEW_FILTERS = 21021, IDM_VIEW_STATUSBAR = 21022,
       IDM_VIEW_ZOOM_IN = 21023, IDM_VIEW_ZOOM_OUT = 21024, IDM_VIEW_ZOOM_RESET = 21025,
       IDM_VIEW_WIN_SMALL = 21026, IDM_VIEW_WIN_MEDIUM = 21027,
       IDM_VIEW_WIN_LARGE = 21028, IDM_VIEW_WIN_AUTO = 21029,
       IDM_VIEW_REFRESH = 21030,
       IDM_SORT_BASE = 21040,          /* +0 name +1 path +2 size +3 ext +4 date */
       IDM_SORT_ASC = 21050, IDM_SORT_DESC = 21051,
       IDM_TOP_NEVER = 21052, IDM_TOP_ALWAYS = 21053,
       IDM_SEARCH_CASE = 21060, IDM_SEARCH_WORD = 21061, IDM_SEARCH_PATH = 21062,
       IDM_FILTER_BASE = 21070,        /* +0..7 = 所有/音频/压缩文件/文档/可执行文件/文件夹/图片/视频 */
       IDM_TOOL_REBUILD = 21080, IDM_TOOL_INSTALL = 21081, IDM_TOOL_UNINSTALL = 21082,
       IDM_HELP_HELP = 21090, IDM_HELP_SYNTAX = 21091, IDM_HELP_CMDLINE = 21092,
       IDM_HELP_ABOUT = 21093 };
#define WM_TRAYICON (WM_APP + 4)
#define WM_SHOWEXISTING (WM_APP + 5)   /* second launch: show the running window */

/* ---- Everything-style UI state ------------------------------------------- */
enum SortField { SORT_NAME = 0, SORT_PATH, SORT_SIZE, SORT_EXT, SORT_DATE };
static int  g_sortField = SORT_NAME;
static bool g_sortAsc   = true;
static bool g_matchCase = false, g_matchWord = false, g_matchPathOpt = false;
static int  g_filterIdx = 0;                 // filter combo selection
static bool g_filterBarVisible = true;       // 视图 > 筛选器
static bool g_statusBarVisible = true;       // 视图 > 状态栏
static int  g_zoom = 0;                      // font size offset in points
static bool g_topmost = false;               // 视图 > 置顶 > 总是
static HACCEL g_hAccel = nullptr;
static HMENU g_hMenu = nullptr;
static HWND  g_hFilter = nullptr;            // filter combo box
static bool  g_selectAllBusy = false;        // guard while LVN_ITEMCHANGED storms

static wstring g_initialDir;                         // -p <dir>: scope the first search

/* Register/unregister the Explorer context-menu entry "用 fastfind 搜索" for
   directories, the folder background and drives. Launched as:
   fastfindui.exe -p "<folder>"  — the search box starts scoped to that folder. */
static bool setShellMenu(bool install) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    struct Spot { const wchar_t* sub; const wchar_t* arg; };
    const Spot spots[] = {
        { L"SOFTWARE\\Classes\\Directory\\shell\\fastfind",             L"\"%1\"" },
        { L"SOFTWARE\\Classes\\Directory\\Background\\shell\\fastfind", L"\"%V\"" },
        { L"SOFTWARE\\Classes\\Drive\\shell\\fastfind",                 L"\"%1\"" },
    };
    for (auto& s : spots) {
        if (!install) {
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, s.sub);
            continue;
        }
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, s.sub, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
            return false;
        const wchar_t* label = L"用 fastfind 搜索(&F)";
        RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)label,
                       (DWORD)((wcslen(label) + 1) * sizeof(wchar_t)));
        RegSetValueExW(k, L"Icon", 0, REG_SZ, (const BYTE*)exe,
                       (DWORD)((wcslen(exe) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
        wstring cmdKey = wstring(s.sub) + L"\\command";
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, cmdKey.c_str(), 0, nullptr, 0,
                            KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
            return false;
        wstring val = wstring(L"\"") + exe + L"\" -p " + s.arg;
        RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)val.c_str(),
                       (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    return true;
}

static NOTIFYICONDATAW g_nid{};
static bool g_tray = false;

static void trayAdd(const wchar_t* tip) {
    if (g_tray) return;
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hMain;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcsncpy(g_nid.szTip, tip ? tip : L"fastfind — 文件搜索（左键恢复，右键退出）", 127);
    g_nid.szTip[127] = 0;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_tray = true;
}

static void trayRemove() {
    if (!g_tray) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_tray = false;
}

static void showMainWindow() {
    trayRemove();
    ShowWindow(g_hMain, SW_RESTORE);
    ShowWindow(g_hMain, SW_SHOW);
    SetForegroundWindow(g_hMain);
    SetFocus(g_hEdit);
}

/* ---- close-button choice (ask / minimize to tray / exit), remembered ------- */

static int g_closeAction = 0;             // 0 = ask every time, 1 = tray, 2 = exit

static wstring settingsFile() {
    const char* la = getenv("LOCALAPPDATA");
    string s = (la ? la : ".");
    s += "\\fastfind-settings.ini";
    wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, MAX_PATH);
    return w;
}
static int loadCloseAction() {
    wchar_t buf[16]{};
    GetPrivateProfileStringW(L"fastfind", L"closeAction", L"ask", buf, 16,
                             settingsFile().c_str());
    if (!wcscmp(buf, L"tray")) return 1;
    if (!wcscmp(buf, L"exit")) return 2;
    return 0;
}
static void saveCloseAction(int a) {
    WritePrivateProfileStringW(L"fastfind", L"closeAction",
                               a == 1 ? L"tray" : a == 2 ? L"exit" : L"ask",
                               settingsFile().c_str());
}

static void trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"显示主窗口(&S)\t搜索");
    AppendMenuW(m, MF_STRING, IDM_TRAY_REBUILD, L"重建索引(&R)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_INSTALL_MENU, L"注册资源管理器右键菜单(&E)");
    AppendMenuW(m, MF_STRING, IDM_UNINSTALL_MENU, L"移除资源管理器右键菜单(&M)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"退出(&X)");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hMain);            // required so the menu dismisses
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hMain, nullptr);
    PostMessageW(g_hMain, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---- Everything-compatible search syntax -----------------------------------
 * Space-separated terms with AND semantics. Supported subset (matching
 * voidtools Everything):
 *   foo bar          every term must appear in the file NAME (default)
 *   "foo bar"        quoted phrase (spaces kept, matched as one term)
 *   path:foo         match the whole path instead of the name only
 *   C:\tools\ foo    a term containing '\' restricts the search path
 *   download\        (relative folder filter works the same way)
 *   *.cpp ext:txt    extension match
 *   file: folder:    only files / only folders
 *   !foo             exclude matches (combinable: !path:foo, !*.tmp)
 * Not implemented: regex, case:, wholeword:, date/size filters. */

struct Term { wstring low, orig; };    // lowered + as-typed (for 匹配大小写)

struct Query {
    vector<Term> name, nameNeg;        // terms matched against the name
    vector<Term> path, pathNeg;        // terms matched against the full path
    vector<wstring> exts, extsNeg;     // ".cpp" style suffixes
    bool filesOnly = false, foldersOnly = false;
    bool matchCase = false, wholeWord = false, matchPath = false;
    bool empty = true;
};

static Query parseQuery(const wstring& text) {
    Query q;
    q.matchCase = g_matchCase; q.wholeWord = g_matchWord; q.matchPath = g_matchPathOpt;
    size_t i = 0;
    auto isSp = [](wchar_t c) { return c == L' ' || c == L'\t'; };
    while (i < text.size()) {
        while (i < text.size() && isSp(text[i])) i++;
        bool neg = false;
        if (i < text.size() && text[i] == L'!') { neg = true; i++; }
        wstring tok;
        if (i < text.size() && text[i] == L'"') {
            size_t j = text.find(L'"', ++i);
            tok = text.substr(i, j == wstring::npos ? wstring::npos : j - i);
            i = (j == wstring::npos ? text.size() : j + 1);
        } else {
            size_t j = i;
            while (j < text.size() && !isSp(text[j])) j++;
            tok = text.substr(i, j - i);
            i = j;
        }
        if (tok.empty()) continue;
        q.empty = false;
        wstring low = toLowerW(tok);
        Term t{ low, tok };
        bool isPath = false;
        if (low.rfind(L"path:", 0) == 0 && low.size() > 5) { low.erase(0, 5); isPath = true; }
        else if (low == L"file:")   { if (!neg) q.filesOnly = true;   continue; }
        else if (low == L"folder:") { if (!neg) q.foldersOnly = true; continue; }
        else if (low.rfind(L"ext:", 0) == 0 && low.size() > 4) {
            low.erase(0, 4);                        // ext:mp3;wav;flac — multi value
            size_t s = 0;
            while (s < low.size()) {
                size_t e = low.find(L';', s);
                wstring one = low.substr(s, e == wstring::npos ? wstring::npos : e - s);
                if (!one.empty()) (neg ? q.extsNeg : q.exts).push_back(L"." + one);
                if (e == wstring::npos) break;
                s = e + 1;
            }
            continue;
        }
        else if (low.rfind(L"*.", 0) == 0 && low.size() > 2) {
            low.erase(0, 1);                       // "*.cpp" -> ".cpp"
            (neg ? q.extsNeg : q.exts).push_back(low);
            continue;
        }
        if (!isPath && low.find(L'\\') != wstring::npos) isPath = true;
        (isPath ? (neg ? q.pathNeg : q.path) : (neg ? q.nameNeg : q.name)).push_back(t);
    }
    return q;
}

static bool endsWithW(const wchar_t* s, size_t slen, const wstring& suffix) {
    return suffix.size() <= slen && !_wcsicmp(s + slen - suffix.size(), suffix.c_str());
}

/* whole-word containment: the hit must start/end a word boundary around the
   term (Everything's 全字匹配); CJK code points never count as word chars */
static bool wordContains(const wchar_t* s, const wchar_t* t) {
    size_t tl = wcslen(t);
    const wchar_t* p = s;
    while ((p = wcsstr(p, t)) != nullptr) {
        bool leftOk  = (p == s) || !(iswalnum((wint_t)p[-1]) || p[-1] == L'_');
        wchar_t nx = p[tl];
        bool rightOk = !nx || !(iswalnum((wint_t)nx) || nx == L'_');
        if (leftOk && rightOk) return true;
        ++p;
    }
    return false;
}

static bool termMatch(const wchar_t* hay, const Term& t, const Query& q) {
    const wchar_t* needle = q.matchCase ? t.orig.c_str() : t.low.c_str();
    return q.wholeWord ? wordContains(hay, needle)
                       : wcsstr(hay, needle) != nullptr;
}

static bool matchQuery(const Hit& h, const Query& q) {
    if (q.filesOnly && h.isDir) return false;
    if (q.foldersOnly && !h.isDir) return false;
    /* case-sensitive matching works on the original path, insensitive on the
       pre-lowercased copy — both are stored in every Hit */
    const wchar_t* full = (q.matchCase ? h.path : h.lower).c_str();
    size_t slash = q.matchCase ? h.path.rfind(L'\\') : h.lower.rfind(L'\\');
    const wchar_t* name = full + (slash == wstring::npos ? 0 : slash + 1);
    /* 匹配路径: name terms are searched against the whole path instead */
    const wchar_t* nameHay = q.matchPath ? full : name;
    for (auto& t : q.name)     if (!termMatch(nameHay, t, q))  return false;
    for (auto& t : q.nameNeg)  if (termMatch(nameHay, t, q))   return false;
    for (auto& t : q.path)     if (!termMatch(full, t, q))     return false;
    for (auto& t : q.pathNeg)  if (termMatch(full, t, q))      return false;
    if (!q.exts.empty()) {
        size_t nameLen = wcslen(name);
        bool ok = false;
        for (auto& e : q.exts) if (endsWithW(name, nameLen, e)) { ok = true; break; }
        if (!ok) return false;
    }
    size_t nameLen = wcslen(name);
    for (auto& e : q.extsNeg) if (endsWithW(name, nameLen, e)) return false;
    return true;
}

/* ---- sorting (Everything: sort menu + clickable column headers) --------- */

static const wchar_t* hitName(const Hit* h, const wstring& p) {
    size_t s = p.rfind(L'\\');
    return p.c_str() + (s == wstring::npos ? 0 : s + 1);
}
static int cmpText(const wstring& a, const wstring& b) {
    int c = _wcsicmp(a.c_str(), b.c_str());
    return c ? c : (a < b ? -1 : a > b ? 1 : 0);   // stable-ish tiebreak
}
static bool hitLess(const Hit* a, const Hit* b, int field, bool asc) {
    int c = 0;
    switch (field) {
    case SORT_NAME: {
        const wchar_t* na = hitName(a, a->lower);
        const wchar_t* nb = hitName(b, b->lower);
        c = _wcsicmp(na, nb);
        if (!c) c = wcscmp(na, nb);            // deterministic tiebreak
        break; }
    case SORT_PATH:
        c = cmpText(a->lower, b->lower); break;
    case SORT_SIZE:
        if (a->size != b->size) c = a->size < b->size ? -1 : 1;
        break;
    case SORT_DATE:
        if (a->mtime != b->mtime) c = a->mtime < b->mtime ? -1 : 1;
        break;
    case SORT_EXT: {
        const wchar_t* na = wcsrchr(a->path.c_str(), L'.');
        const wchar_t* nb = wcsrchr(b->path.c_str(), L'.');
        const wchar_t* sa = na && na > a->path.c_str() && na > wcsrchr(a->path.c_str(), L'\\') ? na : L"";
        const wchar_t* sb = nb && nb > b->path.c_str() && nb > wcsrchr(b->path.c_str(), L'\\') ? nb : L"";
        c = _wcsicmp(sa, sb);
        break; }
    }
    if (c == 0) c = cmpText(a->lower, b->lower);   // secondary: full path
    return asc ? c < 0 : c > 0;
}

/* ---- async search ----------------------------------------------------------
 * Filtering scans the whole multi-million-entry index and can take several
 * hundred milliseconds, so it must never run on the UI thread. The UI thread
 * only publishes the latest query (requestSearch); a dedicated worker scans
 * the immutable snapshots and posts the finished result set back, so the
 * window stays responsive at all times (typing, dragging, during indexing). */

struct SearchOutput {
    vector<const Hit*> items;
    shared_ptr<const vector<Hit>> master;    // keeps item pointers alive
    shared_ptr<const LiveSnap> live;
    unsigned ms = 0;
    bool truncated = false;
};
static mutex g_csSearch;
static condition_variable g_cvSearch;
static wstring g_searchQuery;                // latest query (guarded by g_csSearch)
static atomic<uint64_t> g_queryGen{0};       // bumped when the query TEXT changes; aborts scans
static atomic<uint64_t> g_workGen{0};        // bumped on any re-run request (text or data change)

/* 筛选器下拉（Everything 搜索菜单底部的过滤项）映射为搜索语法前缀 */
static const wchar_t* filterSyntax(int idx) {
    static const wchar_t* syn[] = {
        L"",
        L"ext:mp3;wav;flac;ape;ogg;m4a;wma;aac",                      // 音频
        L"ext:zip;rar;7z;tar;gz;bz2;xz;iso;cab",                      // 压缩文件
        L"ext:doc;docx;pdf;txt;md;xls;xlsx;ppt;pptx;odt;rtf",         // 文档
        L"ext:exe;msi;bat;cmd;com;scr;ps1",                           // 可执行文件
        L"folder:",                                                   // 文件夹
        L"ext:png;jpg;jpeg;gif;bmp;ico;webp;svg;tif;tiff;psd",        // 图片
        L"ext:mp4;mkv;avi;wmv;mov;flv;webm;ts;rmvb;mpg;m4v",          // 视频
    };
    return (idx >= 0 && idx < 8) ? syn[idx] : L"";
}

static void requestSearch() {
    wchar_t q[1200];
    GetWindowTextW(g_hEdit, q, 1024);
    wstring eff = filterSyntax(g_filterIdx);
    if (*q && !eff.empty()) eff += L" ";
    eff += q;
    {
        lock_guard<mutex> lk(g_csSearch);
        bool textChanged = (g_queryGen.load() == 0 || g_searchQuery != eff);
        g_searchQuery = eff;
        g_workGen++;
        if (textChanged) g_queryGen++;
    }
    g_cvSearch.notify_one();
}

static void searchWorker(HWND hwnd) {
    uint64_t done = 0;
    while (!g_quit.load()) {
        wstring query; uint64_t gen = 0, abortGen = 0;
        {
            unique_lock<mutex> lk(g_csSearch);
            g_cvSearch.wait(lk, [] { return g_workGen.load() > 0 || g_quit.load(); });
            if (g_quit.load()) return;
            if (g_workGen.load() == done) continue;
            query = g_searchQuery;
            gen = g_workGen.load();          // which work item we are executing
            abortGen = g_queryGen.load();    // abort only if the TEXT changed
        }
        auto t0 = chrono::steady_clock::now();
        shared_ptr<const vector<Hit>> master;
        shared_ptr<const LiveSnap> live;
        { lock_guard<mutex> lk(g_csMaster); master = g_master; }
        { lock_guard<mutex> lk(g_csLive); live = g_live; }
        if (!master || !live) continue;

        Query q = parseQuery(query);
        SearchOutput* out = new SearchOutput();
        out->master = master;
        out->live = live;
        const size_t CAP = 200000;
        bool stale = false;
        {
            const auto& dead = live->dead;
            uint64_t seen = 0;
            for (auto& h : *master) {
                /* abort promptly when the query TEXT changed (new keystroke),
                   so typing feels instant; pure data refreshes never abort */
                if ((++seen & 0xFFFF) == 0 && g_queryGen.load() != abortGen) { stale = true; break; }
                if (!dead.empty() && dead.count(h.frnKey)) continue;
                if (matchQuery(h, q)) {
                    out->items.push_back(&h);
                    if (out->items.size() >= CAP) { out->truncated = true; break; }
                }
            }
        }
        if (!stale) for (auto& h : live->recent) {
            if (live->dead.count(h.frnKey)) continue;
            if (matchQuery(h, q)) out->items.push_back(&h);
        }
        if (stale) { delete out; continue; }
        /* Everything keeps results sorted (default: name ascending) */
        {
            int field = g_sortField; bool asc = g_sortAsc;
            sort(out->items.begin(), out->items.end(),
                 [field, asc](const Hit* a, const Hit* b) { return hitLess(a, b, field, asc); });
        }
        out->ms = (unsigned)chrono::duration_cast<chrono::milliseconds>(
                      chrono::steady_clock::now() - t0).count();
        logf_("SEARCH '%ls' -> %zu items in %u ms", query.c_str(), out->items.size(), out->ms);

        /* a newer query TEXT arrived while we were scanning: drop this result
           and loop again with the latest query (data-only bumps still deliver) */
        { lock_guard<mutex> lk(g_csSearch); if (g_queryGen.load() != abortGen) { delete out; continue; } }
        if (!PostMessageW(hwnd, WM_APP + 3, 0, (LPARAM)out)) delete out;
        done = gen;
    }
}

static void fmtSize(uint64_t n, wchar_t* out, size_t cch) {
    if (n < 1024) { _snwprintf(out, cch, L"%u B", (unsigned)n); return; }
    const wchar_t* unit[] = { L"KB", L"MB", L"GB", L"TB" };
    double v = (double)n; int u = -1;
    while (v >= 1024 && u < 3) { v /= 1024; u++; }
    _snwprintf(out, cch, L"%.1f %s", v, unit[u]);
}

static void fmtTime(int64_t ft, wchar_t* out, size_t cch) {
    if (!ft) { out[0] = 0; return; }
    FILETIME f{ (DWORD)ft, (DWORD)(uint64_t(ft) >> 32) }, l;
    SYSTEMTIME s;
    FileTimeToLocalFileTime(&f, &l);
    FileTimeToSystemTime(&l, &s);
    _snwprintf(out, cch, L"%04u-%02u-%02u %02u:%02u",
               s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute);
}

static void openHit(const Hit* h) {
    if (!h) return;
    if (h->isDir)
        ShellExecuteW(nullptr, L"open", h->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else {
        wstring args = L"/select,\"" + h->path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

/* "Open Path": open the containing folder with the item selected in Explorer
   (for a folder, select the folder inside its parent) — same as Everything. */
static void openPathInExplorer(const Hit* h) {
    wstring args = L"/select,\"" + h->path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

/* commands from the result-list context menu */
static void handleResultCommand(int id) {
    vector<const Hit*> sel;
    int i = -1;
    while ((i = ListView_GetNextItem(g_hList, i, LVNI_SELECTED)) >= 0 &&
           (size_t)i < g_results.size())
        sel.push_back(g_results[i]);
    if (sel.empty()) return;
    switch (id) {
    case IDM_OPEN:
        for (auto* h : sel) {
            if (h->isDir)
                ShellExecuteW(nullptr, L"open", h->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else
                ShellExecuteW(nullptr, L"open", h->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    case IDM_OPENPATH:
        openPathInExplorer(sel[0]);
        break;
    case IDM_COPY_PATH: {
        wstring text;
        for (auto* h : sel) { if (!text.empty()) text += L"\r\n"; text += h->path; }
        if (OpenClipboard(g_hMain)) {
            EmptyClipboard();
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
            if (g) {
                memcpy(GlobalLock(g), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(g);
                SetClipboardData(CF_UNICODETEXT, g);
            }
            CloseClipboard();
        }
        break; }
    case IDM_COPY_NAME: {
        wstring text;
        for (auto* h : sel) {
            size_t s = h->path.rfind(L'\\');
            if (!text.empty()) text += L"\r\n";
            text += s == wstring::npos ? h->path : h->path.substr(s + 1);
        }
        if (OpenClipboard(g_hMain)) {
            EmptyClipboard();
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
            if (g) {
                memcpy(GlobalLock(g), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(g);
                SetClipboardData(CF_UNICODETEXT, g);
            }
            CloseClipboard();
        }
        break; }
    case IDM_PROPS:
        for (auto* h : sel) {
            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.fMask = SEE_MASK_INVOKEIDLIST;
            sei.lpVerb = L"properties";
            sei.lpFile = h->path.c_str();
            sei.nShow = SW_SHOW;
            ShellExecuteExW(&sei);
        }
        break;
    }
}

static void copyText(wchar_t* dst, const wchar_t* src, size_t cch) {
    if (!cch) return;
    wcsncpy(dst, src, cch - 1);
    dst[cch - 1] = 0;
}

/* icon index into the system image list for a result row. Folders share one
   cached folder icon; files are cached per extension — SHGFI_USEFILEATTRIBUTES
   resolves purely from the extension, no file access, so the cache fills with
   one shell call per distinct type and scrolling millions of rows is free. */
static int iconFor(const Hit* h) {
    static int folderIcon = -1;
    static unordered_map<wstring, int> byExt;
    if (h->isDir) {
        if (folderIcon < 0) {
            SHFILEINFOW sfi{};
            SHGetFileInfoW(L"x", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof sfi,
                           SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES);
            folderIcon = sfi.iIcon;
        }
        return folderIcon;
    }
    size_t dot = h->path.rfind(L'.');
    size_t slash = h->path.rfind(L'\\');
    wstring ext = (dot != wstring::npos && dot > slash) ? toLowerW(h->path.substr(dot)) : L"";
    auto it = byExt.find(ext);
    if (it != byExt.end()) return it->second;
    SHFILEINFOW sfi{};
    wstring fake = ext.empty() ? wstring(L"x") : L"x" + ext;
    SHGetFileInfoW(fake.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi,
                   SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES);
    byExt.emplace(move(ext), sfi.iIcon);
    return sfi.iIcon;
}

/* ---- main menu bar / accelerators / status bar -----------------------------
 * Menu layout, labels and shortcuts follow the Everything conventions.
 * Entries for features this build does not implement (ETP/HTTP servers,
 * regex, bookmarks, file lists, thumbnails…) are grayed, matching how the
 * reference application marks its own unavailable items. */

static HMENU buildMenuBar() {
    HMENU bar = CreateMenu();
    HMENU m, sub;

    m = CreatePopupMenu();                                     // 文件(&F)
    AppendMenuW(m, MF_STRING, IDM_FILE_NEWWINDOW, L"新建搜索窗口(&N)\tCtrl+N");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"打开文件列表(&O)...\tCtrl+O");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"关闭文件列表(&B)");
    AppendMenuW(m, MF_STRING, IDM_FILE_CLOSE, L"关闭(&C)\tCtrl+W");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_FILE_EXPORT, L"导出(&E)...\tCtrl+S");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_FILE_EXIT, L"退出(&X)\tCtrl+Q");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"文件(&F)");

    m = CreatePopupMenu();                                     // 编辑(&E)
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"剪切(&T)\tCtrl+X");
    AppendMenuW(m, MF_STRING, IDM_EDIT_COPY, L"复制(&C)\tCtrl+C");
    AppendMenuW(m, MF_STRING, IDM_EDIT_PASTE, L"粘贴(&P)\tCtrl+V");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"复制到文件夹(&F)...");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"移动到文件夹(&V)...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EDIT_SELECTALL, L"全选(&A)\tCtrl+A");
    AppendMenuW(m, MF_STRING, IDM_EDIT_INVERT, L"反选(&I)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    sub = CreatePopupMenu();
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"高级复制(&F)...");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"高级移动(&V)...");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"高级(&E)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"编辑(&E)");

    m = CreatePopupMenu();                                     // 视图(&V)
    AppendMenuW(m, MF_STRING, IDM_VIEW_FILTERS, L"筛选器(&F)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"预览(&P)\tAlt+P");
    AppendMenuW(m, MF_STRING, IDM_VIEW_STATUSBAR, L"状态栏(&B)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"超大图标(&X)\tCtrl+Shift+1");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"大图标(&L)\tCtrl+Shift+2");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"中等图标(&M)\tCtrl+Shift+3");
    AppendMenuW(m, MF_STRING, 0, L"详情(&D)\tCtrl+Shift+6");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    sub = CreatePopupMenu();                                   // 窗口大小(&W)
    AppendMenuW(sub, MF_STRING, IDM_VIEW_WIN_SMALL,  L"小(&S)\tAlt+1");
    AppendMenuW(sub, MF_STRING, IDM_VIEW_WIN_MEDIUM, L"中(&N)\tAlt+2");
    AppendMenuW(sub, MF_STRING, IDM_VIEW_WIN_LARGE,  L"大(&L)\tAlt+3");
    AppendMenuW(sub, MF_STRING, IDM_VIEW_WIN_AUTO,   L"自动(&A)\tAlt+4");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"窗口大小(&W)");
    sub = CreatePopupMenu();                                   // 字号(&T)
    AppendMenuW(sub, MF_STRING, IDM_VIEW_ZOOM_IN,    L"增大(&I)\tCtrl+=");
    AppendMenuW(sub, MF_STRING, IDM_VIEW_ZOOM_OUT,   L"减小(&D)\tCtrl+-");
    AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sub, MF_STRING, IDM_VIEW_ZOOM_RESET, L"正常(&N)\tCtrl+0");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"字号(&T)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    sub = CreatePopupMenu();                                   // 排序(&S)
    AppendMenuW(sub, MF_STRING, IDM_SORT_BASE + SORT_NAME, L"名称\tCtrl+1");
    AppendMenuW(sub, MF_STRING, IDM_SORT_BASE + SORT_PATH, L"路径\tCtrl+2");
    AppendMenuW(sub, MF_STRING, IDM_SORT_BASE + SORT_SIZE, L"大小\tCtrl+3");
    AppendMenuW(sub, MF_STRING, IDM_SORT_BASE + SORT_EXT,  L"扩展名\tCtrl+4");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"类型\tCtrl+5");
    AppendMenuW(sub, MF_STRING, IDM_SORT_BASE + SORT_DATE, L"修改时间\tCtrl+6");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"创建时间\tCtrl+7");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"访问时间");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"属性\tCtrl+8");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"最近修改时间\tCtrl+9");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"运行次数");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"最近打开时间");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"文件列表名");
    AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sub, MF_STRING, IDM_SORT_ASC,  L"升序(&A)");
    AppendMenuW(sub, MF_STRING, IDM_SORT_DESC, L"降序(&D)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"排序(&S)");
    sub = CreatePopupMenu();                                   // 前往(&G)
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"首页(&H)\tAlt+Home");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"前往(&G)");
    AppendMenuW(m, MF_STRING, IDM_VIEW_REFRESH, L"刷新(&R)\tF5");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    sub = CreatePopupMenu();                                   // 置顶(&T)
    AppendMenuW(sub, MF_STRING, IDM_TOP_NEVER,   L"从不(&N)");
    AppendMenuW(sub, MF_STRING, IDM_TOP_ALWAYS,  L"总是(&A)");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"搜索时(&S)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"置顶(&T)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"视图(&V)");

    m = CreatePopupMenu();                                     // 搜索(&S)
    AppendMenuW(m, MF_STRING, IDM_SEARCH_CASE, L"区分大小写(&C)\tCtrl+I");
    AppendMenuW(m, MF_STRING, IDM_SEARCH_WORD, L"全字匹配(&W)\tCtrl+B");
    AppendMenuW(m, MF_STRING, IDM_SEARCH_PATH, L"匹配路径(&P)\tCtrl+U");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"匹配变音标记(&D)\tCtrl+M");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"使用正则表达式(&R)\tCtrl+R");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"高级搜索(&A)...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"添加到筛选(&A)...");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"管理筛选器(&O)...\tCtrl+Shift+F");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    static const wchar_t* filterNames[] = {
        L"所有", L"音频", L"压缩文件", L"文档",
        L"可执行文件", L"文件夹", L"图片", L"视频" };
    for (int i = 0; i < 8; i++)
        AppendMenuW(m, MF_STRING, IDM_FILTER_BASE + i, filterNames[i]);
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"搜索(&S)");

    m = CreatePopupMenu();                                     // 书签(&B)
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"添加到书签(&A)...\tCtrl+D");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"管理书签(&O)...\tCtrl+Shift+B");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"书签(&B)");

    m = CreatePopupMenu();                                     // 工具(&T)
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"连接 ETP 服务器(&C)...");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"断开 ETP 服务器(&D)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"文件列表编辑器(&E)...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TOOL_REBUILD, L"重建索引(&R)");
    AppendMenuW(m, MF_STRING, IDM_TOOL_INSTALL,   L"注册资源管理器右键菜单(&E)");
    AppendMenuW(m, MF_STRING, IDM_TOOL_UNINSTALL, L"移除资源管理器右键菜单(&M)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"选项(&O)...\tCtrl+P");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"工具(&T)");

    m = CreatePopupMenu();                                     // 帮助(&H)
    AppendMenuW(m, MF_STRING, IDM_HELP_HELP, L"帮助(&H)\tF1");
    AppendMenuW(m, MF_STRING, IDM_HELP_SYNTAX, L"搜索语法(&S)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"正则表达式语法(&R)");
    AppendMenuW(m, MF_STRING, IDM_HELP_CMDLINE, L"命令行选项(&C)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"官方网站(&W)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"检查更新(&U)...");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"捐赠(&D)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_HELP_ABOUT, L"关于 fastfind(&A)...\tCtrl+F1");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)m, L"帮助(&H)");
    return bar;
}

static void initAccelerators() {
    static ACCEL a[] = {
        { FVIRTKEY | FCONTROL, 'N',  IDM_FILE_NEWWINDOW },
        { FVIRTKEY | FCONTROL, 'W',  IDM_FILE_CLOSE },
        { FVIRTKEY | FCONTROL, 'S',  IDM_FILE_EXPORT },
        { FVIRTKEY | FCONTROL, 'Q',  IDM_FILE_EXIT },
        { FVIRTKEY | FCONTROL, 'C',  IDM_EDIT_COPY },
        { FVIRTKEY | FCONTROL, 'V',  IDM_EDIT_PASTE },
        { FVIRTKEY | FCONTROL, 'A',  IDM_EDIT_SELECTALL },
        { FVIRTKEY | FCONTROL, 'I',  IDM_SEARCH_CASE },
        { FVIRTKEY | FCONTROL, 'B',  IDM_SEARCH_WORD },
        { FVIRTKEY | FCONTROL, 'U',  IDM_SEARCH_PATH },
        { FVIRTKEY | FCONTROL, '1',  IDM_SORT_BASE + SORT_NAME },
        { FVIRTKEY | FCONTROL, '2',  IDM_SORT_BASE + SORT_PATH },
        { FVIRTKEY | FCONTROL, '3',  IDM_SORT_BASE + SORT_SIZE },
        { FVIRTKEY | FCONTROL, '4',  IDM_SORT_BASE + SORT_EXT },
        { FVIRTKEY | FCONTROL, '6',  IDM_SORT_BASE + SORT_DATE },
        { FVIRTKEY | FCONTROL, VK_OEM_PLUS,   IDM_VIEW_ZOOM_IN },
        { FVIRTKEY | FCONTROL, VK_OEM_MINUS,  IDM_VIEW_ZOOM_OUT },
        { FVIRTKEY | FCONTROL, '0',  IDM_VIEW_ZOOM_RESET },
        { FVIRTKEY,            VK_F5, IDM_VIEW_REFRESH },
        { FVIRTKEY,            VK_F1, IDM_HELP_HELP },
        { FVIRTKEY | FCONTROL, VK_F1, IDM_HELP_ABOUT },
        { FVIRTKEY | FALT,     '1',  IDM_VIEW_WIN_SMALL },
        { FVIRTKEY | FALT,     '2',  IDM_VIEW_WIN_MEDIUM },
        { FVIRTKEY | FALT,     '3',  IDM_VIEW_WIN_LARGE },
        { FVIRTKEY | FALT,     '4',  IDM_VIEW_WIN_AUTO },
    };
    g_hAccel = CreateAcceleratorTableW(a, (int)(sizeof a / sizeof a[0]));
}

/* sort arrows in the column headers (needs the comctl32 v6 manifest) */
static void updateHeaderArrows() {
    HWND hd = ListView_GetHeader(g_hList);
    int n = Header_GetItemCount(hd);
    for (int i = 0; i < n; i++) {
        HDITEMW it{}; it.mask = HDI_FORMAT;
        Header_GetItem(hd, i, &it);
        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_sortField) it.fmt |= g_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hd, i, &it);
    }
}

/* re-sort the current result list on the UI thread (column click / menu) */
static void applySortNow(int field, bool asc) {
    g_sortField = field; g_sortAsc = asc;
    sort(g_results.begin(), g_results.end(),
         [](const Hit* a, const Hit* b) { return hitLess(a, b, g_sortField, g_sortAsc); });
    ListView_SetItemCountEx(g_hList, (int)g_results.size(),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(g_hList, nullptr, FALSE);
    updateHeaderArrows();
}

/* two-part status bar like Everything: left "N 个对象" / "N 个对象选中",
   right: selected-item details or MACHINE\user */
static void updateStatusBar() {
    if (!g_hStatus) return;
    RECT rc; GetClientRect(g_hStatus, &rc);
    int parts[2] = { max(140, (int)rc.right - 360), -1 };
    SendMessage(g_hStatus, SB_SETPARTS, 2, (LPARAM)parts);
    wchar_t left[128];
    if (!g_ready.load()) {
        wcscpy(left, g_idxPhase.load() == 1
                    ? L"正在建立索引（首次约 40 秒）…"
                    : L"正在从数据库加载索引…");
    } else if (g_replaying.load() > 0) {
        wcscpy(left, L"正在同步上次退出以来的变更…");
    } else {
        UINT sel = ListView_GetSelectedCount(g_hList);
        if (sel > 0)
            _snwprintf(left, 128, L"%u 个对象选中", sel);
        else {
            bool filtered = g_filterIdx != 0 || GetWindowTextLengthW(g_hEdit) > 0;
            unsigned long long n = filtered ? g_results.size() : g_objectCount.load();
            _snwprintf(left, 128, L"%llu 个对象", n);
        }
    }
    wchar_t right[180] = L"";
    UINT selCnt = ListView_GetSelectedCount(g_hList);
    if (selCnt == 1) {
        int i = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
        if (i >= 0 && (size_t)i < g_results.size()) {
            const Hit* h = g_results[i];
            wchar_t sz[32]{}, tm[40]{};
            if (!h->isDir) fmtSize(h->size, sz, 32);
            fmtTime(h->mtime, tm, 40);
            _snwprintf(right, 180, L"%s%s%s  %s",
                       h->isDir ? L"文件夹" : L"", h->isDir ? L"  " : L"", sz, tm);
        }
    } else if (selCnt == 0) {
        wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1]{}, pu[UNLEN + 1]{};
        DWORD n1 = _countof(pc), n2 = _countof(pu);
        GetComputerNameW(pc, &n1);
        GetUserNameW(pu, &n2);
        _snwprintf(right, 180, L"%s\\%s", pc, pu);
    }
    SendMessage(g_hStatus, SB_SETTEXTW, MAKEWPARAM(0, 0), (LPARAM)left);
    SendMessage(g_hStatus, SB_SETTEXTW, MAKEWPARAM(1, 0), (LPARAM)right);
}

static void exportResults() {
    wchar_t file[MAX_PATH] = L"fastfind-结果.csv";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_hMain;
    ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv"; ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    FILE* f = _wfopen(file, L"w, ccs=UTF-8");
    if (!f) { MessageBoxW(g_hMain, L"无法创建文件。", L"fastfind", MB_OK | MB_ICONERROR); return; }
    auto esc = [](const wchar_t* s, wchar_t* out, size_t cch) {
        size_t o = 0;
        out[o++] = L'"';
        for (size_t i = 0; s[i] && o + 2 < cch; i++) {
            if (s[i] == L'"') out[o++] = L'"';
            out[o++] = s[i];
        }
        out[o++] = L'"'; out[o] = 0;
    };
    fwprintf(f, L"名称,路径,大小,修改时间\n");
    wchar_t nm[MAX_PATH], pt[MAX_PATH * 2];
    for (const Hit* h : g_results) {
        size_t s = h->path.rfind(L'\\');
        esc(s == wstring::npos ? h->path.c_str() : h->path.c_str() + s + 1, nm, _countof(nm));
        esc(h->path.c_str(), pt, _countof(pt));
        wchar_t tm[40]{}; fmtTime(h->mtime, tm, 40);
        fwprintf(f, L"%s,%s,%llu,%s\n", nm, pt, (unsigned long long)h->size, tm);
    }
    fclose(f);
    wchar_t msg[160];
    _snwprintf(msg, 160, L"已导出 %zu 条结果到 %s", g_results.size(), file);
    MessageBoxW(g_hMain, msg, L"fastfind", MB_OK | MB_ICONINFORMATION);
}

/* layout the top row: the search edit matches the combo's actual visible
   height (a CBS_DROPDOWNLIST sizes itself from the font; a fixed 30px edit
   next to it looks misaligned) */
static void layoutTopRow(int W) {
    int th = 30;
    if (g_filterBarVisible && g_hFilter) {
        RECT rf{};
        GetWindowRect(g_hFilter, &rf);
        int ch = rf.bottom - rf.top;
        if (ch >= 12) th = ch;
        MoveWindow(g_hFilter, 10, 8, 130, 260, TRUE);
        MoveWindow(g_hEdit, 150, 8, max(60, W - 160), th, TRUE);
    } else {
        MoveWindow(g_hEdit, 10, 8, max(60, W - 20), th, TRUE);
    }
}

static int g_baseFontH = 0;
static void applyFonts(int zoomDelta) {
    g_zoom += zoomDelta;
    if (g_zoom < -4) g_zoom = -4;
    if (g_zoom > 8)  g_zoom = 8;
    if (g_font) DeleteObject(g_font);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    if (!g_baseFontH) g_baseFontH = ncm.lfMessageFont.lfHeight;
    ncm.lfMessageFont.lfHeight = g_baseFontH - g_zoom * 2;
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : { g_hEdit, g_hList, g_hFilter, g_hStatus })
        if (h) SendMessage(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_hList) ListView_Arrange(g_hList, LVA_DEFAULT);
    if (g_hMain) {                       // combo re-sized itself for the new font
        RECT rc; GetClientRect(g_hMain, &rc);
        layoutTopRow(rc.right);
    }
}

static void invertSelection() {
    vector<int> sel;
    int i = -1;
    while ((i = ListView_GetNextItem(g_hList, i, LVNI_SELECTED)) >= 0) sel.push_back(i);
    // virtual listview: toggle by selecting everything then deselecting old set
    ListView_SetItemState(g_hList, -1, LVIS_SELECTED, LVIS_SELECTED);
    for (int j : sel) ListView_SetItemState(g_hList, j, 0, LVIS_SELECTED);
}

static void copySelectionToClipboard(bool namesOnly) {
    wstring text;
    int i = -1;
    while ((i = ListView_GetNextItem(g_hList, i, LVNI_SELECTED)) >= 0 &&
           (size_t)i < g_results.size()) {
        const Hit* h = g_results[i];
        if (!text.empty()) text += L"\r\n";
        if (namesOnly) {
            size_t s = h->path.rfind(L'\\');
            text += s == wstring::npos ? h->path : h->path.substr(s + 1);
        } else text += h->path;
    }
    if (text.empty() || !OpenClipboard(g_hMain)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
}

static void pasteIntoSearch() {
    if (!OpenClipboard(g_hMain)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* s = (const wchar_t*)GlobalLock(h);
        if (s) {
            wstring t(s);
            GlobalUnlock(h);
            for (auto& c : t) if (c == L'\r' || c == L'\n') c = L' ';
            SetWindowTextW(g_hEdit, t.c_str());
            requestSearch();
        }
    }
    CloseClipboard();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hMenu = buildMenuBar();
        SetMenu(hwnd, g_hMenu);
        initAccelerators();
        /* filter combo + search edit (Everything's toolbar row) */
        g_hFilter = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            10, 8, 130, 240, hwnd, (HMENU)1004, nullptr, nullptr);
        static const wchar_t* filterItems[] = {
            L"所有", L"音频", L"压缩文件", L"文档",
            L"可执行文件", L"文件夹", L"图片", L"视频" };
        for (auto* s : filterItems) SendMessageW(g_hFilter, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(g_hFilter, CB_SETCURSEL, 0, 0);
        g_hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            150, 8, 620, 30, hwnd, (HMENU)1001, nullptr, nullptr);
        g_hList = CreateWindowExW(0, WC_LISTVIEW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
            10, 44, 760, 440, hwnd, (HMENU)1002, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        /* system shell icons in the results (folders get the folder icon,
           files the icon of their type): bind the shared system image list
           and serve per-item icon indexes on demand */
        SHFILEINFOW sfiIl{};
        HIMAGELIST sysIl = (HIMAGELIST)SHGetFileInfoW(L"", 0, &sfiIl, sizeof sfiIl,
                                                      SHGFI_SMALLICON | SHGFI_SYSICONINDEX);
        if (sysIl)
            ListView_SetImageList(g_hList, sysIl, LVSIL_SMALL);
        LVCOLUMNW col;
        int widths[] = { 260, 500, 90, 145 };
        const wchar_t* titles[] = { L"名称", L"路径", L"大小", L"修改时间" };
        for (int i = 0; i < 4; i++) {
            memset(&col, 0, sizeof col);
            col.mask = LVCF_TEXT | LVCF_WIDTH | (i == 2 ? LVCF_FMT : 0);
            col.pszText = (LPWSTR)titles[i]; col.cx = widths[i];
            if (i == 2) col.fmt = LVCFMT_RIGHT;
            ListView_InsertColumn(g_hList, i, &col);
        }
        updateHeaderArrows();
        g_hStatus = CreateWindowExW(0, STATUSCLASSNAME, L"正在从数据库加载索引…",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd, (HMENU)1003, nullptr, nullptr);
        SendMessage(g_hEdit, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessage(g_hFilter, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessage(g_hList, WM_SETFONT, (WPARAM)g_font, TRUE);
        { RECT rc; GetClientRect(hwnd, &rc); layoutTopRow(rc.right); }
        EnableWindow(g_hEdit, FALSE);
        SetTimer(hwnd, IDT_DRAIN, 300, nullptr);
        SetTimer(hwnd, IDT_IDLE, 1000, nullptr);
        thread sw(searchWorker, hwnd); sw.detach();   // async search worker
        thread bt(buildThread, hwnd); bt.detach();
        return 0;
    }
    case WM_SIZE: {
        RECT rs; GetWindowRect(g_hStatus, &rs);
        int sh = rs.bottom - rs.top, W = LOWORD(lp), H = HIWORD(lp);
        layoutTopRow(W);
        MoveWindow(g_hList, 10, 44, W - 20, max(40, H - 52 - sh), TRUE);
        SendMessage(g_hStatus, WM_SIZE, wp, lp);
        updateStatusBar();
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_DRAIN) {
            applyPending();
            if (!g_ready.load()) {
                updateStatusBar();       // keep the startup phase text current
            } else if (g_dirtyLive.load() && GetWindowTextLengthW(g_hEdit) > 0) {
                /* live refresh while typing, throttled to 1/s: without the
                   throttle every drain restarts a full background scan */
                static DWORD lastDrainSearch = 0;
                if (GetTickCount() - lastDrainSearch > 1000) {
                    lastDrainSearch = GetTickCount();
                    requestSearch();
                }
            } else if (g_dirtyLive.load())
                updateStatusBar();
        } else if (wp == IDT_IDLE) {
            /* cooldown: during heavy USN churn rebuilds would otherwise run
               back-to-back at full CPU; 30s between rebuilds is plenty */
            if (g_ready.load() && !g_rebuilding.load() &&
                (g_forceRebuild.load() ||
                 (g_dirtyLive.load() && GetTickCount() - g_lastType > 3000 &&
                  GetTickCount() - g_lastRebuildTick.load() > 30000))) {
                g_forceRebuild = false;
                thread rt(rebuildThread, hwnd); rt.detach();
            }
            /* periodic cache flush: bounds how much of the downtime has to be
               replayed from the USN journal after a crash or power loss */
            static DWORD lastSave = 0;
            if (g_ready.load() && !g_rebuilding.load() && !g_saveBusy.load() &&
                g_dirtyLive.load() && GetTickCount() - lastSave > 300000) {
                lastSave = GetTickCount();
                saveCacheBg();
            }
        } else if (wp == IDT_SEARCH) {
            KillTimer(hwnd, IDT_SEARCH);
            requestSearch();
        } else if (wp == IDT_SELSTAT) {
            KillTimer(hwnd, IDT_SELSTAT);
            updateStatusBar();
        }
        return 0;
    case WM_APP + 1: {                             // index ready
        EnableWindow(g_hEdit, TRUE);
        SetFocus(g_hEdit);
        updateStatusBar();
        /* a folder passed via -p (Explorer context menu) scopes the search:
           prefill the box with "C:\folder\" as a path filter term */
        if (!g_initialDir.empty()) {
            wstring q = g_initialDir;
            if (!q.empty() && q.back() != L'\\' && q.back() != L':') q += L'\\';
            SetWindowTextW(g_hEdit, q.c_str());
        }
        requestSearch();
        return 0;
    }
    case WM_APP + 2:                               // rebuild finished / started
        if (!g_rebuilding.load()) requestSearch();
        return 0;
    case WM_APP + 3: {                             // search results arrived (worker)
        SearchOutput* out = (SearchOutput*)lp;
        /* swap on the UI thread; LVN_GETDISPINFO also runs here, so the
           result list is never read while being replaced */
        g_resultMaster = out->master;
        g_resultLive = out->live;
        g_results.swap(out->items);
        ListView_SetItemCountEx(g_hList, (int)g_results.size(),
                                LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        InvalidateRect(g_hList, nullptr, FALSE);
        updateStatusBar();
        delete out;
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 1001 && HIWORD(wp) == EN_CHANGE) {
            g_lastType = GetTickCount();
            SetTimer(hwnd, IDT_SEARCH, 80, nullptr);   // near-instant as-you-type search
        } else if (id == 1004 && HIWORD(wp) == CBN_SELCHANGE) {
            g_filterIdx = (int)SendMessageW(g_hFilter, CB_GETCURSEL, 0, 0);
            if (g_filterIdx < 0) g_filterIdx = 0;
            requestSearch();
        } else if (id == IDM_TRAY_SHOW) {
            showMainWindow();
        } else if (id == IDM_TRAY_REBUILD || id == IDM_TOOL_REBUILD) {
            if (g_ready.load() && !g_rebuilding.load())
                { thread rt(fullRescanThread, hwnd); rt.detach(); }
        } else if (id == IDM_TRAY_EXIT || id == IDM_FILE_EXIT) {
            trayRemove();
            DestroyWindow(hwnd);
        } else if (id == IDM_OPEN || id == IDM_OPENPATH ||
                   id == IDM_COPY_PATH || id == IDM_PROPS || id == IDM_COPY_NAME) {
            handleResultCommand(id);
        } else if (id == IDM_INSTALL_MENU || id == IDM_UNINSTALL_MENU ||
                   id == IDM_TOOL_INSTALL || id == IDM_TOOL_UNINSTALL) {
            bool install = (id == IDM_INSTALL_MENU || id == IDM_TOOL_INSTALL);
            bool ok = setShellMenu(install);
            MessageBoxW(hwnd,
                ok ? (install ? L"已注册资源管理器右键菜单：在文件夹或驱动器上右键即可"
                              L"“用 fastfind 搜索”，并将搜索范围限定为该目录。"
                            : L"已移除资源管理器右键菜单。")
                   : L"操作失败：需要管理员权限。",
                L"fastfind", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        } else if (id == IDM_FILE_NEWWINDOW) {
            /* Everything opens a fresh search window; fastfind keeps one
               tray-resident index, so this resets the current search instead */
            SetWindowTextW(g_hEdit, L"");
            SendMessageW(g_hFilter, CB_SETCURSEL, 0, 0);
            g_filterIdx = 0;
            SetFocus(g_hEdit);
            requestSearch();
        } else if (id == IDM_FILE_CLOSE) {
            SendMessageW(hwnd, WM_CLOSE, 0, 0);       // hide to tray
        } else if (id == IDM_FILE_EXPORT) {
            exportResults();
        } else if (id == IDM_EDIT_COPY) {
            copySelectionToClipboard(false);
        } else if (id == IDM_EDIT_PASTE) {
            pasteIntoSearch();
        } else if (id == IDM_EDIT_SELECTALL) {
            SetFocus(g_hList);
            ListView_SetItemState(g_hList, -1, LVIS_SELECTED, LVIS_SELECTED);
        } else if (id == IDM_EDIT_INVERT) {
            invertSelection();
        } else if (id == IDM_VIEW_FILTERS) {
            g_filterBarVisible = !g_filterBarVisible;
            ShowWindow(g_hFilter, g_filterBarVisible ? SW_SHOW : SW_HIDE);
            RECT rc; GetClientRect(hwnd, &rc);
            SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
        } else if (id == IDM_VIEW_STATUSBAR) {
            g_statusBarVisible = !g_statusBarVisible;
            ShowWindow(g_hStatus, g_statusBarVisible ? SW_SHOW : SW_HIDE);
        } else if (id == IDM_VIEW_ZOOM_IN) {
            applyFonts(+1);
        } else if (id == IDM_VIEW_ZOOM_OUT) {
            applyFonts(-1);
        } else if (id == IDM_VIEW_ZOOM_RESET) {
            g_zoom = 1; applyFonts(-1);               // snap back to 0
        } else if (id == IDM_VIEW_WIN_SMALL) {
            ShowWindow(hwnd, SW_RESTORE); SetWindowPos(hwnd, nullptr, 0, 0, 660, 480, SWP_NOMOVE | SWP_NOZORDER);
        } else if (id == IDM_VIEW_WIN_MEDIUM) {
            ShowWindow(hwnd, SW_RESTORE); SetWindowPos(hwnd, nullptr, 0, 0, 1024, 640, SWP_NOMOVE | SWP_NOZORDER);
        } else if (id == IDM_VIEW_WIN_LARGE) {
            ShowWindow(hwnd, SW_MAXIMIZE);
        } else if (id == IDM_VIEW_WIN_AUTO) {
            ShowWindow(hwnd, SW_RESTORE); SetWindowPos(hwnd, nullptr, 0, 0, 1310, 664, SWP_NOMOVE | SWP_NOZORDER);
        } else if (id == IDM_VIEW_REFRESH) {
            requestSearch();
        } else if (id >= IDM_SORT_BASE && id < IDM_SORT_BASE + 5) {
            int field = id - IDM_SORT_BASE;
            applySortNow(field, field == g_sortField ? !g_sortAsc : true);
        } else if (id == IDM_SORT_ASC || id == IDM_SORT_DESC) {
            applySortNow(g_sortField, id == IDM_SORT_ASC);
        } else if (id == IDM_TOP_NEVER || id == IDM_TOP_ALWAYS) {
            g_topmost = (id == IDM_TOP_ALWAYS);
            SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        } else if (id == IDM_SEARCH_CASE) {
            g_matchCase = !g_matchCase; requestSearch();
        } else if (id == IDM_SEARCH_WORD) {
            g_matchWord = !g_matchWord; requestSearch();
        } else if (id == IDM_SEARCH_PATH) {
            g_matchPathOpt = !g_matchPathOpt; requestSearch();
        } else if (id >= IDM_FILTER_BASE && id < IDM_FILTER_BASE + 8) {
            g_filterIdx = id - IDM_FILTER_BASE;
            SendMessageW(g_hFilter, CB_SETCURSEL, g_filterIdx, 0);
            requestSearch();
        } else if (id == IDM_HELP_HELP || id == IDM_HELP_SYNTAX) {
            if (id == IDM_HELP_HELP) {
                MessageBoxW(hwnd,
                    L"顶部输入即搜，结果实时更新；双击打开，右键更多操作。",
                    L"fastfind 帮助", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd,
                    L"搜索语法\n\n"
                    L"foo bar     多个关键词 AND，默认只匹配文件名\n"
                    L"\"foo bar\"   引号短语（空格保留，整词匹配）\n"
                    L"path:foo    匹配整个路径\n"
                    L"C:\\tools\\   含 \\ 的关键词限定目录（相对亦可：download\\）\n"
                    L"*.cpp ext:txt  扩展名（ext: 支持 a;b 多值）\n"
                    L"file: folder:  只要文件 / 只要文件夹\n"
                    L"!foo        排除（可与 path:、*.tmp 组合）\n\n"
                    L"搜索菜单：Ctrl+I 区分大小写、Ctrl+B 全字匹配、Ctrl+U 匹配路径",
                    L"fastfind 搜索语法", MB_OK | MB_ICONINFORMATION);
            }
        } else if (id == IDM_HELP_CMDLINE) {
            MessageBoxW(hwnd,
                L"fastfindui.exe [-install-menu | -uninstall-menu]\n"
                L"              [-p <folder>]\n\n"
                L"-install-menu / -uninstall-menu\n"
                L"    注册 / 移除资源管理器右键菜单（需管理员）\n"
                L"-p <folder>\n"
                L"    启动时将搜索范围限定为该目录",
                L"fastfind 命令行选项", MB_OK | MB_ICONINFORMATION);
        } else if (id == IDM_HELP_ABOUT) {
            MessageBoxW(hwnd,
                L"fastfind " FF_VER_WSTRING L"\n\n"
                L"即时文件名搜索工具。",
                L"关于 fastfind", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    case WM_INITMENUPOPUP: {
        HMENU m = (HMENU)wp;
        /* keep check marks in sync, exactly like Everything's live menus */
        for (int i = 0; i < GetMenuItemCount(m); i++) {
            MENUITEMINFOW mi{ sizeof(mi), MIIM_ID | MIIM_SUBMENU };
            GetMenuItemInfoW(m, i, TRUE, &mi);
            if (mi.hSubMenu || !mi.wID) continue;
            UINT check = MF_BYCOMMAND | MF_UNCHECKED;
            int id = (int)mi.wID;
            if ((id == IDM_SEARCH_CASE && g_matchCase) ||
                (id == IDM_SEARCH_WORD && g_matchWord) ||
                (id == IDM_SEARCH_PATH && g_matchPathOpt) ||
                (id == IDM_VIEW_FILTERS && g_filterBarVisible) ||
                (id == IDM_VIEW_STATUSBAR && g_statusBarVisible) ||
                (id == IDM_SORT_ASC && g_sortAsc) ||
                (id == IDM_SORT_DESC && !g_sortAsc) ||
                (id == IDM_TOP_ALWAYS && g_topmost) ||
                (id == IDM_TOP_NEVER && !g_topmost) ||
                (id >= IDM_SORT_BASE && id < IDM_SORT_BASE + 5 &&
                 id - IDM_SORT_BASE == g_sortField))
                check = MF_BYCOMMAND | MF_CHECKED;
            CheckMenuItem(g_hMenu, id, check);
        }
        return 0;
    }
    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            showMainWindow();
        } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            trayMenu();
        }
        return 0;
    case WM_SHOWEXISTING:
        showMainWindow();
        return 0;
    case WM_CONTEXTMENU:
        /* right-click on a search result: Everything-style menu with
           open / open-path (jump to folder) / copy path / properties */
        if ((HWND)wp == g_hList) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            if (x != -1 || y != -1) {
                POINT pt{ x, y };
                POINT cli = pt;
                ScreenToClient(g_hList, &cli);
                LVHITTESTINFO ht{}; ht.pt = cli;
                int item = ListView_HitTest(g_hList, &ht);
                if (item >= 0 && (ht.flags & LVHT_ONITEM) &&
                    ListView_GetItemState(g_hList, item, LVIS_SELECTED) == 0) {
                    ListView_SetItemState(g_hList, -1, 0, LVIS_SELECTED);      // clear
                    ListView_SetItemState(g_hList, item, LVIS_SELECTED, LVIS_SELECTED);
                }
            }
            int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
            if (sel < 0) break;                          // nothing selected: default
            if (x == -1 && y == -1) {                    // keyboard (Shift+F10/Menu)
                RECT rc;
                if (!ListView_GetItemRect(g_hList, sel, &rc, LVIR_BOUNDS)) break;
                POINT p{ rc.left + 10, (rc.top + rc.bottom) / 2 };
                ClientToScreen(g_hList, &p);
                x = p.x; y = p.y;
            }
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, IDM_OPEN,      L"打开(&O)");
            AppendMenuW(m, MF_STRING, IDM_OPENPATH,  L"打开路径(&J)...");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_COPY_PATH, L"复制完整路径(&C)");
            AppendMenuW(m, MF_STRING, IDM_COPY_NAME, L"复制文件名(&N)");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_PROPS,     L"属性(&R)");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            DestroyMenu(m);
            return 0;
        }
        return 0;
    case WM_CLOSE: {
        /* ask once (or follow the remembered choice): minimize to tray or exit */
        int action = g_closeAction;
        if (action == 0) {
            TASKDIALOG_BUTTON btns[] = {
                { 1001, L"最小化到系统托盘(&M)" },
                { 1002, L"退出程序(&X)" },
            };
            TASKDIALOGCONFIG td{};
            td.cbSize = sizeof td;
            td.hwndParent = hwnd;
            td.pszWindowTitle = L"fastfind";
            td.pszMainIcon = TD_INFORMATION_ICON;
            td.pszContent = L"要退出 fastfind，还是最小化到系统托盘？";
            td.pButtons = btns;
            td.cButtons = 2;
            td.dwCommonButtons = TDCBF_CANCEL_BUTTON;
            td.pszVerificationText = L"记住我的选择，以后不再询问(&R)";
            int clicked = 0, radio = 0;
            BOOL remember = FALSE;
            /* pfVerificationFlagChecked must be non-NULL, otherwise Windows
               disables the checkbox entirely */
            if (SUCCEEDED(TaskDialogIndirect(&td, &clicked, &radio, &remember)) &&
                (clicked == 1001 || clicked == 1002)) {
                g_closeAction = clicked - 1000;
                if (remember) saveCloseAction(g_closeAction);
            } else {
                return 0;                 // cancelled (Esc): keep the window open
            }
            action = g_closeAction;
        }
        if (action == 1) {
            /* tray mode: the index stays in memory and the USN watch keeps it
               current, so reopening is instant (no rescan) */
            ShowWindow(hwnd, SW_HIDE);
            trayAdd(L"fastfind — 文件搜索（左键恢复，右键退出）");
        } else {
            trayRemove();
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            trayRemove();
            applyPending();               // flush USN records, then persist for next start
            saveCacheAndWait();
            g_quit = true;
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == 1002 && nm->code == LVN_GETDISPINFO) {
            NMLVDISPINFOW* di = (NMLVDISPINFOW*)nm;
            if (di->item.iItem < 0 || (size_t)di->item.iItem >= g_results.size()) return 0;
            const Hit* h = g_results[di->item.iItem];
            if (di->item.iSubItem == 0 && (di->item.mask & LVIF_IMAGE))
                di->item.iImage = iconFor(h);
            wchar_t* dst = di->item.pszText;
            size_t cch = di->item.cchTextMax;
            if (!(di->item.mask & LVIF_TEXT) || !dst || cch < 2) return 0;
            size_t p = h->path.rfind(L'\\');
            switch (di->item.iSubItem) {
            case 0:
                copyText(dst, p == wstring::npos ? h->path.c_str() : h->path.c_str() + p + 1, cch);
                break;
            case 1:
                copyText(dst, h->path.c_str(), cch);
                break;
            case 2:
                if (h->isDir) dst[0] = 0; else fmtSize(h->size, dst, cch);
                break;
            case 3: fmtTime(h->mtime, dst, cch); break;
            }
        } else if (nm->idFrom == 1002 && nm->code == LVN_COLUMNCLICK) {
            /* click a column header: sort by it; clicking again reverses */
            NMLISTVIEW* lv = (NMLISTVIEW*)nm;
            applySortNow(lv->iSubItem, lv->iSubItem == g_sortField ? !g_sortAsc : true);
        } else if (nm->idFrom == 1002 && nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* lv = (NMLISTVIEW*)nm;
            if (lv->uChanged & LVIF_STATE &&
                (lv->uNewState ^ lv->uOldState) & LVIS_SELECTED)
                SetTimer(hwnd, IDT_SELSTAT, 100, nullptr);   // batch the storm
        } else if (nm->idFrom == 1002 && nm->code == NM_DBLCLK) {
            NMITEMACTIVATE* ia = (NMITEMACTIVATE*)nm;
            if (ia->iItem >= 0 && (size_t)ia->iItem < g_results.size())
                openHit(g_results[ia->iItem]);
        }
        return 0;
    }
    case WM_DESTROY:
        trayRemove();
        applyPending();                   // position must match the saved node maps
        saveCacheAndWait();
        if (g_hAccel) DestroyAcceleratorTable(g_hAccel);
        g_quit = true;
        g_cvSearch.notify_all();                   // wake the search worker so it exits
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Ask a live instance to show itself. Returns false when the window is gone
   or its UI thread is blocked (exit saves the index inside WM_DESTROY, so the
   HWND can still exist while nothing is on screen). A false result must not
   be silent — the caller shows a window explaining the lock. */
static bool revealExisting(HWND h) {
    if (!h || !IsWindow(h)) return false;
    if (!IsWindowVisible(h) || IsIconic(h)) {
        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(h, WM_SHOWEXISTING, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result))
            return false;
    }
    DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD tg = GetWindowThreadProcessId(h, nullptr);
    if (fg && fg != tg) AttachThreadInput(fg, tg, TRUE);
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    ShowWindow(h, SW_SHOW);
    if (!SetForegroundWindow(h)) FlashWindow(h, TRUE);
    BringWindowToTop(h);
    if (fg && fg != tg) AttachThreadInput(fg, tg, FALSE);
    return IsWindowVisible(h) != FALSE;
}

static HANDLE g_busyMutex = nullptr;
static bool g_busyGot = false;

static wstring otherFastfindPids() {
    wstring s;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return s;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof pe;
    DWORD self = GetCurrentProcessId();
    int n = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) continue;
            if (_wcsicmp(pe.szExeFile, L"fastfindui.exe")) continue;
            wchar_t line[80];
            _snwprintf(line, 80, L"fastfindui.exe（PID %lu）\n", pe.th32ProcessID);
            s += line;
            if (++n >= 8) break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return s;
}

static LRESULT CALLBACK BusyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm{ sizeof ncm };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)font);
        const wchar_t* text = (const wchar_t*)((CREATESTRUCTW*)lp)->lpCreateParams;
        HWND st = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  18, 16, 484, 168, hwnd, nullptr, nullptr, nullptr);
        HWND btn = CreateWindowExW(0, L"BUTTON", L"取消",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                   210, 192, 100, 30, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
        SendMessageW(st, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(btn, WM_SETFONT, (WPARAM)font, TRUE);
        SetTimer(hwnd, 1, 200, nullptr);
        return 0;
    }
    case WM_TIMER: {
        DWORD wr = WaitForSingleObject(g_busyMutex, 0);
        if (wr == WAIT_OBJECT_0 || wr == WAIT_ABANDONED) {
            g_busyGot = true;
            KillTimer(hwnd, 1);
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        HFONT font = (HFONT)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (font) DeleteObject(font);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Lock is held and no usable window is on screen. Show that immediately —
   a silent wait looks like the double-click did nothing. When the owner
   releases the mutex (index save finished, or the process exited), close
   this notice and let startup continue. */
static bool waitWhileBusy(HINSTANCE hInst, HANDLE single) {
    g_busyMutex = single;
    g_busyGot = false;
    wstring pids = otherFastfindPids();
    wstring text =
        L"fastfind 的启动锁仍被占用，所以这次没有直接打开新窗口。\n\n"
        L"窗口和托盘这时可能已经关闭，进程还在把索引写回磁盘。"
        L"写完后这个提示会关掉，并自动打开 fastfind。\n\n";
    if (!pids.empty()) {
        text += L"仍在运行：\n";
        text += pids;
        text += L"\n如果一直停在这里，请到任务管理器结束上面的进程，或点“取消”。";
    } else {
        text += L"没有找到 fastfindui.exe。锁可能马上会释放；"
                L"若不想等，点“取消”后再试。";
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = BusyProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"fastfindui-busy";
    wc.hIcon = LoadIcon(nullptr, IDI_INFORMATION);
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    DWORD ex = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
    RECT rc{ 0, 0, 520, 240 };
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(ex, wc.lpszClassName, L"fastfind", style,
                                x, y, w, h, nullptr, nullptr, hInst,
                                (LPVOID)text.c_str());
    if (!hwnd) {
        MessageBoxW(nullptr, text.c_str(), L"fastfind", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return g_busyGot;
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    /* command line:
       -install-menu / -uninstall-menu   register/remove the Explorer context
                                        menu entry and exit (needs admin)
       -p <folder>                      start with the search scoped to <folder> */
    {
        int argcW = 0;
        LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
        for (int i = 1; argvW && i < argcW; i++) {
            if (!wcscmp(argvW[i], L"-install-menu") || !wcscmp(argvW[i], L"-uninstall-menu")) {
                bool install = !wcscmp(argvW[i], L"-install-menu");
                bool ok = setShellMenu(install);
                MessageBoxW(nullptr,
                    ok ? (install ? L"已注册资源管理器右键菜单。"
                                  : L"已移除资源管理器右键菜单。")
                       : L"操作失败：需要管理员权限。",
                    L"fastfind", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
                LocalFree(argvW);
                return 0;
            }
            if (!wcscmp(argvW[i], L"-p") && i + 1 < argcW)
                g_initialDir = argvW[++i];
        }
        if (argvW) LocalFree(argvW);
    }
    /* single instance. Ownership, not the name, means "running": exit drops
       the window and tray first, then keeps the mutex while it writes the
       index. If that owner still has a window that can be shown, show it.
       Otherwise put a notice on screen immediately — waiting in silence looks
       like the double-click was ignored. The notice closes itself and startup
       continues once the owner releases the mutex. */
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\fastfindui-single-instance");
    if (!single) {
        MessageBoxW(nullptr, L"无法启动 fastfind（创建实例锁失败）。",
                    L"fastfind", MB_OK | MB_ICONERROR);
        return 0;
    }
    DWORD wr = WaitForSingleObject(single, 0);
    if (wr != WAIT_OBJECT_0 && wr != WAIT_ABANDONED) {
        HWND other = FindWindowW(L"fastfindui", nullptr);
        if (other && revealExisting(other)) {
            CloseHandle(single);
            return 0;
        }
        if (!waitWhileBusy(hInst, single)) {
            CloseHandle(single);
            return 0;
        }
    }
    if (const char* lg = getenv("FASTFINDUI_LOG")) {
        wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, lg, -1, w, MAX_PATH);
        g_log = _wfopen(w, L"a");
    }
    g_closeAction = loadCloseAction();
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"fastfindui";
    RegisterClassW(&wc);
    g_hMain = CreateWindowExW(0, L"fastfindui",
        L"fastfind",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 680,
        nullptr, nullptr, hInst, nullptr);
    HICON smIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (smIcon) SendMessageW(g_hMain, WM_SETICON, ICON_SMALL, (LPARAM)smIcon);
    ShowWindow(g_hMain, nShow);
    UpdateWindow(g_hMain);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_hAccel && TranslateAcceleratorW(g_hMain, g_hAccel, &msg))
            continue;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            msg.hwnd == g_hList) {
            int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
            if (sel >= 0 && (size_t)sel < g_results.size()) openHit(g_results[sel]);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_log) fclose(g_log);
    ReleaseMutex(single);
    CloseHandle(single);
    return (int)msg.wParam;
}
