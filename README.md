# ChatLogFix v1.2 - Full-Height Chat Log + Chat Thread-Safety for Ashita v4

Fills the expanded chat log (fulllog) to the full height of its window, and serializes FFXI's
non-thread-safe chat fixing the chat buffer corruption.

## Features

- **Base fix** - the fulllog rebuild stops at 50 lines; it now stops at 99, which fills any window up
  to 1679px tall.
- **Fill mode** - on taller windows, raises the chat ring itself to match the window, up to 200 records.
- **Chat serialization** - one lock so chat is never written while the game is drawing or growing the
  buffer.

Everything applies itself on load. There is nothing to configure.

## Requirements

- Ashita 4.3.1.2 (interface version 4.30) - the version ChatLogFix was built and tested against.

## Installation

Copy `chatlogfix.dll` into `Ashita-v4beta-main\plugins\`, then:

```
/load chatlogfix
```

Add `/load chatlogfix` to your startup script to have it every session.

## Commands

| Command | Description |
|---------|-------------|
| `/chatlogfix status` | What it is doing right now |
| `/chatlogfix diag` | Write a full report to `logs\chatlogfix_diag.log` |

`/clf` for short.

## How it works

FFXI keeps chat in three layers:

- a **store** of retained history, backed by 20 page files per window on disk
- a 100-record **ring** holding what is currently on screen
- the **view** you scroll

### Base fix

Every time the view resets - opening fulllog, switching tab, zoning - the ring is cleared and rebuilt
from the store. The rebuild accumulates a running line count and stops once it reaches 50, so half the
window is left empty. The gap fills in on its own as new chat arrives, which is why the bug feels
intermittent and is always worst right after you open or tab.

The fix is one byte in each of the two branches of the view-reset:

```
cmp  dx, 50        ->        cmp  dx, 99
```

Those two bytes are put back on unload.

### Fill mode

The base fix fills the ring to its capacity of 100 records, which completely fills any chat window of
99 rows or fewer. A taller window is taller than the ring can fill: at 4K that is 130 rows against 99
records, leaving 31 blank rows above the oldest line. Fill mode raises the ring itself to match the
window, up to **200 records**.

Your chat window holds `(viewport height / 16) - 5` rows. The plugin measures and decides; the chart
is here so you can check it did the right thing.

| Display | Window holds | Stock | Base fix alone | + fill mode |
|---------|-------------:|------:|---------------:|-------------:|
| 1080p (1080) | 62 rows | 50, **12 blank** | **62 - full** | declines |
| 1200p (1200) | 70 rows | 50, **20 blank** | **70 - full** | declines |
| 1440p (1440) | 85 rows | 50, **35 blank** | **85 - full** | declines |
| 1600p (1600) | 95 rows | 50, **45 blank** | **95 - full** | declines |
| 1680 | 100 rows | 50, 50 blank | 99, **1 blank** | **100 - full** |
| 2112 | 127 rows | 50, 77 blank | 99, 28 blank | **127 - full** |
| 4K (2160) | 130 rows | 50, 80 blank | 99, 31 blank | **130 - full** |
| 5K (2880) | 175 rows | 50, 125 blank | 99, 76 blank | **175 - full** |
| 8K (4320) | 265 rows | 50, 215 blank | 99, 166 blank | 199, **66 blank** |

| Viewport height | What happens |
|-----------------|--------------|
| up to **1679px** | the base fix already fills the window; fill mode declines. |
| **1680 - 3279px** | fill mode engages and fills the window exactly. |
| **3280px and up** | fill mode engages but stops at 199 lines; some rows stay blank. |

That 199 cap is a hard one: 200 records is the size of the client's own ring allocation, and there is
no larger number that is safe to write.

**How it reaches past 127.** Six blocks of client code hold the ring's bound in a sign-extended 8-bit
immediate, so 127 is the largest number that fits. Below that it writes constants and nothing else
happens. At 128 and above it re-encodes those six blocks with 32-bit immediates in a block of memory it
allocates, and replaces each original site with a jump to it. Four of the six wrap or normalise a ring
index; the other two are the rebuild's own stop value, which is what decides how many lines a rebuild
places. It takes the cheaper mechanism whenever that reaches, then says what it did. Every site is
checked against its expected bytes before anything is written, the whole set rolls back if any single
write does not take, and the allocated block is released only after all six sites are back to stock.

### Chat serialization

Every bit of color in FFXI chat is an escape code stored in the chat buffer, so a burst of
heavily-colored text fills that buffer fast. Chat is written to it on a **background thread** while the
game's **main thread is drawing the same buffer every frame**. When a write lands in the middle of the
game's own work on that buffer - drawing it, or growing it to hold more - it corrupts: the client
crashes, or lines render garbled or blank.

The plugin puts a lock on the buffer, so the two threads take turns and only one touches it at a time.
It can't be corrupted mid-write, and both the crash and the garble stop. If a game update moves the code
it patches, serialization turns itself off, says so in chat, and everything else keeps working.

## Version history

See **CHANGELOG.md**.

## Thanks

- **The Ashita Team** - atom0s, thorny, and the Ashita Discord community

## License

MIT - see **LICENSE**.
