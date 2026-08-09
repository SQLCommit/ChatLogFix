/**
 * chatlogfix - see chatlogfix.hpp for what the bug is and how the fix works.
 */
#include "chatlogfix.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
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

    struct RingAnchor { const char* name; const uint8_t* pat; const char* mask; size_t len; };

    const uint8_t a_clear[]   = { 0x8B,0xD1,0x57,0xB9,0x00,0xC8,0x00,0x00,0x33,0xC0 };
    const uint8_t a_wrap[]    = { 0x8B,0xD1,0x56,0x57,0x0F,0xBF,0x72,0x14,0x46,0x83,0xFE,0x64 };
    const uint8_t a_maxidx[]  = { 0x8B,0xD1,0x56,0x57,0x0F,0xBF,0x72,0x16,0x4E,0x79,0x05 };
    const uint8_t a_capinit[] = { 0x8B,0x44,0x24,0x04,0x56,0x25,0xFF,0x00,0x00,0x00,0x8D,0x70,0xFF };
    const uint8_t a_walk[]    = { 0x8B,0x54,0x24,0x04,0x66,0x8B,0x02,0x66,0x3D,0xFF,0xFF,0x75,0x05,
                                  0x33,0xC0,0xC2,0x08,0x00,0x56,0x0F,0xBF,0xF0,0xC1,0xE6,0x0B,
                                  0x66,0x3B,0x41,0x16 };
    const uint8_t a_walk2[]   = { 0x8B,0x54,0x24,0x04,0x66,0x8B,0x02,0x66,0x3D,0xFF,0xFF,0x75,0x05,
                                  0x33,0xC0,0xC2,0x08,0x00,0x56,0x0F,0xBF,0xF0,0xC1,0xE6,0x0B,
                                  0x66,0x3B,0x41,0x14 };
    const uint8_t a_capget[]  = { 0x8B,0x44,0x24,0x04,0x83,0xF8,0x0A,0x77,0x5D,0xFF,0x24,0x85,
                                  0x00,0x00,0x00,0x00 };
    const uint8_t a_stepfwd[] = { 0x8B,0x44,0x24,0x04,0x66,0x3D,0xFF,0xFF,0x75,0x06 };
    const uint8_t a_flstepa[] = { 0x53,0x56,0x8B,0xF1,0x8B,0x0D,0x00,0x00,0x00,0x00,0x57,0xB3,0x01 };
    const uint8_t a_flstepb[] = { 0x53,0x56,0x8B,0xF1,0xB3,0x01,0x8B,0x46,0x20,0x85,0xC0 };
    // the viewport rect: `add eax, <&height>`. The operand is the global's ADDRESS, so the gate can
    // measure the window without a Direct3D callback or needing fulllog open.
    const uint8_t a_vprect[]  = { 0x05,0x00,0x00,0x00,0x00,0x2B,0xC2,0x89,0x74,0x24,0x14,
                                  0x89,0x44,0x24,0x10 };
    // logwindo's box pointer. Needed to reach the LIVE ring (ring = *(box+0x18)) so the indices can
    // be reset before the modulus is narrowed -- see FillConst's restore path.
    const uint8_t a_gslot[]   = { 0xA1,0x00,0x00,0x00,0x00,0x53,0x55,0x56,0x8B,0xF1,0x33,0xDB,
                                  0x0F,0xBF,0x48,0x32,0x89,0x4C,0x24,0x10,0x57,0x8D,0x4C,0x24,
                                  0x28,0x89,0x74,0x24,0x1C,0x89,0x5C,0x24,0x10 };

    enum { RA_CLEAR=0, RA_WRAP, RA_MAXIDX, RA_CAPINIT, RA_WALK, RA_WALK2,
           RA_CAPGET, RA_STEPFWD, RA_FLA, RA_FLB, RA_VPRECT, RA_GSLOT, RA_COUNT };

    const RingAnchor k_anchors[RA_COUNT] = {
        { "ring_clear",   a_clear,   "xxxxxxxxxx",                    sizeof(a_clear)   },
        { "ring_wrap",    a_wrap,    "xxxxxxxxxxxx",                  sizeof(a_wrap)    },
        { "ring_maxidx",  a_maxidx,  "xxxxxxxxxxx",                   sizeof(a_maxidx)  },
        { "cap_init",     a_capinit, "xxxxxxxxxxxxx",                 sizeof(a_capinit) },
        { "ring_walk",    a_walk,    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxx", sizeof(a_walk)    },
        { "ring_walk2",   a_walk2,   "xxxxxxxxxxxxxxxxxxxxxxxxxxxxx", sizeof(a_walk2)   },
        { "cap_get",      a_capget,  "xxxxxxxxxxxx????",              sizeof(a_capget)  },
        { "step_fwd",     a_stepfwd, "xxxxxxxxxx",                    sizeof(a_stepfwd) },
        { "fulllog_stepA",a_flstepa, "xxxxxx????xxx",                 sizeof(a_flstepa) },
        { "fulllog_stepB",a_flstepb, "xxxxxxxxxxx",                   sizeof(a_flstepb) },
        { "viewport_rect",a_vprect,  "x????xxxxxxxxxx",               sizeof(a_vprect)  },
        { "logwindo_slot",a_gslot,   "x????xxxxxxxxxxxxxxxxxxxxxxxxxxxx", sizeof(a_gslot) },
    };

    // kind: 0 = imm8 n | 1 = imm8 -n | 2 = dword n-1 | 3 = dword n*0x200 | 4 = word n-1 | 5 = word n
    struct RingSite { uint8_t anchor; uint32_t off; uint8_t kind; int stock; int alt; };
    const RingSite k_ringSites[] = {
        { RA_CLEAR,   0x0004, 3, 51200, -1 },
        { RA_WRAP,    0x000B, 0,   100, -1 },
        { RA_WRAP,    0x003D, 0,   100, -1 },
        { RA_MAXIDX,  0x000C, 2,    99, -1 },
        { RA_MAXIDX,  0x003E, 2,    99, -1 },
        // Per-channel cap table, mgr+0x64020..29. Each channel is `cmp dl,100 / mov [tbl],dl /
        // jbe / mov byte [tbl],100`
        { RA_CAPINIT, 0x0036, 0,   100, -1 },   // ch0 test
        { RA_CAPINIT, 0x0045, 0,   100, -1 },   // ch0 clamp
        { RA_CAPINIT, 0x0069, 0,   100, -1 },
        { RA_CAPINIT, 0x0078, 0,   100, -1 },
        { RA_CAPINIT, 0x009C, 0,   100, -1 },
        { RA_CAPINIT, 0x00AB, 0,   100, -1 },
        { RA_CAPINIT, 0x00CF, 0,   100, -1 },
        { RA_CAPINIT, 0x00DE, 0,   100, -1 },
        { RA_CAPINIT, 0x0102, 0,   100, -1 },
        { RA_CAPINIT, 0x0111, 0,   100, -1 },
        { RA_CAPINIT, 0x0135, 0,   100, -1 },
        { RA_CAPINIT, 0x0144, 0,   100, -1 },
        { RA_CAPINIT, 0x0168, 0,   100, -1 },
        { RA_CAPINIT, 0x0177, 0,   100, -1 },
        { RA_CAPINIT, 0x019B, 0,   100, -1 },
        { RA_CAPINIT, 0x01AA, 0,   100, -1 },
        { RA_CAPINIT, 0x01CE, 0,   100, -1 },
        { RA_CAPINIT, 0x01DD, 0,   100, -1 },
        { RA_WALK,    0x0041, 4,    99, -1 },
        { RA_WALK2,   0x0038, 5,   100, -1 },
        { RA_CAPGET,  0x0011, 0,   100, -1 },
        { RA_STEPFWD, 0x0013, 5,   100, -1 },
        { RA_FLA,     0x0078, 0,   100, -1 },
        { RA_FLA,     0x007D, 0,   100, -1 },
        { RA_FLA,     0x0082, 1,  -100, -1 },
        { RA_FLB,     0x0064, 0,   100, -1 },
        { RA_FLB,     0x006F, 0,   100, -1 },
        { RA_FLB,     0x0074, 1,  -100, -1 },
    };
    const size_t k_ringN = sizeof(k_ringSites) / sizeof(k_ringSites[0]);
    static_assert(k_ringN <= 64, "k_ringSites outgrew the 64-entry state arrays");

    // Above 127 these eight immediates stop existing: their instructions are re-encoded into the
    // cave and the site becomes a jump, so writing them would land in the middle of that jump.
    bool site_is_caved(const RingSite& s)
    { return s.anchor == RA_WRAP || s.anchor == RA_FLA || s.anchor == RA_FLB; }

    // The six blocks that cannot hold a bound above 127 in place. Four wrap or normalise a ring
    // index (`cmp r32, ib` / `add r32, ib`, sign-extended); two are the rebuild's own stop value
    // (`cmp dx, ib`), which is what decides how many lines a rebuild actually places.
    enum { BK_WRAP1 = 0, BK_WRAP2, BK_FLA, BK_FLB, BK_FILLA, BK_FILLB, BK_COUNT };

    struct RingBlock
    {
        const char* name;
        int         anchor;      // -1 = address comes from the base fix's own site
        uint32_t    off;         // from that anchor
        uint8_t     len;         // bytes replaced by the jump
        uint32_t    backOff;     // fall-through, from the anchor; 0 = block ends in ret
        const uint8_t* stock;    // exact stock bytes
        const char*    mask;     // 'x' compare, '?' ignore
    };

    const uint8_t b_wrap1[] = { 0x83,0xFE,0x64,0x7C,0x02,0x33,0xF6 };
    const uint8_t b_wrap2[] = { 0x83,0xF8,0x64,0x7C,0x02,0x33,0xC0 };
    const uint8_t b_fla[]   = { 0x85,0xC0,0x7D,0x05,0x83,0xC0,0x64,0xEB,0x08,0x83,
                                0xF8,0x64,0x7C,0x06,0x83,0xC0,0x9C,0x89,0x46,0x20 };
    const uint8_t b_flb[]   = { 0x85,0xC0,0x7D,0x0B,0x83,0xC0,0x64,0x89,0x46,0x20,
                                0x8A,0xC3,0x5E,0x5B,0xC3,0x83,0xF8,0x64,0x7C,0x06,
                                0x83,0xC0,0x9C,0x89,0x46,0x20,0x8A,0xC3,0x5E,0x5B,0xC3 };
    const uint8_t b_filla[] = { 0x66,0x83,0xFA,0x00,0x89,0x4D,0x1C,0x7D,0x00 };
    const uint8_t b_fillb[] = { 0x66,0x83,0xF9,0x00,0x89,0x55,0x1C,0x0F,0x8D,0x00,0x00,0x00,0x00 };

    const RingBlock k_blocks[BK_COUNT] = {
        { "ring_wrap+0x09",  RA_WRAP, 0x09, 7,  0x10, b_wrap1, "xxxxxxx" },
        { "ring_wrap+0x3B",  RA_WRAP, 0x3B, 7,  0x42, b_wrap2, "xxxxxxx" },
        { "fulllog_stepA",   RA_FLA,  0x72, 20, 0x86, b_fla,   "xxxxxxxxxxxxxxxxxxxx" },
        { "fulllog_stepB",   RA_FLB,  0x5E, 31, 0,    b_flb,   "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" },
        { "rebuild_boundA",  -1,      0,    9,  9,    b_filla, "xxx?xxxx?" },
        { "rebuild_boundB",  -1,      0,    13, 13,   b_fillb, "xxx?xxxxx????" },
    };

    const int k_ringMax = 200;

    uint8_t* alloc_near(uintptr_t anchor, size_t len)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
        for (uintptr_t d = gran; d < 0x30000000; d += gran)
        {
            for (int below = 0; below < 2; ++below)
            {
                const uintptr_t p = below ? (anchor - d) : (anchor + d);
                if (p < 0x10000 || p > 0x7FFF0000) continue;
                void* m = VirtualAlloc(reinterpret_cast<void*>(p & ~(gran - 1)), len,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (m != nullptr) return static_cast<uint8_t*>(m);
            }
        }
        return nullptr;
    }

    void emit(uint8_t*& p, std::initializer_list<uint8_t> b)
    { for (uint8_t v : b) *p++ = v; }
    void emit_u16(uint8_t*& p, int v)
    { *p++ = static_cast<uint8_t>(v & 0xFF); *p++ = static_cast<uint8_t>((v >> 8) & 0xFF); }
    void emit_i32(uint8_t*& p, int32_t v)
    { for (int i = 0; i < 4; ++i) *p++ = static_cast<uint8_t>((v >> (i * 8)) & 0xFF); }
    void emit_rel32(uint8_t*& p, uintptr_t target)
    { const uintptr_t after = reinterpret_cast<uintptr_t>(p) + 4;
      emit_i32(p, static_cast<int32_t>(static_cast<intptr_t>(target - after))); }

    bool safe_read(uintptr_t a, void* dst, size_t n)
    {
        if (a < 0x10000) return false;
        __try { memcpy(dst, reinterpret_cast<const void*>(a), n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    const int k_constMax   = 127;   // the imm8 ceiling: two wrap tests are `cmp r32, ib`, sign-extended
    const int k_ringStock = 100;


    const uint8_t k_stock  = 50;
    const uint8_t k_target = 99;

    const uint8_t k_colInfo = 0x6A;
    const uint8_t k_colWarn = 0x68;
    const uint8_t k_colFail = 0x44;
    const uint8_t k_colCmd  = 0x02;

    const char HL_ON  = '\x11';
    const char HL_OFF = '\x12';

    // mask: nullptr = exact match
    bool scan_span(const uint8_t* lo, uint32_t span, const uint8_t* pat, size_t len, ScanInfo* r,
                   const char* mask = nullptr)
    {
        __try
        {
            for (uint32_t o = 0; o + len <= span; ++o)
            {
                if (mask == nullptr)
                {
                    if (lo[o] != pat[0]) continue;
                    if (memcmp(lo + o, pat, len) != 0) continue;
                }
                else
                {
                    size_t k = 0;
                    for (; k < len; ++k)
                        if (mask[k] == 'x' && lo[o + k] != pat[k]) break;
                    if (k != len) continue;
                }
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

    void scan_all(const ModuleInfo& m, const uint8_t* pat, size_t len, ScanInfo& out,
                  const char* mask = nullptr)
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
                      vsize, pat, len, &out, mask);
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

    if (m_DiagFp != nullptr)
    {
        char plain[512]; size_t w = 0;
        for (size_t r = 0; raw[r] != '\0' && w + 1 < sizeof(plain); ++r)
            if (raw[r] != '\x11' && raw[r] != '\x12') plain[w++] = raw[r];
        plain[w] = '\0';
        fprintf(m_DiagFp, "      %s\n", plain);
        return;
    }

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
    cm->AddChatMessage(1, false, out);
}

void chatlogfix::Usage(void)
{
    Print(k_colInfo, HL("/chatlogfix") " [" HL("status") "|" HL("diag") "]   (or " HL("/clf") ")");
}

void chatlogfix::Fail(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(m_Fail, sizeof(m_Fail), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log(true, "%s", m_Fail);

    if (m_ToldHowToReport) return;
    m_ToldHowToReport = true;
    Print(k_colFail, "Nothing was patched - this client build is not one chatlogfix knows.");
}

void chatlogfix::FailLoud(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(m_Fail, sizeof(m_Fail), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log(true, "%s", m_Fail);
    Print(k_colFail, "%s", m_Fail);
}

void chatlogfix::Log(bool warn, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m_DiagFp != nullptr)
    {
        fprintf(m_DiagFp, "%s%s\n", warn ? "WARN  " : "      ", buf);
        return;
    }
    if (m_Log == nullptr) return;
    m_Log->Log(static_cast<uint32_t>(warn ? Ashita::LogLevel::Warn : Ashita::LogLevel::Info),
               "chatlogfix", buf);
}

bool chatlogfix::DiagOpen(char* pathOut, size_t n)
{
    if (m_Core == nullptr) return false;
    const char* inst = m_Core->GetInstallPath();
    if (inst == nullptr || inst[0] == '\0') return false;

    const size_t len = strlen(inst);
    const char* sep  = (len > 0 && (inst[len - 1] == '\\' || inst[len - 1] == '/')) ? "" : "\\";
    _snprintf_s(pathOut, n, _TRUNCATE, "%s%slogs\\chatlogfix_diag.log", inst, sep);

    if (fopen_s(&m_DiagFp, pathOut, "a") != 0 || m_DiagFp == nullptr) { m_DiagFp = nullptr; return false; }

    char stamp[64] = "unknown time";
    __time64_t t = _time64(nullptr);
    struct tm lt;
    if (_localtime64_s(&lt, &t) == 0) strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &lt);
    fprintf(m_DiagFp, "\n===== chatlogfix diag  %s =====\n", stamp);
    return true;
}

void chatlogfix::DiagClose(void)
{
    if (m_DiagFp == nullptr) return;
    fclose(m_DiagFp);
    m_DiagFp = nullptr;
}

void chatlogfix::NoteWrite(int i, const WriteInfo& w)
{
    if (w.reprotOk) return;
    Log(true, "%s RVA 0x%06X left PAGE_EXECUTE_READWRITE - restoring the original page protection "
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
               "was loaded from a startup script, reload it once you are in game."
             : (mc == MOD_BAD_PE)
             ? "FFXiMain.dll does not parse as a PE image (DOS/NT header check failed). This is not a "
               "client-version problem - the module handle is wrong. Not patching."
             : "Scanned 0 executable sections of FFXiMain.dll, so 'not found' would mean nothing. "
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
                Fail("Reading FFXiMain's code faulted partway through the scan of %s, so the search "
                     "never completed. This is not a missing signature - see the Ashita log for how "
                     "far it got before it stopped.", k_sigs[i].name);
            }
            else if (m_Scan[i].code == SCAN_NOT_FOUND)
                Fail("%s: signature NOT FOUND (scanned %u executable section(s), %u KB) on FFXiMain "
                     "build 0x%08X. This client build is not one chatlogfix knows - the pattern "
                     "needs re-deriving for it, see BUILD.md.",
                     k_sigs[i].name, m_Scan[i].sections, m_Scan[i].bytes / 1024, m_ImgStamp);
            else
                Fail("%s: signature AMBIGUOUS - matched %u times, not once. Refusing to guess which "
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

// ================================================================================================
// 4K MODE
// ================================================================================================

uintptr_t chatlogfix::Anchor(int idx)
{
    ModuleInfo m;
    if (probe_module(m) != MOD_OK) return 0;
    ScanInfo r;
    scan_all(m, k_anchors[idx].pat, k_anchors[idx].len, r, k_anchors[idx].mask);
    return (r.hits == 1) ? r.hitAt[0] : 0;
}

int chatlogfix::WindowRows(void)
{
    if (m_VpAnchor == 0) m_VpAnchor = Anchor(RA_VPRECT);
    const uintptr_t a = m_VpAnchor;
    if (a == 0) return -1;
    uint32_t pHeight = 0;
    if (!safe_read(a + 1, &pHeight, 4) || pHeight < 0x10000) return -1;
    uint16_t hw = 0;
    if (!safe_read(pHeight, &hw, 2)) return -1;
    const int h = static_cast<int>(hw);
    if (h < 240 || h > 16384) return -1;
    return (h / 16) - 5;
}

bool chatlogfix::RingResetState(void)
{
    const uintptr_t a = Anchor(RA_GSLOT);
    if (a == 0) return false;
    uint32_t slot = 0, box = 0, ring = 0;
    if (!safe_read(a + 1, &slot, 4) || slot < 0x10000) return false;
    if (!safe_read(slot, &box, 4)) return false;
    if (box < 0x10000) return true;
    if (!safe_read(box + 0x18, &ring, 4)) return false;
    if (ring < 0x10000) return true;
    uint16_t z = 0;
    WriteAt(ring + 0x14, &z, 2);                       // newest
    WriteAt(ring + 0x16, &z, 2);                       // oldest
    WriteAt(ring + 0x1a, &z, 2);                       // count
    return true;
}

bool chatlogfix::WriteAt(uintptr_t addr, const void* src, size_t len)
{
    if (addr < 0x10000) return false;
    DWORD op = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), len, PAGE_EXECUTE_READWRITE, &op)) return false;
    memcpy(reinterpret_cast<void*>(addr), src, len);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), len, op, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), len);
    return true;
}

static int ring_read_at(uintptr_t a, uint8_t kind)
{
    if (kind == 2 || kind == 3) return static_cast<int32_t>(*reinterpret_cast<const uint32_t*>(a));
    if (kind == 4 || kind == 5) return static_cast<int32_t>(*reinterpret_cast<const uint16_t*>(a));
    // kind 0 is UNSIGNED: every kind-0 site is a byte operand of an unsigned compare, and a bound
    // of 200 stored through int8_t reads back as -56 and fails its own verify.
    if (kind == 1) return static_cast<int8_t>(*reinterpret_cast<const uint8_t*>(a));
    return static_cast<uint8_t>(*reinterpret_cast<const uint8_t*>(a));
}
static int ring_want(uint8_t kind, int n)
{
    if (kind == 3) return n * 0x200;
    if (kind == 2 || kind == 4) return n - 1;
    if (kind == 1) return -n;
    return n;
}

bool chatlogfix::FillConst(bool on)
{
    if (!on)
    {
        if (!m_ConstOn) { Log(false, "fill mode already off"); return true; }
        if (!RingResetState())
        {
            FailLoud("Cannot reach the chat ring to reset it, so fill mode is being " HL("LEFT ON") ". "
                 "Narrowing the size while the indices are out of range would hang the client. "
                 "Zone or relog, then unload chatlogfix again.");
            return false;
        }
        int bad = 0;
        for (size_t i = 0; i < k_ringN; ++i)
        {
            if (m_RingAddr[i] == 0) continue;
            const uint8_t k = k_ringSites[i].kind;
            if (k == 2 || k == 3) { uint32_t v = static_cast<uint32_t>(m_RingOrig[i]); WriteAt(m_RingAddr[i], &v, 4); }
            else if (k == 4 || k == 5) { uint16_t v = static_cast<uint16_t>(m_RingOrig[i]); WriteAt(m_RingAddr[i], &v, 2); }
            else { int8_t v = static_cast<int8_t>(m_RingOrig[i]); WriteAt(m_RingAddr[i], &v, 1); }
            if (ring_read_at(m_RingAddr[i], k) != m_RingOrig[i]) ++bad;
        }
        if (m_Patched && Ready())
        {
            const uint8_t v = k_target;
            for (int b = 0; b < 2; ++b)
            {
                WriteAt(m_Site[b], &v, 1);
                if (*reinterpret_cast<const uint8_t*>(m_Site[b]) != v) ++bad;
            }
        }
        m_ConstOn = false;
        if (bad == 0) Print(k_colInfo, "Fill mode " HL("OFF") " - ring back to %d records, fill back to %d.",
                            k_ringStock, k_target);
        else          FailLoud("Fill mode restore INCOMPLETE - %d site(s) did not take. "
                           "Run " HL("/clf diag") " and report it.", bad);
        return bad == 0;
    }

    if (m_ConstOn) { Log(false, "fill mode already on"); return true; }
    if (!Ready() || !m_Patched)
    { Fail("Fill mode needs the base fix applied first."); return false; }
    if (m_RelocOn)
    { Fail("The relocated form is already on - this is a chatlogfix bug, please report it."); return false; }

    const int rows = WindowRows();
    if (rows < 0)
    { Fail("Cannot measure the chat window on this client build, so fill mode is refused."); return false; }
    if (rows <= k_ringStock - 1)
    {
        Log(false, "not needed: window holds %d rows, fix fills %d", rows, k_ringStock - 1);
        return false;
    }

    // ---- PASS 1: resolve and verify EVERYTHING before one byte is written -----------------------
    uintptr_t anch[RA_COUNT] = { 0 };
    uintptr_t addr[64] = { 0 };
    int       orig[64] = { 0 };
    for (size_t i = 0; i < k_ringN; ++i)
    {
        const uint8_t ai = k_ringSites[i].anchor;
        if (anch[ai] == 0)
        {
            anch[ai] = Anchor(ai);
            if (anch[ai] == 0)
            { Fail("Fill mode: %s did not resolve uniquely - refusing (nothing was patched).",
                   k_anchors[ai].name); return false; }
        }
        addr[i] = anch[ai] + k_ringSites[i].off;
        orig[i] = ring_read_at(addr[i], k_ringSites[i].kind);
        if (orig[i] != k_ringSites[i].stock &&
            (k_ringSites[i].alt < 0 || orig[i] != k_ringSites[i].alt))
        { Fail("Fill mode: %s+0x%X holds %d, expected %d - refusing (nothing was patched).",
               k_anchors[ai].name, k_ringSites[i].off, orig[i], k_ringSites[i].stock); return false; }
    }

    // ---- PASS 2: write, read back, roll the WHOLE set back on any failure -----------------------
    for (size_t i = 0; i < k_ringN; ++i)
    {
        m_RingAddr[i] = addr[i];
        m_RingOrig[i] = orig[i];
        const uint8_t k = k_ringSites[i].kind;
        const int w = ring_want(k, k_constMax);
        if (k == 3 || k == 2) { uint32_t v = static_cast<uint32_t>(w); WriteAt(addr[i], &v, 4); }
        else if (k == 4 || k == 5) { uint16_t v = static_cast<uint16_t>(w); WriteAt(addr[i], &v, 2); }
        else { int8_t v = static_cast<int8_t>(w); WriteAt(addr[i], &v, 1); }
        if (ring_read_at(addr[i], k) != w)
        {
            for (size_t j = 0; j <= i; ++j)
            {
                const uint8_t kj = k_ringSites[j].kind;
                if (kj == 2 || kj == 3) { uint32_t v = static_cast<uint32_t>(m_RingOrig[j]); WriteAt(m_RingAddr[j], &v, 4); }
                else if (kj == 4 || kj == 5) { uint16_t v = static_cast<uint16_t>(m_RingOrig[j]); WriteAt(m_RingAddr[j], &v, 2); }
                else { int8_t v = static_cast<int8_t>(m_RingOrig[j]); WriteAt(m_RingAddr[j], &v, 1); }
            }
            m_ConstOn = false;
            Fail("Fill mode: write failed at %s+0x%X - rolled back, nothing is patched.",
                 k_anchors[k_ringSites[i].anchor].name, k_ringSites[i].off);
            return false;
        }
    }
    for (int b = 0; b < 2; ++b)
    {
        const uint8_t v = static_cast<uint8_t>(k_constMax - 1);
        WriteAt(m_Site[b], &v, 1);
    }
    m_ConstOn = true;
    Print(k_colInfo, "Filling %d lines for your %d-row window. Reopen the log to rebuild.",
          k_constMax - 1, rows);
    return true;
}

void chatlogfix::AutoApply(void)
{
    if (m_ConstOn || m_RelocOn) return;
    if (!Ready() || !m_Patched) return;

    const int rows = WindowRows();
    if (rows < 0)
    {
        Fail("Could not measure the chat window, so the log may not fill it. Reload chatlogfix once "
             "you are in game.");
        return;
    }
    if (rows <= k_ringStock - 1) return;

    FillApply(true);
}

bool chatlogfix::FillApply(bool on)
{
    if (!on)
    {
        if (!m_ConstOn && !m_RelocOn)
        { Log(false, "fill mode already off"); return true; }
        bool ok = true;
        if (m_RelocOn)  ok = FillReloc(false) && ok;
        if (m_ConstOn) ok = FillConst(false)  && ok;
        return ok;
    }

    if (m_ConstOn || m_RelocOn)
    { Log(false, "fill mode already on"); return true; }
    if (!Ready() || !m_Patched)
    { Fail("Fill mode needs the base fix applied first."); return false; }

    const int rows = WindowRows();
    if (rows < 0)
    { Fail("Cannot measure the chat window on this client build, so fill mode is refused."); return false; }
    if (rows <= k_ringStock - 1)
    {
        Log(false, "not needed: window holds %d rows, base fix fills %d", rows, k_ringStock - 1);
        return false;
    }
    return (rows <= k_constMax - 1) ? FillConst(true) : FillReloc(true);
}

bool chatlogfix::CaveInstall(int n, int fill)
{
    uintptr_t site[BK_COUNT] = { 0 };
    uintptr_t back[BK_COUNT] = { 0 };
    uintptr_t jgeT[BK_COUNT] = { 0 };
    uintptr_t anch[RA_COUNT] = { 0 };

    for (int b = 0; b < BK_COUNT; ++b)
    {
        const RingBlock& k = k_blocks[b];
        if (k.anchor < 0)
        {
            const int which = (b == BK_FILLA) ? 0 : 1;
            if (m_Site[which] == 0) { Fail("Fill mode: the rebuild bound is not resolved - refusing."); return false; }
            site[b] = m_Site[which] - 3;
        }
        else
        {
            if (anch[k.anchor] == 0)
            {
                anch[k.anchor] = Anchor(k.anchor);
                if (anch[k.anchor] == 0)
                { Fail("Fill mode: %s did not resolve uniquely - refusing (nothing was patched).",
                       k_anchors[k.anchor].name); return false; }
            }
            site[b] = anch[k.anchor] + k.off;
        }

        uint8_t cur[32];
        if (!safe_read(site[b], cur, k.len))
        { Fail("Fill mode: %s could not be read - refusing.", k.name); return false; }
        for (uint8_t i = 0; i < k.len; ++i)
            if (k.mask[i] == 'x' && cur[i] != k.stock[i])
            { Fail("Fill mode: %s does not match its stock bytes at +%u - refusing (nothing was "
                   "patched).", k.name, i); return false; }

        memcpy(m_BlkOrig[b], cur, k.len);
        back[b] = (k.anchor < 0) ? site[b] + k.len
                : (k.backOff != 0) ? anch[k.anchor] + k.backOff : 0;
        if (b == BK_FILLA) jgeT[b] = site[b] + 9  + static_cast<int8_t>(cur[8]);
        if (b == BK_FILLB) jgeT[b] = site[b] + 13 + *reinterpret_cast<const int32_t*>(cur + 9);
    }

    m_Cave = alloc_near(m_Base, 0x1000);
    if (m_Cave == nullptr) { Fail("Fill mode: no executable memory within reach of the client - refusing."); return false; }

    // ---- build the cave, then check every jump fits in a rel32 before a single site is touched --
    uint8_t* p = m_Cave;
    uintptr_t entry[BK_COUNT] = { 0 };

    for (int b = 0; b < BK_COUNT; ++b)
    {
        entry[b] = reinterpret_cast<uintptr_t>(p);
        switch (b)
        {
        case BK_WRAP1:                                             // cmp esi,n / jl +2 / xor esi,esi
            emit(p, { 0x81, 0xFE }); emit_i32(p, n);
            emit(p, { 0x7C, 0x02, 0x33, 0xF6 });
            emit(p, { 0xE9 }); emit_rel32(p, back[b]);
            break;
        case BK_WRAP2:                                             // cmp eax,n / jl +2 / xor eax,eax
            emit(p, { 0x3D }); emit_i32(p, n);
            emit(p, { 0x7C, 0x02, 0x33, 0xC0 });
            emit(p, { 0xE9 }); emit_rel32(p, back[b]);
            break;
        case BK_FLA:                                               // normalise [esi+0x20] mod n
            emit(p, { 0x85, 0xC0, 0x7D, 0x07, 0x05 }); emit_i32(p, n);
            emit(p, { 0xEB, 0x0C, 0x3D });              emit_i32(p, n);
            emit(p, { 0x7C, 0x08, 0x2D });              emit_i32(p, n);
            emit(p, { 0x89, 0x46, 0x20 });
            emit(p, { 0xE9 }); emit_rel32(p, back[b]);
            break;
        case BK_FLB:                                               // same, but both paths return
            emit(p, { 0x85, 0xC0, 0x7D, 0x0D, 0x05 }); emit_i32(p, n);
            emit(p, { 0x89, 0x46, 0x20, 0x8A, 0xC3, 0x5E, 0x5B, 0xC3, 0x3D }); emit_i32(p, n);
            emit(p, { 0x7C, 0x08, 0x2D });              emit_i32(p, n);
            emit(p, { 0x89, 0x46, 0x20, 0x8A, 0xC3, 0x5E, 0x5B, 0xC3 });
            break;
        case BK_FILLA:                                             // cmp dx,fill / store / jge stop
            emit(p, { 0x66, 0x81, 0xFA }); emit_u16(p, fill);
            emit(p, { 0x89, 0x4D, 0x1C, 0x0F, 0x8D }); emit_rel32(p, jgeT[b]);
            emit(p, { 0xE9 }); emit_rel32(p, back[b]);
            break;
        case BK_FILLB:                                             // cmp cx,fill / store / jge stop
            emit(p, { 0x66, 0x81, 0xF9 }); emit_u16(p, fill);
            emit(p, { 0x89, 0x55, 0x1C, 0x0F, 0x8D }); emit_rel32(p, jgeT[b]);
            emit(p, { 0xE9 }); emit_rel32(p, back[b]);
            break;
        }
    }

    for (int b = 0; b < BK_COUNT; ++b)
    {
        const intptr_t d = static_cast<intptr_t>(entry[b]) - static_cast<intptr_t>(site[b] + 5);
        if (d > 0x7FFF0000 || d < -0x7FFF0000)
        { Fail("Fill mode: %s is out of jump range of the cave - refusing.", k_blocks[b].name);
          VirtualFree(m_Cave, 0, MEM_RELEASE); m_Cave = nullptr; return false; }
    }

    // ---- write the jumps; roll the whole set back if any one does not take -----------------------
    for (int b = 0; b < BK_COUNT; ++b)
    {
        uint8_t jmp[32];
        memset(jmp, 0x90, sizeof(jmp));
        jmp[0] = 0xE9;
        const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(entry[b]) -
                                                 static_cast<intptr_t>(site[b] + 5));
        memcpy(jmp + 1, &rel, 4);
        WriteAt(site[b], jmp, k_blocks[b].len);
        m_BlkAddr[b] = site[b];

        uint8_t chk[32];
        if (!safe_read(site[b], chk, k_blocks[b].len) || memcmp(chk, jmp, k_blocks[b].len) != 0)
        {
            for (int j = 0; j <= b; ++j)
                if (m_BlkAddr[j] != 0)
                { WriteAt(m_BlkAddr[j], m_BlkOrig[j], k_blocks[j].len); m_BlkAddr[j] = 0; }
            VirtualFree(m_Cave, 0, MEM_RELEASE);
            m_Cave = nullptr;
            Fail("Fill mode: could not redirect %s - rolled back, nothing is patched.", k_blocks[b].name);
            return false;
        }
    }
    return true;
}

