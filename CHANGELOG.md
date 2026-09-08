# Changelog

## 1.2

Adds **chat serialization**: the client's chat buffer is written from one thread while the main
thread draws it, and a burst of colored text can make the buffer grow mid-draw. One recursive
spinlock now makes the append (writer) and the draw (reader) take turns, which stops the flood crash
and the garbled/blank chat lines.

- Teardown resets BOTH chat windows' rings (logwindo and logwin2) with read-back before narrowing the
  ring size; a ring that cannot be zeroed refuses the narrowing.
- The relocated-block cave is freed only when every block reads back as stock; a surviving block keeps
  the cave and blocks the base-fix restore, instead of leaving a jump into freed memory.

## 1.1

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

## 1.0

Initial release.
