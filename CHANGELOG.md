# Changelog

[Back to ChatLogFix](README.md)

## v1.3

### Logs

- **One log per character**, `logs\chatlogfix\<Name>_<id>\chatlogfix.log`; lines from before login move into it at login.
- **Each log keeps its newest 1 MB**.
- **`diag` writes its report into your character's log** instead of a separate file.
- Every failure says so once in chat and names the log.
- The update deletes the old files: `logs\chatlogfix\chatlogfix.log`, `logs\chatlogfix\chatlogfix.log.old` and `logs\chatlogfix_diag.log`.

### Unloading

- **Unload leaves ChatLogFix's changes in place until the game closes.** Putting the chat ring back to its stock size
  is safe only if no game thread holds a ring index anywhere up its call stack, and pausing threads cannot see that (a
  routine that returns index 110 to a loop that then runs against the stock size of 100 never finishes). Everything that
  stays is self-contained - the memory blocks ChatLogFix adds never call back into it, and it keeps itself loaded - so
  chat keeps working exactly as before. Restart the game for the stock chat code.
- **`/load chatlogfix` after an unload takes those changes back over.** The same ChatLogFix stays in memory until the
  game closes, so a new build needs a game restart (ChatLogFix says so when the file on disk has changed). Another copy
  of ChatLogFix is refused.

### Safety

- **Every change to the game's code is made with the game's other threads paused,** at a moment when none of them is
  inside the chat code, either added memory block, or ChatLogFix itself. If no such moment comes within a second, the
  change is refused and reported.
- **ChatLogFix keeps itself loaded from its first change** (checked: if it cannot, it changes nothing).
- A site that holds another tool's bytes is left alone and reported.
- The thread pause skips threads that have already exited (one kept alive by another handle used to make every pause fail).
- `/clf diag` no longer reports chat serialization as active after a failed install.

## v1.2

Adds **chat serialization**: the client's chat buffer is written from one thread while the main
thread draws it, and a burst of colored text can make the buffer grow mid-draw. One recursive
spinlock now makes the append (writer) and the draw (reader) take turns, which stops the flood crash
and the garbled/blank chat lines.

- Teardown resets BOTH chat windows' rings (logwindo and logwin2) with read-back before narrowing the
  ring size; a ring that cannot be zeroed refuses the narrowing.
- The relocated-block cave is freed only when every block reads back as stock; a surviving block keeps
  the cave and blocks the base-fix restore, instead of leaving a jump into freed memory.

## v1.1

Adds **fill mode**, which fills chat windows taller than the base fix can reach. The base fix is
unchanged - the same two bytes, resolved the same way.

### Fill mode

- Raises the chat render ring to match your window, up to **200 records** - the size of the client's
  own ring allocation (`0x64058` = header + 200 x 0x800 + fields).
- **Only engages on a window holding more than 99 rows.** The base fix already fills any window of 99
  rows or fewer, which covers every display up to 1679px tall - so at 1080p and 1440p it does nothing extra.
- 33 constants across ten functions, all located by signature and all verified to hold their expected
  stock value before anything is written. Any site that does not resolve, or reads something
  unexpected, aborts the whole operation with nothing patched. Every write is read back, and a
  failure at any site rolls the entire set back.
- Nine of those are a **pair** per chat channel: the record counter's clamp test (`cmp dl,100`) and
  the value written when it fires (`mov byte [tbl],100`). Both have to move together - raising only
  the value makes a counter that reaches 101 get written as 127 rather than clamped.

### Reaching past 127

- **127 is an instruction encoding limit, not a design one.** Six blocks of client code hold the
  bound in a sign-extended `imm8`, so no larger number can be written into them.
- Below 128 the mode writes constants and relocates nothing. At 128 and above it re-encodes those six
  blocks with 32-bit immediates in an allocated cave and replaces each site with a jump. **It picks
  the cheaper mechanism itself** and reports which it used; there is nothing to choose.
- Four of the six wrap or normalise a ring index; **two are the rebuild's own stop value**, which is
  what decides how many lines a rebuild actually places. Raising the ring without those two buys
  capacity nothing can fill.
- Every block is verified against its stock bytes before anything is written, and the whole set rolls
  back if any single write does not take. The cave is freed only after all six are restored.
- Verified on a live client: the ring holds 200 records, the write index reaches 199, and it wraps
  199 -> 0.
- **Applied automatically on load, with nothing to configure.** `Initialize` measures the window and
  acts on it.
- Turning it off resets the ring's live indices before restoring its size.

### Interface

- **Two commands: `status` and `diag`.**
- `diag` writes to **`logs\chatlogfix_diag.log`**.

## v1.0

Initial release.