void chatlogfix::CaveRemove(void)
{
    for (int b = 0; b < BK_COUNT; ++b)
        if (m_BlkAddr[b] != 0)
        { WriteAt(m_BlkAddr[b], m_BlkOrig[b], k_blocks[b].len); m_BlkAddr[b] = 0; }
    if (m_Cave != nullptr) { VirtualFree(m_Cave, 0, MEM_RELEASE); m_Cave = nullptr; }
}

bool chatlogfix::FillReloc(bool on)
{
    if (!on)
    {
        if (!m_RelocOn) { Log(false, "fill mode already off"); return true; }
        if (!RingResetState())
        {
            FailLoud("Cannot reach the chat ring to reset it, so fill mode is being " HL("LEFT ON") ". "
                 "Narrowing the size while the indices are out of range would hang the client. "
                 "Zone or relog, then unload chatlogfix again.");
            return false;
        }
        int bad = 0;
        for (size_t i = 0; i < k_ringN; ++i)
        {
            if (m_RingAddr[i] == 0) continue;
            const uint8_t k = k_ringSites[i].kind;
            if (k == 2 || k == 3) { uint32_t v = static_cast<uint32_t>(m_RingOrig[i]); WriteAt(m_RingAddr[i], &v, 4); }
            else if (k == 4 || k == 5) { uint16_t v = static_cast<uint16_t>(m_RingOrig[i]); WriteAt(m_RingAddr[i], &v, 2); }
            else { uint8_t v = static_cast<uint8_t>(m_RingOrig[i]); WriteAt(m_RingAddr[i], &v, 1); }
            if (ring_read_at(m_RingAddr[i], k) != m_RingOrig[i]) ++bad;
            m_RingAddr[i] = 0;
        }
        CaveRemove();
        if (m_Patched && Ready())
        {
            const uint8_t v = k_target;
            for (int b = 0; b < 2; ++b)
            {
                WriteAt(m_Site[b], &v, 1);
                if (*reinterpret_cast<const uint8_t*>(m_Site[b]) != v) ++bad;
            }
        }
        m_RelocOn = false;
        m_RelocN  = 0;
        if (bad == 0) Print(k_colInfo, "Fill mode " HL("OFF") " - ring back to %d records, fill back to %d.",
                            k_ringStock, k_target);
        else          FailLoud("Fill mode restore INCOMPLETE - %d site(s) did not take. "
                           "Run " HL("/clf diag") " and report it.", bad);
        return bad == 0;
    }

    if (m_RelocOn) { Log(false, "fill mode already on"); return true; }
    if (!Ready() || !m_Patched)
    { Fail("Fill mode needs the base fix applied first."); return false; }
    if (m_ConstOn)
    { Fail("The plain form is already on - this is a chatlogfix bug, please report it."); return false; }

    memset(m_RingAddr, 0, sizeof(m_RingAddr));
    memset(m_RingOrig, 0, sizeof(m_RingOrig));

    const int rows = WindowRows();
    int n;

    if (rows < 0)
    { Fail("Cannot measure the chat window on this client build, so fill mode is refused."); return false; }
    if (rows <= k_constMax - 1)
    {
        Log(false, "relocation not needed: window %d rows, constants reach %d", rows, k_constMax - 1);
        return false;
    }
    n = (rows + 1 > k_ringMax) ? k_ringMax : rows + 1;


    // ---- PASS 1: resolve and verify every plain site before one byte is written -------------------
    uintptr_t anch[RA_COUNT] = { 0 };
    uintptr_t addr[64] = { 0 };
    int       orig[64] = { 0 };
    for (size_t i = 0; i < k_ringN; ++i)
    {
        if (site_is_caved(k_ringSites[i])) continue;
        const uint8_t ai = k_ringSites[i].anchor;
        if (anch[ai] == 0)
        {
            anch[ai] = Anchor(ai);
            if (anch[ai] == 0)
            { Fail("Fill mode: %s did not resolve uniquely - refusing (nothing was patched).",
                   k_anchors[ai].name); return false; }
        }
        addr[i] = anch[ai] + k_ringSites[i].off;
        orig[i] = ring_read_at(addr[i], k_ringSites[i].kind);
        if (orig[i] != k_ringSites[i].stock)
        { Fail("Fill mode: %s+0x%X holds %d, expected %d - refusing (nothing was patched).",
               k_anchors[ai].name, k_ringSites[i].off, orig[i], k_ringSites[i].stock); return false; }
    }

    // ---- PASS 2: the six relocations. They resolve and verify as a set, or nothing happens -------
    if (!CaveInstall(n, n - 1)) return false;

    // ---- PASS 3: the plain sites --------------------------------------------------------------
    for (size_t i = 0; i < k_ringN; ++i)
    {
        if (addr[i] == 0) continue;
        m_RingAddr[i] = addr[i];
        m_RingOrig[i] = orig[i];
        const uint8_t k = k_ringSites[i].kind;
        const int w = ring_want(k, n);
        if (k == 3 || k == 2) { uint32_t v = static_cast<uint32_t>(w); WriteAt(addr[i], &v, 4); }
        else if (k == 4 || k == 5) { uint16_t v = static_cast<uint16_t>(w); WriteAt(addr[i], &v, 2); }
        else { uint8_t v = static_cast<uint8_t>(w); WriteAt(addr[i], &v, 1); }
        if (ring_read_at(addr[i], k) != w)
        {
            for (size_t j = 0; j <= i; ++j)
            {
                if (m_RingAddr[j] == 0) continue;
                const uint8_t kj = k_ringSites[j].kind;
                if (kj == 2 || kj == 3) { uint32_t v = static_cast<uint32_t>(m_RingOrig[j]); WriteAt(m_RingAddr[j], &v, 4); }
                else if (kj == 4 || kj == 5) { uint16_t v = static_cast<uint16_t>(m_RingOrig[j]); WriteAt(m_RingAddr[j], &v, 2); }
                else { uint8_t v = static_cast<uint8_t>(m_RingOrig[j]); WriteAt(m_RingAddr[j], &v, 1); }
                m_RingAddr[j] = 0;
            }
            CaveRemove();
            m_RelocOn = false;
            Fail("Fill mode: write failed at %s+0x%X - rolled back, nothing is patched.",
                 k_anchors[k_ringSites[i].anchor].name, k_ringSites[i].off);
            return false;
        }
    }

    m_RelocOn = true;
    m_RelocN  = n;
    Print(k_colInfo, "Filling %d lines for your %d-row window. Reopen the log to rebuild.",
          n - 1, rows);
    return true;
}

