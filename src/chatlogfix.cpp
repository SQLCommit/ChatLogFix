/**
 * chatlogfix - see chatlogfix.hpp for what the bug is and how the fix works.
 */
#include "chatlogfix.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

// ------------------------------------------------------------------------------------------------
// THE TWO PATCH SITES
//
// Both branches of the chat view-reset accumulate a running LINE count and stop once it reaches 50.
// Raise that bound and the rebuild fills the window instead of stopping halfway.
//
//   66 01 85 2A 40 06 00   add   word ptr [ebp+6402Ah], ax     <- accumulate the lines just inserted
//   8B 4D 1C               mov   ecx, [ebp+1Ch]
//   66 8B 95 2A 40 06 00   mov   dx,  word ptr [ebp+6402Ah]
//   03 C8                  add   ecx, eax
//   66 83 FA 32            cmp   dx,  50                        <- THE BYTE (the trailing 32)
//
// ------------------------------------------------------------------------------------------------
namespace
{
    struct Sig { const char* name; const uint8_t* pat; size_t len; };

    const uint8_t k_sigA[] = {
        0x66, 0x01, 0x85, 0x2A, 0x40, 0x06, 0x00, 0x8B, 0x4D, 0x1C,
        0x66, 0x8B, 0x95, 0x2A, 0x40, 0x06, 0x00, 0x03, 0xC8, 0x66, 0x83, 0xFA
    };
    const uint8_t k_sigB[] = {
        0x66, 0x01, 0x85, 0x2A, 0x40, 0x06, 0x00, 0x8B, 0x55, 0x1C,
        0x66, 0x8B, 0x8D, 0x2A, 0x40, 0x06, 0x00, 0x03, 0xD0, 0x66, 0x83, 0xF9
    };
    const Sig k_sigs[2] = {
        { "branch A", k_sigA, sizeof(k_sigA) },
        { "branch B", k_sigB, sizeof(k_sigB) },
    };

    const uint8_t k_probe[] = { 0x66, 0x01, 0x85, 0x2A, 0x40, 0x06, 0x00 };

    const uint8_t k_stock  = 50;
    const uint8_t k_target = 99;

    const uint8_t k_colGood = 0x02;
    const uint8_t k_colInfo = 0x6A;
    const uint8_t k_colWarn = 0x68;
    const uint8_t k_colFail = 0x44;
    const uint8_t k_colCmd  = 0x02;

    const char HL_ON  = '\x11';
    const char HL_OFF = '\x12';

