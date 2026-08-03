# ChatLogFix v1.0 - Full-Height Chat Log for Ashita v4

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

The fix applies immediately on load. Add `/load chatlogfix` to your startup script to have it every session.

## Commands

| Command | Description |
|---------|-------------|
| `/chatlogfix on` | Apply the fix |
| `/chatlogfix off` | Restore the stock behaviour |
| `/chatlogfix status` | Report whether it is currently applied |
| `/chatlogfix diag` | Write a full diagnostic report to the Ashita log |

`/clf` is a short form of all of these. Since the fix applies automatically on load, these exist mainly to compare on and off in-session.

## How It Works

It changes two bytes of client-side display logic in this process's memory, and puts them back when unloaded.

FFXI keeps chat in three layers:

- a **store** of retained history, backed by 20 page files per window on disk
- a 100-record **ring** holding what is currently on screen
- the **view** you scroll

Every time the view resets - opening fulllog, switching tab, zoning - the ring is cleared and rebuilt from the store. The rebuild accumulates a running line count and stops once it reaches 50, so half the window is left empty. This plugin raises that stop value to 99.

Two details worth knowing, both of which are stock behaviour and unaffected by this plugin:

- It changes **nothing** about how much history the game retains. It only fills the window you are already looking at.
- The ring is a **snapshot**. It only updates live while you are sitting at the newest line; scrolled back, what you see is frozen until the next rebuild.

## Version history

See **CHANGELOG.md**.

## Thanks

- **The Ashita Team** - atom0s, thorny, and the Ashita Discord community

## License

MIT - see **LICENSE**.
