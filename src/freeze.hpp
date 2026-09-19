/**
 * freeze - every byte written into the client goes in or out only
 * while every other thread of the process is suspended and none
 * of them is inside the code being changed, and the DLL pins itself before its first write so no
 * thread can ever return into an unmapped image.
 */
#ifndef CHATLOGFIX_FREEZE_HPP_INCLUDED
#define CHATLOGFIX_FREEZE_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
#include <windows.h>

// A span of code no other thread may be running while bytes in it change: [lo, hi).
struct CodeRange { uintptr_t lo = 0, hi = 0; };

// Every other thread of this process, suspended until destruction. Threads are walked with
// NtGetNextThread, which hands out handles without allocating (a suspended thread may hold the heap
// lock), and the walk repeats until a pass finds no thread it does not already hold. Anything that
// cannot be established -- a thread that cannot be opened or suspended, too many threads, a walk that
// does not settle -- makes the freeze incomplete, and anyInRanges() then reports every range occupied.
class ThreadFreeze
{
public:
    ThreadFreeze()
    {
        using NextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
        static const auto nextThread = reinterpret_cast<NextThreadFn>(
            GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtGetNextThread"));
        if (!nextThread) { complete_ = false; return; }
        const DWORD self = GetCurrentThreadId();
        const ACCESS_MASK access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION;
        bool settled = false;
        for (int pass = 0; pass < kMaxPasses && complete_; pass++)
        {
            bool added = false;
            HANDLE current = nullptr;
            for (;;)
            {
                HANDLE next = nullptr;
                const LONG status = nextThread(GetCurrentProcess(), current, access, 0, 0, &next);
                if (current) CloseHandle(current);
                current = nullptr;
                if (status == LONG(0x8000001A)) break;                    // STATUS_NO_MORE_ENTRIES
                if (status < 0 || !next) { complete_ = false; break; }    // cannot be opened with this access
                const DWORD id = GetThreadId(next);
                if (id == self || holds(id)) { current = next; continue; }
                if (count_ == kMaxThreads) { current = next; complete_ = false; break; }
                // An exited thread whose object another handle keeps alive is still walked, and SuspendThread refuses it. It runs nothing: skip it.
                { DWORD code = 0; if (GetExitCodeThread(next, &code) && code != STILL_ACTIVE) { current = next; continue; } }
                if (SuspendThread(next) == DWORD(-1)) { DWORD code = 0; if (GetExitCodeThread(next, &code) && code != STILL_ACTIVE) { current = next; continue; } current = next; complete_ = false; break; }
                Thread& t = threads_[count_++];
                t.id = id;
                t.handle = next;                                           // closed by the destructor after resuming
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;                    // also waits for the suspension to take effect
                t.ipKnown = GetThreadContext(next, &context) != FALSE;
                t.ip = t.ipKnown ? uintptr_t(context.Eip) : 0;
                added = true;
                // NtGetNextThread continues from a handle; take a second one so this thread's handle stays with it.
                if (!DuplicateHandle(GetCurrentProcess(), next, GetCurrentProcess(), &current, 0, FALSE, DUPLICATE_SAME_ACCESS))
                { complete_ = false; break; }
            }
            if (current) CloseHandle(current);
            if (complete_ && !added) { settled = true; break; }           // a whole pass found no new thread
        }
        if (!settled) complete_ = false;
    }
    ~ThreadFreeze()
    {
        for (size_t i = 0; i < count_; i++) { ResumeThread(threads_[i].handle); CloseHandle(threads_[i].handle); }
    }
    ThreadFreeze(const ThreadFreeze&) = delete;
    ThreadFreeze& operator=(const ThreadFreeze&) = delete;

    // True if any suspended thread has its instruction pointer in one of `ranges`, or might (unread,
    // or the freeze is incomplete).
    bool anyInRanges(const CodeRange* ranges, size_t n) const
    {
        if (!complete_) return true;
        for (size_t i = 0; i < count_; i++)
        {
            if (!threads_[i].ipKnown) return true;
            for (size_t r = 0; r < n; r++)
                if (threads_[i].ip >= ranges[r].lo && threads_[i].ip < ranges[r].hi) return true;
        }
        return false;
    }
    bool complete() const { return complete_; }
    size_t suspended() const { return count_; }

private:
    static constexpr size_t kMaxThreads = 512;
    static constexpr int kMaxPasses = 8;
    struct Thread { DWORD id = 0; HANDLE handle = nullptr; uintptr_t ip = 0; bool ipKnown = false; };
    bool holds(DWORD id) const
    {
        for (size_t i = 0; i < count_; i++) if (threads_[i].id == id) return true;
        return false;
    }
    Thread threads_[kMaxThreads];
    size_t count_ = 0;
    bool complete_ = true;
};

// Runs `act` with every other thread suspended, at a moment when none of them is in `ranges`: tries
// up to `attempts` times, 1 ms apart. `act` must not allocate, log, or take a lock another (suspended)
// thread may hold. Returns whether `act` ran.
template <class Act>
inline bool WhenNoThreadIn(const CodeRange* ranges, size_t n, int attempts, Act&& act)
{
    for (int i = 0; i < attempts; i++)
    {
        {
            ThreadFreeze freeze;
            if (!freeze.anyInRanges(ranges, n)) { act(); return true; }
        }
        Sleep(1);
    }
    return false;
}

// The image span of the module containing `addr` (from its PE header), or false.
inline bool ModuleRangeOf(const void* addr, uintptr_t& lo, uintptr_t& hi)
{
    HMODULE h = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(addr), &h) || !h) return false;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(h);
    const uint32_t e = *reinterpret_cast<const uint32_t*>(b + 0x3C);
    const uint32_t size = *reinterpret_cast<const uint32_t*>(b + e + 0x50);   // OptionalHeader.SizeOfImage
    if (size == 0) return false;
    lo = reinterpret_cast<uintptr_t>(h);
    hi = lo + size;
    return true;
}

// Pins this DLL for the life of the process. Called before the first byte goes into the client: from
// then on a thread caught between a patched site and this code can never reach unmapped memory. Checked.
inline bool PinThisModule()
{
    HMODULE self = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCSTR>(&PinThisModule), &self) != FALSE && self != nullptr;
}

#endif // CHATLOGFIX_FREEZE_HPP_INCLUDED