    bool scan_span(const uint8_t* lo, uint32_t span, const uint8_t* pat, size_t len, ScanInfo* r)
    {
        __try
        {
            for (uint32_t o = 0; o + len <= span; ++o)
            {
                if (lo[o] != pat[0]) continue;
                if (memcmp(lo + o, pat, len) != 0) continue;
                if (r->hits < 4) r->hitAt[r->hits] = reinterpret_cast<uintptr_t>(lo + o);
                ++r->hits;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++r->faulted;
            return false;
        }
    }

    ModCode probe_module(ModuleInfo& m)
    {
        memset(&m, 0, sizeof(m));
        m.base = reinterpret_cast<uintptr_t>(GetModuleHandleA("FFXiMain.dll"));
        if (m.base == 0) return MOD_NO_MODULE;

        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return MOD_BAD_PE;
        if (dos->e_lfanew <= 0 || dos->e_lfanew >= 0x10000) return MOD_BAD_PE;
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return MOD_BAD_PE;

        m.sizeOfImage = nt->OptionalHeader.SizeOfImage;
        m.timeStamp   = nt->FileHeader.TimeDateStamp;
        m.sec         = IMAGE_FIRST_SECTION(nt);
        m.nsec        = nt->FileHeader.NumberOfSections;
        for (unsigned i = 0; i < m.nsec; ++i)
            if ((m.sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) ++m.nexec;
        return (m.nexec != 0) ? MOD_OK : MOD_NO_EXEC_SECTIONS;
    }

    void scan_all(const ModuleInfo& m, const uint8_t* pat, size_t len, ScanInfo& out)
    {
        memset(&out, 0, sizeof(out));
        for (unsigned i = 0; i < m.nsec; ++i)
        {
            if ((m.sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
            const uint32_t vsize = (m.sec[i].Misc.VirtualSize != 0)
                                 ? m.sec[i].Misc.VirtualSize : m.sec[i].SizeOfRawData;
            if (vsize <= static_cast<uint32_t>(len)) continue;
            ++out.sections;
            out.bytes += vsize;
            scan_span(reinterpret_cast<const uint8_t*>(m.base + m.sec[i].VirtualAddress),
                      vsize, pat, len, &out);
        }
        out.code = out.faulted   ? SCAN_FAULTED
                 : (out.hits == 0) ? SCAN_NOT_FOUND
                 : (out.hits == 1) ? SCAN_OK
                 :                   SCAN_AMBIGUOUS;
        out.imm8 = (out.hits == 1) ? (out.hitAt[0] + len) : 0;
    }

    void hex_at(uintptr_t base, uint32_t imgSize, uintptr_t p, size_t n, char* out, size_t outSz)
    {
        out[0] = '\0';
        if (outSz < 8 || base == 0 || imgSize == 0 || p < base) return;
        const uintptr_t end = base + imgSize;
        if (p >= end) return;
        if (p + n > end) n = static_cast<size_t>(end - p);
        size_t w = 0;
        for (size_t i = 0; i < n && w + 4 < outSz; ++i)
        {
            const int r = _snprintf_s(out + w, outSz - w, _TRUNCATE, "%02X ",
                                      *reinterpret_cast<const volatile uint8_t*>(p + i));
            if (r <= 0) break;
            w += static_cast<size_t>(r);
        }
    }

    void sec_name(const IMAGE_SECTION_HEADER& s, char out[9])
    {
        for (int i = 0; i < 8; ++i)
        {
            const char c = static_cast<char>(s.Name[i]);
            out[i] = (c >= 32 && c < 127) ? c : ' ';
        }
        out[8] = '\0';
    }

    bool write_byte(uintptr_t addr, uint8_t v, WriteInfo* w)
    {
        w->code = WRITE_OK; w->err = 0; w->got = 0; w->reprotOk = true;

        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), 1, PAGE_EXECUTE_READWRITE, &old))
        {
            w->code = WRITE_NO_ACCESS;
            w->err  = static_cast<uint32_t>(GetLastError());
            w->got  = *reinterpret_cast<volatile uint8_t*>(addr);
            return false;
        }
        *reinterpret_cast<volatile uint8_t*>(addr) = v;

        DWORD tmp = 0;
        w->reprotOk = (VirtualProtect(reinterpret_cast<LPVOID>(addr), 1, old, &tmp) != FALSE);
        if (!w->reprotOk) w->err = static_cast<uint32_t>(GetLastError());
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), 1);

        w->got = *reinterpret_cast<volatile uint8_t*>(addr);
        if (w->got != v) { w->code = WRITE_NOT_STUCK; return false; }
        return true;
    }
}

chatlogfix::chatlogfix(void)
    : m_Core(nullptr), m_Log(nullptr), m_Id(0), m_Patched(false), m_ToldHowToReport(false)
    , m_Base(0), m_ImgSize(0), m_ImgStamp(0), m_RestoreOk(true)
{
    m_Site[0]  = m_Site[1]  = 0;
    m_Orig[0]  = m_Orig[1]  = k_stock;
    m_Dirty[0] = m_Dirty[1] = false;
    memset(m_Scan, 0, sizeof(m_Scan));
    m_Fail[0] = '\0';
}

