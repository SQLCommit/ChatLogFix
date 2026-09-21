// Serialize chat append and draw with a recursive spinlock.
// Writer: entry 81 EC 6C 06 00 00 53 55 56 8B; exit add esp,0x66C; ret 0x14.
// Reader: entry 83 EC 20 53 56 8B F1 33 DB 88; two exits add esp,0x20; ret 8.
#include "chatserial.hpp"
#include "chatserial_cave.hpp"
#include "freeze.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    const uint8_t k_sigWriter[]  = { 0x81,0xec,0x6c,0x06,0x00,0x00,0x53,0x55,0x56,0x8b }; // writer entry
    const uint8_t k_sigReader[]  = { 0x83,0xec,0x20,0x53,0x56,0x8b,0xf1,0x33,0xdb,0x88 }; // reader entry
    const uint8_t k_wexitPat[]   = { 0x81,0xc4,0x6c,0x06,0x00,0x00,0xc2,0x14,0x00 };       // add esp,0x66C ; ret 0x14
    const uint8_t k_rexitPat[]   = { 0x83,0xc4,0x20,0xc2,0x08,0x00 };                      // add esp,0x20  ; ret 8

    // golden cave code for BuildChatLockCave(0x30000000, 0x10000000, 0x20000000).
    const uint8_t k_goldenCave[] = {
        0xf0,0xff,0x05,0x08,0x00,0x00,0x30,0x64,0x8b,0x15,0x24,0x00,
        0x00,0x00,0x3b,0x15,0x00,0x00,0x00,0x30,0x74,0x0c,0x33,0xc0,
        0xf0,0x0f,0xb1,0x15,0x00,0x00,0x00,0x30,0x75,0xec,0xff,0x05,
        0x04,0x00,0x00,0x30,0xf0,0xff,0x0d,0x08,0x00,0x00,0x30,0x81,
        0xec,0x6c,0x06,0x00,0x00,0xe9,0xbc,0xff,0xff,0xdf,0xf0,0xff,
        0x05,0x08,0x00,0x00,0x30,0x64,0x8b,0x15,0x24,0x00,0x00,0x00,
        0x3b,0x15,0x00,0x00,0x00,0x30,0x74,0x0c,0x33,0xc0,0xf0,0x0f,
        0xb1,0x15,0x00,0x00,0x00,0x30,0x75,0xec,0xff,0x05,0x04,0x00,
        0x00,0x30,0xf0,0xff,0x0d,0x08,0x00,0x00,0x30,0x83,0xec,0x20,
        0x53,0x56,0xe9,0x82,0xff,0xff,0xef,0x52,0x64,0x8b,0x15,0x24,
        0x00,0x00,0x00,0x3b,0x15,0x00,0x00,0x00,0x30,0x5a,0x75,0x12,
        0xff,0x0d,0x04,0x00,0x00,0x30,0x75,0x0a,0xc7,0x05,0x00,0x00,
        0x00,0x30,0x00,0x00,0x00,0x00,0x81,0xc4,0x6c,0x06,0x00,0x00,
        0xc2,0x14,0x00,0x52,0x64,0x8b,0x15,0x24,0x00,0x00,0x00,0x3b,
        0x15,0x00,0x00,0x00,0x30,0x5a,0x75,0x12,0xff,0x0d,0x04,0x00,
        0x00,0x30,0x75,0x0a,0xc7,0x05,0x00,0x00,0x00,0x30,0x00,0x00,
        0x00,0x00,0x83,0xc4,0x20,0xc2,0x08,0x00,
    };

    uint8_t* g_cave = nullptr;
    // Restore only owned entry jumps; exit patches remain installed.
    struct Site { uintptr_t addr; uint8_t saved[5]; uint8_t installed[5]; bool entry; bool done; };
    Site     g_sites[5];
    int      g_nSites = 0;
    bool     g_active = false;
    bool     g_adopted = false;         // this load reused the cave a previous load left behind
    bool     g_pinned = false;          // the DLL pinned itself (before its first write); never cleared
    uintptr_t g_caveLive = 0;           // a cave the exits point into (this load's, or adopted): reachable until the game closes
    ChatSerialLog g_log = nullptr; void* g_ctx = nullptr;
    const size_t k_releaseHead = 35;    // bytes before the replicated exit in a release stub (see the builder)

    // SEH-guarded compare of `n` bytes at `a` against `ref`.
    bool mem_equals(uintptr_t a, const uint8_t* ref, size_t n)
    {
        __try { return memcmp(reinterpret_cast<const void*>(a), ref, n) == 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    uintptr_t jmp_target(uintptr_t a)
    {
        __try
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
            if (p[0] != 0xE9) return 0;
            int32_t rel; memcpy(&rel, p + 1, 4);
            return a + 5 + rel;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    uintptr_t g_base = 0;
    uint32_t  g_writerRva = 0, g_readerRva = 0;
    uintptr_t g_went = 0, g_rent = 0;     // the writer/reader entries once resolved (kept after removal)
    // Freeze the full writer, reader and cave spans: a thread may hold the lock or a stolen instruction.
    const uint32_t k_writerSpan = 0x600, k_readerSpan = 0x400;

    void logf(ChatSerialLog log, void* ctx, bool warn, const char* fmt, ...)
    {
        if (!log) return;
        char buf[256];
        va_list ap; va_start(ap, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        log(ctx, warn, buf);
    }

    bool module_range(uintptr_t& base, uint32_t& size)
    {
        HMODULE h = GetModuleHandleA("FFXiMain.dll");
        if (!h) return false;
        base = reinterpret_cast<uintptr_t>(h);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(h);
        const uint32_t e = *reinterpret_cast<const uint32_t*>(b + 0x3C);
        size = *reinterpret_cast<const uint32_t*>(b + e + 0x50);
        return size != 0 && size < 0x4000000;
    }

    // SEH-guarded: a non-readable region in the image faults to 0 (not found).
    uintptr_t find_unique(uintptr_t base, uint32_t size, const uint8_t* pat, size_t len)
    {
        __try
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(base);
            uintptr_t found = 0;
            for (uint32_t i = 0; i + len <= size; ++i)
                if (memcmp(p + i, pat, len) == 0)
                {
                    if (found) return 0;   // not unique
                    found = base + i;
                }
            return found;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // count occurrences of pat in [start, start+range) clamped to the image [base, base+size);
    // records up to `cap` addresses. Returns the TOTAL count (caller checks it equals what it expects).
    int scan_in(uintptr_t start, uint32_t range, const uint8_t* pat, size_t len,
                uintptr_t base, uint32_t size, uintptr_t* out, int cap)
    {
        __try
        {
            uintptr_t hi = start + range;
            const uintptr_t imgEnd = base + size;
            if (hi > imgEnd) hi = imgEnd;
            int n = 0;
            for (uintptr_t a = start; a + len <= hi; ++a)
                if (memcmp(reinterpret_cast<const void*>(a), pat, len) == 0)
                {
                    if (n < cap) out[n] = a;
                    ++n;
                }
            return n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }   // fault -> caller's count check fails -> aborts
    }

    // Match stock epilogues or retained jumps whose release stubs end in the same epilogue.
    bool exit_site_ok(uintptr_t a, const uint8_t* pat, size_t len)
    {
        __try
        {
            if (memcmp(reinterpret_cast<const void*>(a), pat, len) == 0) return true;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
            if (p[0] != 0xE9) return false;
            int32_t rel; memcpy(&rel, p + 1, 4);
            const uint8_t* t = reinterpret_cast<const uint8_t*>(a + 5 + rel);
            return t[0] == 0x52 && memcmp(t + k_releaseHead, pat, len) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    int scan_exits(uintptr_t start, uint32_t range, const uint8_t* pat, size_t len,
                   uintptr_t base, uint32_t size, uintptr_t* out, int cap)
    {
        __try
        {
            uintptr_t hi = start + range;
            const uintptr_t imgEnd = base + size;
            if (hi > imgEnd) hi = imgEnd;
            int n = 0;
            for (uintptr_t a = start; a + len <= hi; ++a)
                if (exit_site_ok(a, pat, len))
                {
                    if (n < cap) out[n] = a;
                    ++n;
                    if (memcmp(reinterpret_cast<const void*>(a), pat, len) != 0) a += 4;   // skip the jmp body
                }
            return n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    bool write_bytes(uintptr_t addr, const uint8_t* src, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), n, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(reinterpret_cast<void*>(addr), src, n);
        DWORD tmp; VirtualProtect(reinterpret_cast<LPVOID>(addr), n, old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), n);
        return true;
    }
}

bool ChatSerial_Install(ChatSerialLog log, void* ctx)
{
    if (g_active) return true;

    uintptr_t base; uint32_t size;
    if (!module_range(base, size)) { logf(log,ctx,true,"chatserial: FFXiMain not found"); return false; }

    const uintptr_t went = find_unique(base, size, k_sigWriter, sizeof(k_sigWriter));
    const uintptr_t rent = find_unique(base, size, k_sigReader, sizeof(k_sigReader));
    if (!went) { logf(log,ctx,true,"chatserial: writer sig not found/unique (client build unknown) - NOT installed"); return false; }
    if (!rent) { logf(log,ctx,true,"chatserial: reader sig not found/unique (client build unknown) - NOT installed"); return false; }
    g_went = went; g_rent = rent;   // known from here on, for the freeze ranges even if this install stops below

    // load-time self-check.
    {
        std::vector<uint8_t> ref; size_t roff[4];
        BuildChatLockCave(0x30000000, 0x10000000, 0x20000000, ref, roff);
        if (ref.size() != sizeof(k_goldenCave) || memcmp(ref.data(), k_goldenCave, sizeof(k_goldenCave)) != 0)
        { logf(log,ctx,true,"chatserial: cave builder self-check FAILED (drift from proven bytes) - NOT installed"); return false; }
    }

    // locate exit sites by scanning each function body (robust to internal shifts; bounds-clamped).
    uintptr_t wexit = 0, rexits[2] = { 0, 0 };
    if (scan_exits(went, 0x600, k_wexitPat, sizeof(k_wexitPat), base, size, &wexit, 1) != 1)
    { logf(log,ctx,true,"chatserial: writer exit pattern not found/unique - NOT installed"); return false; }
    if (scan_exits(rent, 0x400, k_rexitPat, sizeof(k_rexitPat), base, size, rexits, 2) != 2)
    { logf(log,ctx,true,"chatserial: reader exits != 2 (client build unknown) - NOT installed"); return false; }
    const bool repointed = memcmp(reinterpret_cast<const void*>(wexit), k_wexitPat, sizeof(k_wexitPat)) != 0
                        || memcmp(reinterpret_cast<const void*>(rexits[0]), k_rexitPat, sizeof(k_rexitPat)) != 0
                        || memcmp(reinterpret_cast<const void*>(rexits[1]), k_rexitPat, sizeof(k_rexitPat)) != 0;

    // Reuse retained exits only when the cave matches this builder byte-for-byte.
    // Keep the same lock so existing holders and waiters remain synchronized.
    uint8_t* cave = nullptr; uintptr_t C = 0; std::vector<uint8_t> code; size_t off[4];
    if (repointed)
    {
        std::vector<uint8_t> probe; size_t poff[4];
        BuildChatLockCave(0, went, rent, probe, poff);              // offsets do not depend on C
        // A failed install may leave only one exit jump. All others must be stock or target the same cave.
        uintptr_t C0 = 0;
        if (jmp_target(wexit))          C0 = jmp_target(wexit)     - 16 - poff[2];
        else if (jmp_target(rexits[0])) C0 = jmp_target(rexits[0]) - 16 - poff[3];
        else if (jmp_target(rexits[1])) C0 = jmp_target(rexits[1]) - 16 - poff[3];
        std::vector<uint8_t> want; size_t woff[4];
        BuildChatLockCave(C0, went, rent, want, woff);
        auto exit_ok = [&](uintptr_t a, const uint8_t* pat, size_t len, uintptr_t stub) -> bool
        { return mem_equals(a, pat, len) || jmp_target(a) == stub; };
        const bool ok = C0 > 0x10000 && mem_equals(C0 + 16, want.data(), want.size())
                     && exit_ok(wexit,     k_wexitPat, sizeof(k_wexitPat), C0 + 16 + woff[2])
                     && exit_ok(rexits[0], k_rexitPat, sizeof(k_rexitPat), C0 + 16 + woff[3])
                     && exit_ok(rexits[1], k_rexitPat, sizeof(k_rexitPat), C0 + 16 + woff[3]);
        if (!ok)
        { logf(log,ctx,true,"chatserial: the exits are redirected into a cave that is not this build's - NOT installed (chat still works, the serialization is off; restart the game to clear it)"); return false; }
        cave = reinterpret_cast<uint8_t*>(C0); C = C0; code = want; memcpy(off, woff, sizeof(off));
        g_adopted = true;
    }
    else
    {
        cave = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!cave) { logf(log,ctx,true,"chatserial: VirtualAlloc failed"); return false; }
        C = reinterpret_cast<uintptr_t>(cave);
        *reinterpret_cast<uint32_t*>(cave+0)  = 0;   // owner
        *reinterpret_cast<uint32_t*>(cave+4)  = 0;   // count
        *reinterpret_cast<uint32_t*>(cave+8)  = 0;   // waiters (threads inside the acquire path)
        *reinterpret_cast<uint32_t*>(cave+12) = 0;
        BuildChatLockCave(C, went, rent, code, off);
        memcpy(cave + 16, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
        g_adopted = false;
    }
    const uintptr_t ACQW = C + 16 + off[0], ACQR = C + 16 + off[1], RELW = C + 16 + off[2], REL8 = C + 16 + off[3];

    // Pin the DLL before the first write; it remains mapped until process exit.
    g_pinned = PinThisModule();
    if (!g_pinned)
    {
        if (!g_adopted) VirtualFree(cave, 0, MEM_RELEASE);   // never published: nothing can be in it
        logf(log,ctx,true,"chatserial: could not pin the DLL, so nothing was patched (chat still works, the serialization is off)");
        return false;
    }
    g_cave = cave; g_nSites = 0; g_log = log; g_ctx = ctx;
    auto patch = [&](uintptr_t addr, uintptr_t target) -> bool
    {
        Site& s = g_sites[g_nSites];
        s.addr = addr;
        // Save stock epilogues for exit sites, including jumps retained from an earlier load.
        if      (addr == wexit)                            memcpy(s.saved, k_wexitPat, 5);
        else if (addr == rexits[0] || addr == rexits[1])   memcpy(s.saved, k_rexitPat, 5);
        else                                               memcpy(s.saved, reinterpret_cast<void*>(addr), 5);
        uint8_t p[5]; p[0] = 0xe9;
        const uint32_t rel = (uint32_t)(target - (addr + 5));
        memcpy(p+1, &rel, 4);
        memcpy(s.installed, p, 5);
        s.entry = (addr == went || addr == rent);
        s.done  = false;
        if (!mem_equals(addr, p, 5) && !write_bytes(addr, p, 5)) return false;   // adopted exits are already right
        ++g_nSites;
        return true;
    };
    // Freeze outside the writer, reader and cave before replacing instructions.
    // No logging or allocation while other threads are suspended.
    CodeRange ranges[4]; size_t nr = 0;
    { uintptr_t lo, hi; if (ModuleRangeOf(reinterpret_cast<const void*>(&ChatSerial_Install), lo, hi)) ranges[nr++] = CodeRange{lo, hi}; }
    ranges[nr++] = CodeRange{C, C + 0x1000};
    ranges[nr++] = CodeRange{went, went + k_writerSpan};
    ranges[nr++] = CodeRange{rent, rent + k_readerSpan};
    bool ok = false, ran = false;
    int stranded = 0;
    ran = WhenNoThreadIn(ranges, nr, 1000, [&]
    {
        // Install release stubs before acquire stubs.
        ok = patch(wexit, RELW) && patch(rexits[0], REL8) && patch(rexits[1], REL8)
          && patch(went, ACQW) && patch(rent, ACQR);
        // Same rule as removal: only ENTRY sites go back on failure, each verified; one that does not
        // go back keeps its record so a later removal can retry it.
        if (!ok)
            for (int i = 0; i < g_nSites; ++i)
            {
                if (!g_sites[i].entry) continue;
                if (mem_equals(g_sites[i].addr, g_sites[i].saved, 5) || (write_bytes(g_sites[i].addr, g_sites[i].saved, 5) && mem_equals(g_sites[i].addr, g_sites[i].saved, 5)))
                    g_sites[i].done = true;
                else
                    ++stranded;
            }
    });
    if (ran || g_adopted) g_caveLive = C;   // the exits jump into it from now on (an adopted one already did)
    if (!ran)
    {
        if (!g_adopted) VirtualFree(cave, 0, MEM_RELEASE);   // never published
        g_nSites = 0; g_cave = nullptr;
        logf(log,ctx,true,"chatserial: no moment without a thread in the chat append or draw came within a second - NOT installed (chat still works, the serialization is off)");
        return false;
    }
    if (!ok)
    {
        if (stranded == 0) { g_nSites = 0; g_cave = nullptr; }   // exits retained (safe), entries verified back
        logf(log,ctx,true, stranded == 0
             ? "chatserial: a patch write failed - entries reverted, NOT installed (redirected exits are retained, which is safe)"
             : "chatserial: a patch write failed, and an entry did NOT revert - it stays recorded for unload to retry; NOT active");
        return false;
    }
    g_base = base; g_writerRva = (uint32_t)(went - base); g_readerRva = (uint32_t)(rent - base);
    g_active = true;
    if (g_adopted) logf(log,ctx,false,"chatserial: adopted the lock cave a previous load left behind (same lock state)");
    logf(log,ctx,false,"chatserial: chat append/draw serialized (writer RVA 0x%06X, reader RVA 0x%06X)",
         (uint32_t)(went-base), (uint32_t)(rent-base));
    return true;
}

size_t ChatSerial_Ranges(CodeRange* out, size_t max)
{
    size_t n = 0;
    // The live cave stays reachable through the retained exits even after a failed install or a removal.
    if (g_caveLive && n < max) out[n++] = CodeRange{ g_caveLive, g_caveLive + 0x1000 };
    if (g_went && n < max) out[n++] = CodeRange{ g_went, g_went + k_writerSpan };
    if (g_rent && n < max) out[n++] = CodeRange{ g_rent, g_rent + k_readerSpan };
    return n;
}

int ChatSerial_SitesLeft(void)
{
    int n = 0;
    for (int i = 0; i < g_nSites; ++i) if (g_sites[i].entry && !g_sites[i].done) ++n;
    return n;
}

bool ChatSerial_ReaderEntry(uintptr_t& rent)
{
    if (g_rent == 0) return false;
    rent = g_rent;
    return true;
}

void ChatSerial_Diag(ChatSerialLog log, void* ctx)
{
    if (!log) return;
    if (!g_active && g_nSites == 0) { logf(log,ctx,false,"chatserial: INACTIVE (serialization not installed)"); return; }
    if (!g_active)
    {
        logf(log,ctx,true,"chatserial: INACTIVE, but %d entry site(s) still hold a jmp (a failed install); they stay until the game closes", ChatSerial_SitesLeft());
        for (int i = 0; i < g_nSites; ++i)
            if (!g_sites[i].done) logf(log,ctx,false,"  stranded site %d: address 0x%08X (%s)", i, (uint32_t)g_sites[i].addr, g_sites[i].entry ? "entry" : "exit");
        return;
    }
    logf(log,ctx,false,"chatserial: ACTIVE - writer RVA 0x%06X, reader RVA 0x%06X, %d sites patched%s",
         g_writerRva, g_readerRva, g_nSites, g_adopted ? " (cave adopted from a previous load)" : "");
    for (int i = 0; i < g_nSites; ++i)
        logf(log,ctx,false,"  chatserial site %d: RVA 0x%06X (%s%s)", i, (uint32_t)(g_sites[i].addr - g_base), g_sites[i].entry ? "entry" : "exit", g_sites[i].done ? ", restored" : "");
}

bool ChatSerial_Active(void) { return g_active; }
bool ChatSerial_Pinned(void) { return g_pinned; }
