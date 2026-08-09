# ChatLogFix v1.1 - Full Height Chat Log for Ashita v4

Fills the expanded chat log (fulllog) to the full height of its window instead of stopping halfway.

## Why this exists

FFXI wipes the chat render ring on every view reset and then repopulates it with only **50** lines. The ring holds **100**. The gap fills in on its own as new chat arrives, which is why the bug feels intermittent and is always worst right after you open or tab.

The fix is one byte in each of the two branches of the view-reset:

```
cmp  dx, 50        ->        cmp  dx, 99
```

That is the entire change.

## Requirements

- Ashita 4.3.1.2 (interface version 4.30) - the version ChatLogFix was built and tested against.

## Installation

Copy `chatlogfix.dll` into `Ashita-v4beta-main\plugins\`, then:

```
/load chatlogfix
```

The fix applies immediately on load. Add `/load chatlogfix` to your startup script to have it every
session.

## Commands

| Command | Description |
|---------|-------------|
| `/chatlogfix status` | What it is doing right now |
| `/chatlogfix diag` | Write a full report to `logs\chatlogfix_diag.log` |

`/clf` for short.

## What it does at your resolution

You do not have to pick any of this as the plugin measures and decides. The chart is here so you can
check it did the right thing. Your chat window holds `(viewport height / 16) - 5` rows.

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

**In short:**

| Viewport height | What you need |
|-----------------|---------------|
| up to **1679px** | nothing beyond the base fix. Fill mode declines silently - nothing is blank for it to fill. |
| **1680 - 3279px** | fill mode engages and fills the window exactly. |
| **3280px and up** | fill mode engages but stops at 199 lines; some rows stay blank. |


That 199 cap is a hard one: 200 records is the size of the client's own ring allocation, and there is
no larger number that is safe to write.

## Fill mode

The fix fills the render ring to its capacity of **100 records**, which completely fills any chat
window of 99 rows or fewer. That covers every display up to 1679px tall, so at 1080p and 1440p there
is no blank space left at all.

A taller window is taller than the ring can fill: at 4K that is 130 rows against 99 records, leaving
31 blank rows above the oldest line.

Fill mode raises the ring itself to match your window, up to **200 records**.

**It applies itself.** On load the plugin measures the window and does whatever that window needs, so
there is nothing to turn on and nothing to configure.

### How it reaches past 127

There is a ceiling in the way, and it is worth knowing about because it decides how much of your
client gets touched.

Six blocks of client code hold the ring's bound in a **sign-extended 8-bit immediate**, so 127 is
simply the largest number that fits in the space available. Below that it writes constants and
nothing else happens. At 128 and above there is no number to write, so it re-encodes those six blocks
with 32-bit immediates in a block of memory it allocates, and replaces each original site with a jump
to it.

Four of the six wrap or normalise a ring index. **The other two are the rebuild's own stop value**,
which is what decides how many lines a rebuild places - raising the ring without those two would buy
capacity nothing can fill.

You do not choose between these. It measures the window and takes the cheaper one whenever that
reaches, then says what it did. Either way every site is checked against its expected bytes
before anything is written, the whole set rolls back if any single write does not take, and the
allocated block is released only after all six sites are back to stock.

## How It Works

The base fix changes two bytes of client-side display logic in this process's memory, and puts them back when unloaded. (Fill mode changes more - see above.)

FFXI keeps chat in three layers:

- a **store** of retained history, backed by 20 page files per window on disk
- a 100-record **ring** holding what is currently on screen
- the **view** you scroll

Every time the view resets - opening fulllog, switching tab, zoning - the ring is cleared and rebuilt from the store. The rebuild accumulates a running line count and stops once it reaches 50, so half the window is left empty. This plugin raises that stop value to 99.

## Version history

See **CHANGELOG.md**.

## Thanks

- **The Ashita Team** - atom0s, thorny, and the Ashita Discord community

## License

MIT - see **LICENSE**.
