# Building chatlogfix

A prebuilt `plugins/chatlogfix.dll` is included, so you only need this if you want to change something.

## Requirements

- Windows, Visual Studio 2022 with the **x86** toolset (Ashita plugins are 32-bit)
- CMake 3.22+
- The Ashita v4 SDK — a folder containing `Ashita.h`

## Build

Point `ASHITA4_SDK_PATH` at the SDK, then:

```
set ASHITA4_SDK_PATH=C:\path\to\ashita-sdk
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `build\Release\chatlogfix.dll` — copy it into `Ashita-v4beta-main\plugins\`.

**`-A Win32` is required.** A 64-bit build will compile and then fail to load, because the FFXI client
and Ashita are both 32-bit.

Safe-SEH is deliberately left off; the legacy Direct3D8 import libraries are not `/SAFESEH`-compatible.
A linker `.map` is emitted so a crash address can be resolved to a function name.

## If the signatures stop matching

The two patch sites are found by scanning `FFXiMain.dll` for these 23-byte patterns, taking the byte
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