bool chatlogfix::Apply(void)
{
    if (m_Patched)
    {
        const uint8_t a0 = *reinterpret_cast<volatile uint8_t*>(m_Site[0]);
        const uint8_t b0 = *reinterpret_cast<volatile uint8_t*>(m_Site[1]);
        if (a0 == k_target && b0 == k_target) return true;
        Fail("Chatlogfix thinks it is on, but the sites read %d and %d instead of %d - something "
             "overwrote our patch. Reload chatlogfix to re-take them.", a0, b0, k_target);
        return false;
    }
    if (!Ready() && !Resolve()) return false;

    uint8_t cur[2];
    for (int i = 0; i < 2; ++i) cur[i] = *reinterpret_cast<volatile uint8_t*>(m_Site[i]);
    if (cur[0] != k_stock || cur[1] != k_stock)
    {
        if (cur[0] == k_target && cur[1] == k_target)
            FailLoud("An earlier chatlogfix did not unload cleanly, or a second copy is loaded. "
                     "The log is already filled, but this copy will not restore it. Restart the game.");
        else
            Fail("%s RVA 0x%06X reads %d and %s RVA 0x%06X reads %d, expected the stock %d for both "
                 "- something else is already changing these bytes. Not patching, so it does not get "
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
            Fail("Could not make %s RVA 0x%06X writable (Windows error %u) - no change made.",
                 k_sigs[i].name, Rva(m_Site[i]), w.err);
        else
            Fail("Wrote %d to %s RVA 0x%06X but it reads back %d - the write did not stick, so "
                 "another tool is holding this byte. Not patching.",
                 k_target, k_sigs[i].name, Rva(m_Site[i]), w.got);

        for (int j = 0; j < i; ++j)
        {
            WriteInfo rw;
            const bool back = write_byte(m_Site[j], m_Orig[j], &rw);
            NoteWrite(j, rw);
            if (back) { m_Dirty[j] = false; continue; }
            FailLoud("ROLLBACK FAILED - %s RVA 0x%06X is stuck at %d and chatlogfix cannot put it back "
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
    if (m_RelocOn && !FillReloc(false))
    {
        FailLoud("The base fix was left applied too: its two bytes sit inside relocated code right "
                 "now, and writing them would corrupt it. Zone or relog, then unload again.");
        return false;
    }
    if (m_ConstOn) FillConst(false);
    if (!m_Dirty[0] && !m_Dirty[1]) return false;

    bool allBack = true;
    for (int i = 0; i < 2; ++i)
    {
        if (!m_Dirty[i]) continue;
        const uint8_t cur = *reinterpret_cast<volatile uint8_t*>(m_Site[i]);
        if (cur != k_target)
            Log(true, "%s RVA 0x%06X reads %d before restore, not our %d - something else wrote it "
                      "after we did. Restoring to %d anyway, so the byte is not left on a value nobody "
                      "owns.", k_sigs[i].name, Rva(m_Site[i]), cur, k_target, m_Orig[i]);

        WriteInfo w;
        const bool back = write_byte(m_Site[i], m_Orig[i], &w);
        NoteWrite(i, w);
        if (back) { m_Dirty[i] = false; continue; }

        allBack = false;
        FailLoud("RESTORE FAILED - %s RVA 0x%06X still reads %d and chatlogfix cannot put it back "
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
        Log(true, "INACTIVE - %s", (m_Fail[0] != '\0')
            ? m_Fail
            : "Apply() failed without recording a reason (this is a chatlogfix bug)");
    }
    AutoApply();
    return true;
}

void chatlogfix::Release(void)
{
    if (m_RelocOn)  FillReloc(false);
    if (m_ConstOn) FillConst(false);
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
    Log(true, "RELEASED WITH THE PATCH STILL APPLIED - FFXiMain.dll still holds %d and no chatlogfix "
              "instance owns it any more. Reload chatlogfix, or restart the game.",
        k_target);
}

void chatlogfix::Status(bool full, bool terse)
{
    ModuleInfo m;
    const ModCode mc = probe_module(m);
    if (mc == MOD_OK && m_Base != 0 && m.base != m_Base)
    {
        Log(true, "FFXiMain moved from base 0x%08X to 0x%08X - the resolved sites belong to an image "
                  "that is no longer there. Dropping them and re-resolving.",
            static_cast<unsigned>(m_Base), static_cast<unsigned>(m.base));
        m_Site[0]  = m_Site[1]  = 0;
        m_Dirty[0] = m_Dirty[1] = false;
        m_Patched  = false;
    }
    if (!Ready()) Resolve();

    if (!Ready())
    {
        Print(k_colFail, "INACTIVE - %s", (m_Fail[0] != '\0')
              ? m_Fail
              : "chatlogfix has not applied its patch.");

    }
    else if (m_RelocOn)
    {
        if (terse) Print(k_colInfo, "Currently " HL("ON") " (relocated).");
        else       Print(k_colInfo, HL("ON") " - filling %d lines, ring %d.", m_RelocN - 1, m_RelocN);
    }
    else
    {
        const uint8_t a0 = *reinterpret_cast<volatile uint8_t*>(m_Site[0]);
        const uint8_t b0 = *reinterpret_cast<volatile uint8_t*>(m_Site[1]);
        const bool ours = m_Dirty[0] || m_Dirty[1];
        const uint8_t on_val = m_ConstOn ? static_cast<uint8_t>(k_constMax - 1) : k_target;
        if (a0 == on_val && b0 == on_val)
        {
            if (ours)
            {
                if (terse) Print(k_colInfo, "Currently " HL("ON") ".");
                else       Print(k_colInfo, HL("ON") " - filling %d lines.   Fill mode %s", a0,
                                 m_ConstOn ? HL("ON") : HL("OFF"));
            }
            else
                Print(k_colWarn, "Filling %d lines, but not from this session - an earlier copy "
                                 "left it patched.", k_target);
        }
        else if (a0 == k_stock && b0 == k_stock)
        {
            if (terse) Print(k_colInfo, "Currently " HL("OFF") ".");
            else       Print(k_colInfo, HL("OFF") " - stock %d lines.   Fill mode %s", k_stock,
                             m_ConstOn ? HL("ON") : HL("OFF"));
        }
        else
        {
            Log(true, "unexpected: %s reads %d and %s reads %d (expect %d on, %d off)",
                k_sigs[0].name, a0, k_sigs[1].name, b0, on_val, k_stock);
            Print(k_colWarn, "Something else is changing the same bytes. " HL("/clf diag") " for detail.");
        }
        if (full)
            Log(false, "FFXiMain build 0x%08X, branch A RVA 0x%06X, branch B RVA 0x%06X.",
                  m_ImgStamp, Rva(m_Site[0]), Rva(m_Site[1]));
    }

    if (full)
    {
        ReportBlock((mc == MOD_OK) ? &m : nullptr, "/chatlogfix diag");
        if (mc == MOD_OK) ProbeReport(m);
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

    const bool bare = (args.size() <= 1);
    const char* a = bare ? "status" : args[1].c_str();

    if (_stricmp(a, "status") == 0)
    {
        Status(false, bare);
        if (bare) Usage();
    }
    else if (_stricmp(a, "diag") == 0)
    {
        char path[MAX_PATH];
        if (!DiagOpen(path, sizeof(path)))
        {
            Fail("Could not open the diagnostic file for writing - reporting to the Ashita log "
                 "instead.");
            Status(true);
        }
        else
        {
            Status(true);
            DiagClose();
            Print(k_colInfo, "Diagnostics written to " HL("logs\\chatlogfix_diag.log")
                             " in your Ashita folder.");
        }
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
