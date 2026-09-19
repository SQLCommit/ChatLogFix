/**
 * chatserial - serialize FFXiMain's chat append vs. draw with a recursive spinlock.
 */
#ifndef CHATSERIAL_HPP_INCLUDED
#define CHATSERIAL_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
struct CodeRange;

// Logging callback: warn=true for problems, false for info.
typedef void (*ChatSerialLog)(void* ctx, bool warn, const char* msg);

bool ChatSerial_Install(ChatSerialLog log, void* ctx);

// Entry sites still holding chatserial's jmp (0 once removal is clean, or nothing was installed).
int ChatSerial_SitesLeft(void);

// The reader entry once resolved (for the caller's own freeze ranges).
bool ChatSerial_ReaderEntry(uintptr_t& rent);

// The spans a thread must be out of while chatserial's sites change: the cave and the writer/reader
// bodies. Returns how many were written to `out`.
size_t ChatSerial_Ranges(CodeRange* out, size_t max);

// Report detailed state (RVAs, patched sites) via the callback - for /clf diag.
void ChatSerial_Diag(ChatSerialLog log, void* ctx);

// True while the lock is active.
bool ChatSerial_Active(void);

// True once chatserial pinned the DLL (it does so before its first write, even if that write failed).
bool ChatSerial_Pinned(void);

#endif // CHATSERIAL_HPP_INCLUDED
