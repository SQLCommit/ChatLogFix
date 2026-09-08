/**
 * chatserial - serialize FFXiMain's chat append vs. draw with a recursive spinlock.
 */
#ifndef CHATSERIAL_HPP_INCLUDED
#define CHATSERIAL_HPP_INCLUDED

#include <cstdint>

// Logging callback: warn=true for problems, false for info.
typedef void (*ChatSerialLog)(void* ctx, bool warn, const char* msg);

bool ChatSerial_Install(ChatSerialLog log, void* ctx);

void ChatSerial_Remove(void);

// Report detailed state (RVAs, patched sites) via the callback - for /clf diag.
void ChatSerial_Diag(ChatSerialLog log, void* ctx);

// True while the lock is active.
bool ChatSerial_Active(void);

#endif // CHATSERIAL_HPP_INCLUDED
