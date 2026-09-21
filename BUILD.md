# Building

[Back to ChatLogFix](README.md)

For normal installation, download the plugin ZIP from the release page. These steps are for building the source.

## Requirements

- Windows with Visual Studio 2022 and its C++ tools.
- CMake 3.22 or newer.
- The Ashita v4 SDK folder containing `Ashita.h`.

## Build

From the project folder in Command Prompt:

```bat
set ASHITA4_SDK_PATH=C:\path\to\ashita-sdk
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

CMake builds for **32-bit x86** and writes `build\Release\chatlogfix.dll`.

Fully close FFXI before replacing `/ashita/plugins/chatlogfix.dll`, then relaunch and load the plugin.

## Release documentation

Edit the root `README.md` for GitHub. Both release workflows generate a plain Markdown `docs/chatlogfix/README.md` inside the ZIP: badges become links, image headings become text, and feature dropdowns are expanded. The source README stays unchanged.

To preview the packaged README in PowerShell:

```powershell
./.github/scripts/export-readme.ps1 -Output build/README.release.md
```

The README is read from the revision being packaged. Documentation edits need to be included in that revision before preparing its release files; existing ZIPs do not update automatically.

## If the signatures stop matching

This section covers the base expanded-log fix. Fill mode and chat serialization have additional checks in the source; these two patterns do not verify the entire plugin.

The two base-fix patch sites are found by scanning `FFXiMain.dll` for these 23-byte patterns, taking the byte
immediately after each:

```
A:  66 01 85 2A 40 06 00  8B 4D 1C  66 8B 95 2A 40 06 00  03 C8  66 83 FA  [??]
B:  66 01 85 2A 40 06 00  8B 55 1C  66 8B 8D 2A 40 06 00  03 D0  66 83 F9  [??]
```

Both decode as: accumulate the just-inserted line count into `[ebp+6402Ah]`, load it into a 16-bit
register, then compare against the stop value. The trailing byte is the stop value and is what gets
written; it is wildcarded so the signature still matches its own site once patched.

If a client update moves this code, re-derive the patterns from the same instruction sequence. Keep
excluding the `call rel32` that precedes them — a relative call encodes a distance and changes whenever
anything between the caller and its target moves, which would make the signature fail for a reason that
has nothing to do with the code you care about.

## Why the scanner walks the section table

FFXiMain.dll is POL-packed, and its PE header's `BaseOfCode` / `SizeOfCode` describe the **packer
section**, not the game:

```
BaseOfCode 0x009CB000, SizeOfCode 0x001E6000   -> the POL1 section
.text      0x00001000, vsize      0x00326FEE   -> the real code, where the patch sites live
```

The first version of this plugin scanned `[BaseOfCode, BaseOfCode + SizeOfCode)` — the obvious,
usually-correct idiom — and therefore searched only the packer stub. It reported "could not locate the
chat rebuild bound" on a client where both signatures were present and correct.

The scanner now walks the section table and searches every section marked `IMAGE_SCN_MEM_EXECUTE`,
which covers `.text` and `POL1` both and cannot be misled the same way. If you write another tool that
scans this binary, do the same — the header fields are actively misleading here.
