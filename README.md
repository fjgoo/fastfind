<div align="center">

**English** | [简体中文](README.zh-CN.md)

</div>

# fastfind

A free, open-source instant filename search tool for Windows ([MIT License](LICENSE)). It reads the NTFS `$MFT` into memory and stays current from the USN journal.

If this tool is useful to you, please give it a **Star** on GitHub. That is the most direct way to support the project.

| Source | Program |
|---|---|
| `fastfind.cpp` | `fastfind.exe` (command line) |
| `fastfindui.cpp` | `fastfindui.exe` (GUI) |

![GUI screenshot](ui-screenshot2.png)

## Use

Double-click `fastfindui.exe` (reading a volume needs administrator rights). The first index takes about half a minute; later starts load from cache and replay USN changes that happened while the program was closed (a wrapped or recreated journal triggers an automatic full rescan). Type in the box to search; a space means all words must match. Results mix files and folders, and every row shows its shell icon. Creates, renames, and deletes show up within a few seconds. Double-click a row to reveal that file in Explorer. Closing the window asks between minimizing to the tray and exiting, and remembers the choice (left-click the tray to restore, right-click for its menu).

Right-click a folder to search inside it. Register or remove that menu from the tray, or run:

```
fastfindui.exe -install-menu
fastfindui.exe -uninstall-menu
fastfindui.exe -p "D:\some\directory"
```

## Query syntax

| Syntax | Meaning |
|---|---|
| `foo bar` | Every word must appear in the filename |
| `"foo bar"` | Treat the quoted text, spaces included, as one word |
| `path:foo` | Search the full path |
| `C:\tools\` | Limit the search to that directory |
| `*.cpp` / `ext:txt` | Filter by extension |
| `file:` / `folder:` | Files only, or folders only |
| `!foo` | Exclude |

The Search menu also toggles case sensitivity, whole-word matching, and path matching.

## Command line

```
fastfind --build [--fresh]      build or rebuild the index
fastfind <keyword> [keyword...] search once
fastfind -i [--watch]           interactive search; --watch enables live updates
```

The index cache is `%LOCALAPPDATA%\fastfind-index.bin`. Without administrator rights it falls back to a normal directory walk, which still works but is slower.

## Build

Requires MinGW-w64. Double-click `build.bat`, or run it in a terminal. Quit the running program from the tray before building.

## Notice

This project is completely free and open source under the [MIT License](LICENSE). Personal and commercial use are both free of charge. If you use or reference this project, in whole or in part, you must credit the source and keep the copyright and license notice. The software is provided as is. Reading NTFS normally requires administrator rights.