void chatlogfix::Print(uint8_t bodyColor, const char* fmt, ...)
{
    if (fmt == nullptr) return;
    if (bodyColor == 0) bodyColor = k_colInfo;

    char raw[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(raw, sizeof(raw), _TRUNCATE, fmt, ap);
    va_end(ap);

    char body[512];
    {
        size_t w = 0;
        for (size_t r = 0; raw[r] != '\0' && w + 3 < sizeof(body); ++r)
        {
            if (raw[r] == HL_ON)       { body[w++] = '\x1E'; body[w++] = static_cast<char>(k_colCmd); }
            else if (raw[r] == HL_OFF) { body[w++] = '\x1E'; body[w++] = static_cast<char>(bodyColor); }
            else                       { body[w++] = raw[r]; }
        }
        body[w] = '\0';
    }

    char plain[512];
    size_t w = 0;
    for (size_t r = 0; body[r] != '\0' && w + 1 < sizeof(plain); ++r)
    {
        if (body[r] == '\x1E') { if (body[r + 1] != '\0') ++r; continue; }
        plain[w++] = body[r];
    }
    plain[w] = '\0';
    Log(bodyColor == k_colFail || bodyColor == k_colWarn, "%s", plain);

    IChatManager* cm = (m_Core != nullptr) ? m_Core->GetChatManager() : nullptr;
    if (cm == nullptr)
    {
        if (m_Log == nullptr) OutputDebugStringA(plain);
        return;
    }

    char out[576];
    _snprintf_s(out, sizeof(out), _TRUNCATE,
        "\x1E\x51" "[" "\x1E\x06" "chatlogfix" "\x1E\x51" "]" "\x1E\x01" " "
        "\x1E%c" "%s" "\x1E\x01", bodyColor, body);
    // mode MUST be 1 here; other modes are silently dropped for plugin output.
    cm->AddChatMessage(1, false, out);
}

void chatlogfix::Usage(void)
{
    Print(k_colInfo, HL("/chatlogfix") " [" HL("on") "|" HL("off") "|" HL("status") "|" HL("diag")
                     "]   (or " HL("/clf") ")");
}

void chatlogfix::Fail(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(m_Fail, sizeof(m_Fail), _TRUNCATE, fmt, ap);
    va_end(ap);
    Print(k_colFail, "%s", m_Fail);

    if (!m_ToldHowToReport)
    {
        m_ToldHowToReport = true;
        Print(k_colInfo, "run " HL("/clf diag") " and report it.");
    }
}

void chatlogfix::Log(bool warn, const char* fmt, ...)
{
    if (m_Log == nullptr) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    m_Log->Log(static_cast<uint32_t>(warn ? Ashita::LogLevel::Warn : Ashita::LogLevel::Info),
               "chatlogfix", buf);
}

void chatlogfix::NoteWrite(int i, const WriteInfo& w)
{
    if (w.reprotOk) return;
    Log(true, "%s RVA 0x%06X left PAGE_EXECUTE_READWRITE -- restoring the original page protection "
              "failed (Windows error %u).", k_sigs[i].name, Rva(m_Site[i]), w.err);
}

void chatlogfix::LogSections(const ModuleInfo& m)
{
    for (unsigned i = 0; i < m.nsec; ++i)
    {
        if ((m.sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        char nm[9];
        sec_name(m.sec[i], nm);
        Log(false, "section   %-8s RVA 0x%06X  vsize 0x%06X  chars 0x%08X",
            nm, m.sec[i].VirtualAddress, m.sec[i].Misc.VirtualSize, m.sec[i].Characteristics);
    }
}

void chatlogfix::LogSiteBytes(bool warn)
{
    if (!Ready()) return;
    for (int i = 0; i < 2; ++i)
    {
        char hx[128];
        hex_at(m_Base, m_ImgSize, m_Site[i] - k_sigs[i].len, 24, hx, sizeof(hx));
        Log(warn, "site %c    RVA 0x%06X = %d  (orig %d, stock %d, target %d, dirty %s)  %s",
            'A' + i, Rva(m_Site[i]), *reinterpret_cast<volatile uint8_t*>(m_Site[i]),
            m_Orig[i], k_stock, k_target, m_Dirty[i] ? "yes" : "no", hx);
    }
}

void chatlogfix::ProbeReport(const ModuleInfo& m)
{
    ScanInfo p;
    scan_all(m, k_probe, sizeof(k_probe), p);
    const bool odd = (p.hits != 2);
    Log(odd, "probe     shared accumulator prefix: %u hit(s) across %u section(s), %u KB scanned%s",
        p.hits, p.sections, p.bytes / 1024,
        odd ? "  <- expected 2 (one per branch); a different count means the code moved" : "");
    for (uint32_t h = 0; h < p.hits && h < 4u; ++h)
    {
        char hx[128];
        hex_at(m.base, m.sizeOfImage, p.hitAt[h], 24, hx, sizeof(hx));
        Log(odd, "probe       RVA 0x%06X  %s", static_cast<uint32_t>(p.hitAt[h] - m.base), hx);
    }
}

void chatlogfix::ReportBlock(const ModuleInfo* m, const char* title)
{
    Log(false, "--- %s ---", title);
    Log(false, "plugin    chatlogfix v%.2f  (interface %.2f)", GetVersion(), GetInterfaceVersion());
    if (m != nullptr)
    {
        Log(false, "module    FFXiMain.dll base 0x%08X  SizeOfImage 0x%08X  TimeDateStamp 0x%08X  "
                   "sections %u (%u executable)",
            static_cast<unsigned>(m->base), m->sizeOfImage, m->timeStamp, m->nsec, m->nexec);
        LogSections(*m);
    }
    else
    {
        Log(true, "module    FFXiMain.dll could not be probed");
    }

    for (int i = 0; i < 2; ++i)
    {
        const ScanInfo& s = m_Scan[i];
        Log(false, "scan      %s: %s, %u hit(s), %u section(s), %u KB, %u faulted, imm8 RVA 0x%06X",
            k_sigs[i].name,
            (s.code == SCAN_OK) ? "OK" : (s.code == SCAN_NOT_FOUND) ? "NOT FOUND" : (s.code == SCAN_FAULTED) ? "FAULTED (read aborted)" : "AMBIGUOUS",
            s.hits, s.sections, s.bytes / 1024, s.faulted, Rva(s.imm8));
        for (uint32_t h = 0; h < s.hits && h < 4u; ++h)
            Log(false, "scan        hit %u RVA 0x%06X", h, Rva(s.hitAt[h]));
    }

    LogSiteBytes(false);
    Log(false, "state     patched=%s restoreOk=%s dirty=%s/%s",
        m_Patched ? "yes" : "no", m_RestoreOk ? "yes" : "no",
        m_Dirty[0] ? "yes" : "no", m_Dirty[1] ? "yes" : "no");
    Log(m_Fail[0] != '\0', "lastFail  %s", (m_Fail[0] != '\0') ? m_Fail : "(none)");
}

bool chatlogfix::Resolve(void)
{
    ModuleInfo m;
    const ModCode mc = probe_module(m);
    if (mc != MOD_OK)
    {
        m_Site[0] = m_Site[1] = 0;
        Fail(mc == MOD_NO_MODULE
             ? "FFXiMain.dll is not loaded in this process yet, so nothing was patched. If chatlogfix "
               "was loaded from a startup script, run " HL("/clf on") " once you are in game."
             : (mc == MOD_BAD_PE)
             ? "FFXiMain.dll does not parse as a PE image (DOS/NT header check failed). This is not a "
               "client-version problem -- the module handle is wrong. Not patching."
             : "scanned 0 executable sections of FFXiMain.dll, so 'not found' would mean nothing. "
               "This is a chatlogfix bug, not a problem with your client. Please report it.");
        return false;
    }
    m_Base     = m.base;
    m_ImgSize  = m.sizeOfImage;
    m_ImgStamp = m.timeStamp;
    Log(false, "resolve   FFXiMain.dll base 0x%08X, build 0x%08X, %u executable section(s)",
        static_cast<unsigned>(m.base), m.timeStamp, m.nexec);
    LogSections(m);

    for (int i = 0; i < 2; ++i) scan_all(m, k_sigs[i].pat, k_sigs[i].len, m_Scan[i]);

    if (m_Scan[0].code != SCAN_OK || m_Scan[1].code != SCAN_OK)
    {
        for (int i = 0; i < 2; ++i)
        {
            if (m_Scan[i].code == SCAN_OK) continue;
            if (m_Scan[i].code == SCAN_FAULTED)
            {
                Fail("reading FFXiMain's code faulted partway through the scan of %s, so the search "
                     "never completed. This is not a missing signature -- see the Ashita log for how "
                     "far it got before it stopped.", k_sigs[i].name);
            }
            else if (m_Scan[i].code == SCAN_NOT_FOUND)
                Fail("%s: signature NOT FOUND (scanned %u executable section(s), %u KB) on FFXiMain "
                     "build 0x%08X. This client build is not one chatlogfix knows -- the pattern "
                     "needs re-deriving for it, see BUILD.md.",
                     k_sigs[i].name, m_Scan[i].sections, m_Scan[i].bytes / 1024, m_ImgStamp);
            else
                Fail("%s: signature AMBIGUOUS -- matched %u times, not once. Refusing to guess which "
                     "one is the chat bound; the pattern is no longer unique on FFXiMain build "
                     "0x%08X and needs tightening.", k_sigs[i].name, m_Scan[i].hits, m_ImgStamp);
        }
        ProbeReport(m);
        m_Site[0] = m_Site[1] = 0;
        return false;
    }

    m_Site[0] = m_Scan[0].imm8;
    m_Site[1] = m_Scan[1].imm8;
    m_Fail[0] = '\0';
    return true;
}

bool chatlogfix::Apply(void)
{
    if (m_Patched)
    {
        const uint8_t a0 = *reinterpret_cast<volatile uint8_t*>(m_Site[0]);
        const uint8_t b0 = *reinterpret_cast<volatile uint8_t*>(m_Site[1]);
        if (a0 == k_target && b0 == k_target) return true;
        Fail("chatlogfix thinks it is on, but the sites read %d and %d instead of %d -- something "
             "overwrote our patch. Run " HL("/clf off") " then " HL("/clf on") " to re-take them.", a0, b0, k_target);
        return false;
    }
    if (!Ready() && !Resolve()) return false;

    uint8_t cur[2];
    for (int i = 0; i < 2; ++i) cur[i] = *reinterpret_cast<volatile uint8_t*>(m_Site[i]);
    if (cur[0] != k_stock || cur[1] != k_stock)
    {
        if (cur[0] == k_target && cur[1] == k_target)
            Fail("both sites already read %d, which is our own value -- an earlier chatlogfix session "
                 "did not unload cleanly, or a second copy is loaded. The chat log is already filled, "
                 "but this instance did not apply it and will not restore it. Restart the game for a "
                 "clean state.", k_target);
        else
            Fail("%s RVA 0x%06X reads %d and %s RVA 0x%06X reads %d, expected the stock %d for both "
                 "-- something else is already changing these bytes. Not patching, so it does not get "
                 "corrupted.",
                 k_sigs[0].name, Rva(m_Site[0]), cur[0],
                 k_sigs[1].name, Rva(m_Site[1]), cur[1], k_stock);
        LogSiteBytes(true);
        return false;
    }
    m_Orig[0] = cur[0];
    m_Orig[1] = cur[1];

    for (int i = 0; i < 2; ++i)
    {
        WriteInfo w;
        const bool ok = write_byte(m_Site[i], k_target, &w);
        NoteWrite(i, w);
        if (ok) { m_Dirty[i] = true; continue; }

        if (w.code == WRITE_NO_ACCESS)
            Fail("could not make %s RVA 0x%06X writable (Windows error %u) -- no change made.",
                 k_sigs[i].name, Rva(m_Site[i]), w.err);
        else
            Fail("wrote %d to %s RVA 0x%06X but it reads back %d -- the write did not stick, so "
                 "another tool is holding this byte. Not patching.",
                 k_target, k_sigs[i].name, Rva(m_Site[i]), w.got);

        for (int j = 0; j < i; ++j)
        {
            WriteInfo rw;
            const bool back = write_byte(m_Site[j], m_Orig[j], &rw);
            NoteWrite(j, rw);
            if (back) { m_Dirty[j] = false; continue; }
            Fail("ROLLBACK FAILED -- %s RVA 0x%06X is stuck at %d and chatlogfix cannot put it back "
                 "(Windows error %u). Restart the game to clear it.",
                 k_sigs[j].name, Rva(m_Site[j]), rw.got, rw.err);
        }
        return false;
    }

    m_Patched   = true;
    m_RestoreOk = true;
    m_Fail[0]   = '\0';
    return true;
}

bool chatlogfix::Restore(void)
{
    if (!m_Dirty[0] && !m_Dirty[1]) return false;

    bool allBack = true;
    for (int i = 0; i < 2; ++i)
    {
        if (!m_Dirty[i]) continue;
        const uint8_t cur = *reinterpret_cast<volatile uint8_t*>(m_Site[i]);
        if (cur != k_target)
            Log(true, "%s RVA 0x%06X reads %d before restore, not our %d -- something else wrote it "
                      "after we did. Restoring to %d anyway, so the byte is not left on a value nobody "
                      "owns.", k_sigs[i].name, Rva(m_Site[i]), cur, k_target, m_Orig[i]);

        WriteInfo w;
        const bool back = write_byte(m_Site[i], m_Orig[i], &w);
        NoteWrite(i, w);
        if (back) { m_Dirty[i] = false; continue; }

        allBack = false;
        Fail("RESTORE FAILED -- %s RVA 0x%06X still reads %d and chatlogfix cannot put it back "
             "(Windows error %u). The client is running patched code. Restart the game to clear it.",
             k_sigs[i].name, Rva(m_Site[i]), w.got, w.err);
    }

    m_Patched   = m_Dirty[0] || m_Dirty[1];
    if (m_Patched) m_ToldHowToReport = false;
    m_RestoreOk = allBack;
    if (allBack) m_Fail[0] = '\0';
    return allBack;
}

bool chatlogfix::Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id)
{
    m_Core = core;
    m_Log  = logger;
    m_Id   = id;

    if (Apply())
    {
        Usage();
        Log(false, "applied %d -> %d at branch A RVA 0x%06X, branch B RVA 0x%06X "
                   "(FFXiMain base 0x%08X, build 0x%08X, SizeOfImage 0x%08X)",
            k_stock, k_target, Rva(m_Site[0]), Rva(m_Site[1]),
            static_cast<unsigned>(m_Base), m_ImgStamp, m_ImgSize);
        LogSiteBytes(false);
    }
    else
    {
        Log(true, "INACTIVE -- %s", (m_Fail[0] != '\0')
            ? m_Fail
            : "Apply() failed without recording a reason (this is a chatlogfix bug)");
    }
    return true;
}

void chatlogfix::Release(void)
{
    if (!m_Dirty[0] && !m_Dirty[1])
    {
        Log(false, "released; nothing of ours was in the image.");
        return;
    }
    if (Restore())
    {
        Log(false, "released; both sites restored to %d.", k_stock);
        return;
    }
    Log(true, "RELEASED WITH THE PATCH STILL APPLIED -- FFXiMain.dll still holds %d and no chatlogfix "
              "instance owns it any more. Reload chatlogfix and run " HL("/clf off") ", or restart the game.",
        k_target);
}

void chatlogfix::Status(bool full)
{
    ModuleInfo m;
    const ModCode mc = probe_module(m);
    if (mc == MOD_OK && m_Base != 0 && m.base != m_Base)
    {
        Log(true, "FFXiMain moved from base 0x%08X to 0x%08X -- the resolved sites belong to an image "
                  "that is no longer there. Dropping them and re-resolving.",
            static_cast<unsigned>(m_Base), static_cast<unsigned>(m.base));
        m_Site[0]  = m_Site[1]  = 0;
        m_Dirty[0] = m_Dirty[1] = false;
        m_Patched  = false;
    }
    if (!Ready()) Resolve();

    if (!Ready())
    {
        Print(k_colFail, "INACTIVE -- %s", (m_Fail[0] != '\0')
              ? m_Fail
              : "chatlogfix has not tried to patch yet. Run " HL("/clf on") ".");
        Print(k_colInfo, "run " HL("/clf diag") " and report it.");
    }
    else
    {
        const uint8_t a0 = *reinterpret_cast<volatile uint8_t*>(m_Site[0]);
        const uint8_t b0 = *reinterpret_cast<volatile uint8_t*>(m_Site[1]);
        const bool ours = m_Dirty[0] || m_Dirty[1];
        if (a0 == k_target && b0 == k_target)
        {
            if (ours)
                Print(k_colGood, "on -- the expanded chat log fills to %d lines instead of %d.",
                      k_target, k_stock);
            else
                Print(k_colWarn, "filling to %d lines, but NOT because of this session -- an earlier "
                                 "chatlogfix (or another tool) left these bytes patched and they "
                                 "survive an unload. " HL("/clf off") " will not revert them; " HL("/clf on") " then " HL("/clf off") " will.", k_target);
        }
        else if (a0 == k_stock && b0 == k_stock)
            Print(k_colInfo, "off -- stock %d lines.", k_stock);
        else
        {
            Print(k_colWarn, "UNEXPECTED -- %s reads %d and %s reads %d (we expect %d when on, %d "
                             "when off). Something else is writing these bytes; " HL("/clf off") " then " HL("/clf on") " "
                             "to re-take them.",
                  k_sigs[0].name, a0, k_sigs[1].name, b0, k_target, k_stock);
        }
        if (full)
            Print(k_colInfo, "FFXiMain build 0x%08X, branch A RVA 0x%06X, branch B RVA 0x%06X.",
                  m_ImgStamp, Rva(m_Site[0]), Rva(m_Site[1]));
    }

    if (full)
    {
        ReportBlock((mc == MOD_OK) ? &m : nullptr, "/chatlogfix diag");
        if (mc == MOD_OK) ProbeReport(m);
        Print(k_colInfo, "diagnostics written to the Ashita log (" HL("Ashita\\logs") ") -- attach it "
                         "to a bug report.");
    }
}

bool chatlogfix::HandleCommand(int32_t mode, const char* command, bool injected)
{
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(injected);

    std::vector<std::string> args;
    Ashita::Commands::GetCommandArgs(command, &args);
    if (args.empty()) return false;
    if (_stricmp(args[0].c_str(), "/chatlogfix") != 0 && _stricmp(args[0].c_str(), "/clf") != 0)
        return false;

    const char* a = (args.size() > 1) ? args[1].c_str() : "status";

    if (_stricmp(a, "on") == 0)
    {
        if (Apply()) Print(k_colGood, "on -- filling to %d lines.", k_target);
    }
    else if (_stricmp(a, "off") == 0)
    {
        if (!m_Dirty[0] && !m_Dirty[1])
            Print(k_colInfo, "already off -- nothing of ours is in the image, so there was nothing to "
                             "restore.");
        else if (Restore())
            Print(k_colGood, "off -- restored to the stock %d lines.", k_stock);
    }
    else if (_stricmp(a, "status") == 0)
    {
        Status(false);
    }
    else if (_stricmp(a, "diag") == 0)
    {
        Status(true);
    }
    else
    {
        Usage();
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Ashita plugin entry points (see src/exports.def).
// ------------------------------------------------------------------------------------------------
extern "C"
{
    __declspec(noinline) IPlugin* __stdcall expCreatePlugin(const char* args)
    {
        UNREFERENCED_PARAMETER(args);
        return new chatlogfix();
    }
    __declspec(noinline) void __stdcall expDestroyPlugin(void* instance)
    {
        if (instance != nullptr) delete static_cast<chatlogfix*>(instance);
    }
    __declspec(noinline) double __stdcall expGetInterfaceVersion(void)
    {
        return ASHITA_INTERFACE_VERSION;
    }
}
