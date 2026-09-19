/**
 * chatlogfix - base fix (the two stop-value bytes below), fill mode (the ring constants and the six
 * relocated blocks), and chat serialization (chatserial.cpp). README.md explains each.
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

    // The chat page routine at 0x1622E0 (Sep-10): nothing is written in it, but it and the two routines
    // after it (0x162560, 0x1626E0) call the ring helpers and keep a ring index in a register across
    // the call, so a thread inside them must not see the ring reset or narrowed under it. The writer
    // calls the third one, so an addon thread can be there.
    const uint8_t a_chatpage[] = { 0x83,0xEC,0x0C,0xA1,0x00,0x00,0x00,0x00,0x89,0x4C,0x24,0x00,0x53,0x55,
                                   0x8A,0x48,0x08,0x56,0x84,0xC9 };

    enum { RA_CLEAR=0, RA_WRAP, RA_MAXIDX, RA_CAPINIT, RA_WALK, RA_WALK2,
           RA_CAPGET, RA_STEPFWD, RA_FLA, RA_FLB, RA_VPRECT, RA_CHATPAGE, RA_COUNT };

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
        { "chat_page",    a_chatpage,"xxxx????xxxxxxxxxxxx",           sizeof(a_chatpage) },
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

    // Function extents of every routine holding a site chatlogfix writes, as
    // [anchor, anchor + len): each anchor sits at its function's entry. While a byte in one of these
    // changes, no other thread may be anywhere inside it: it may hold a ring index in a register, sit
    // part-way through a relocated block, or be about to read a bound (freeze.hpp).
    const uint32_t k_anchorFnLen[RA_COUNT] = {
        0x23,  // ring_clear     0x127250..0x127272
        0x9A,  // ring_wrap      0x127280..0x127319
        0x9B,  // ring_maxidx    0x127320..0x1273BA
        0x1F7, // cap_init       0x1281F0..0x1283E6
        0x50,  // ring_walk      0x1292E0..0x12932F
        0x51,  // ring_walk2     0x129330..0x129380
        0x6B,  // cap_get        0x1293F0..0x12945A
        0x27,  // step_fwd       0x129C20..0x129C46
        0x94,  // fulllog_stepA  0x1380E0..0x138173
        0x7D,  // fulllog_stepB  0x138180..0x1381FC (its final ret is inside the relocated block)
        0,     // viewport_rect  (a data read, never written)
        0x9DB, // chat_page      0x1622E0..0x162CBA (three routines that call the ring helpers)
    };
    // The view-reset routine holding the two base-fix bytes: branch A's byte is 0x333 into it, and it
    // is 0x3A8 long (0x1295D0..0x129977).
    const uint32_t k_rebuildFnBefore = 0x333, k_rebuildFnLen = 0x3A8;
    // Two clusters on top of the per-routine spans, so that every direct caller of a ring helper is
    // covered whether or not it is a Ghidra-defined function: from the routine before ring_clear
    // (0x1271C0, the first caller) through the view-reset routine's end (0x129978), which also holds
    // the writer and the two helpers 0x129090/0x129290; and around the reader (0x137840..0x138600),
    // which holds the fulllog steps and their other callers. Wider ranges only cost retries.
    const uint32_t k_clusterABefore = 0x90, k_clusterALen = 0x27B8;    // ring_clear - 0x90 .. + len
    const uint32_t k_clusterBBefore = 0x430, k_clusterBLen = 0xDC0;    // reader - 0x430 .. + len

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

    plog::FileLog g_clfLog;   // logs\chatlogfix\<Name>_<id>\chatlogfix.log.

    // The build stamp in the header of the file this image was loaded from, now; 0 when it cannot be read.
    uint32_t file_stamp_of_self(void)
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&file_stamp_of_self), &self) || self == nullptr)
            return 0;
        char path[MAX_PATH];
        const DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return 0;
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return 0;
        uint8_t head[1024];
        DWORD got = 0;
        const BOOL read = ReadFile(f, head, sizeof(head), &got, nullptr);
        CloseHandle(f);
        if (!read || got < 0x40) return 0;
        uint32_t pe = 0;
        memcpy(&pe, head + 0x3C, 4);
        if (pe > got - 12 || memcmp(head + pe, "PE\0\0", 4) != 0) return 0;
        uint32_t stamp = 0;
        memcpy(&stamp, head + pe + 8, 4);
        return stamp;
    }

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

// The image's record of what it has written (chatlogfix.hpp): set once when the DLL maps, never by a load.
uintptr_t chatlogfix::m_Site[2]          = { 0, 0 };
uint8_t   chatlogfix::m_Orig[2]          = { k_stock, k_stock };
bool      chatlogfix::m_Patched          = false;
bool      chatlogfix::m_Dirty[2]         = { false, false };
uintptr_t chatlogfix::m_Base             = 0;
uint32_t  chatlogfix::m_ImgSize          = 0;
uint32_t  chatlogfix::m_ImgStamp         = 0;
ScanInfo  chatlogfix::m_Scan[2]          = {};
bool      chatlogfix::m_RestoreOk        = true;
bool      chatlogfix::m_ConstOn          = false;
uintptr_t chatlogfix::m_RingAddr[64]     = {};
int       chatlogfix::m_RingOrig[64]     = {};
bool      chatlogfix::m_RelocOn          = false;
int       chatlogfix::m_RelocN           = 0;
uint8_t*  chatlogfix::m_Cave             = nullptr;
uintptr_t chatlogfix::m_BlkAddr[6]       = {};
uint8_t   chatlogfix::m_BlkOrig[6][32]   = {};
uint8_t   chatlogfix::m_BlkPatch[6][32]  = {};
uintptr_t chatlogfix::m_BlkSite[6]       = {};
uint8_t   chatlogfix::m_BaseNow[2]       = { k_stock, k_stock };
int       chatlogfix::m_FillN            = 0;
bool      chatlogfix::m_Pinned           = false;
HANDLE    chatlogfix::m_Sole             = nullptr;
uintptr_t chatlogfix::m_AnchorAddr[16]   = {};
bool      chatlogfix::m_AnchorTried[16]  = {};
uintptr_t chatlogfix::m_VpAnchor         = 0;

chatlogfix::chatlogfix(void)
    : m_Core(nullptr), m_Log(nullptr), m_Id(0), m_ToldHowToReport(false)
{
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
    if (bodyColor == k_colFail && !m_Diag)
    {
        char named[512];
        _snprintf_s(named, sizeof(named), _TRUNCATE, "%s Details: " HL("%s") "%s", raw, LogShown().c_str(), LogNote());
        strcpy_s(raw, sizeof(raw), named);
    }

    if (m_Diag)
    {
        char plain[512]; size_t w = 0;
        for (size_t r = 0; raw[r] != '\0' && w + 1 < sizeof(plain); ++r)
            if (raw[r] != '\x11' && raw[r] != '\x12') plain[w++] = raw[r];
        plain[w] = '\0';
        m_DiagText += "      ";
        m_DiagText += plain;
        m_DiagText += '\n';
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
    if (cm == nullptr) return;   // already in chatlogfix's own log

    char out[576];
    _snprintf_s(out, sizeof(out), _TRUNCATE,
        "\x1E\x51" "[" "\x1E\x06" "chatlogfix" "\x1E\x51" "]" "\x1E\x01" " "
        "\x1E%c" "%s" "\x1E\x01", bodyColor, body);
    cm->AddChatMessage(1, false, out);
}

// Chat only: the usage line, and the unload line (the log has it in its own words, with the run tag).
void chatlogfix::Chat(uint8_t bodyColor, const char* text)
{
    IChatManager* cm = (m_Core != nullptr) ? m_Core->GetChatManager() : nullptr;
    if (cm == nullptr || text == nullptr) return;
    char body[512];
    size_t w = 0;
    for (size_t r = 0; text[r] != '\0' && w + 3 < sizeof(body); ++r)
    {
        if (text[r] == HL_ON)       { body[w++] = '\x1E'; body[w++] = static_cast<char>(k_colCmd); }
        else if (text[r] == HL_OFF) { body[w++] = '\x1E'; body[w++] = static_cast<char>(bodyColor); }
        else                        { body[w++] = text[r]; }
    }
    body[w] = '\0';
    char out[576];
    _snprintf_s(out, sizeof(out), _TRUNCATE,
        "\x1E\x51" "[" "\x1E\x06" "chatlogfix" "\x1E\x51" "]" "\x1E\x01" " " "\x1E%c" "%s" "\x1E\x01", bodyColor, body);
    cm->AddChatMessage(1, false, out);
}

std::string chatlogfix::LogShown(void) { return plog::underRoot(m_Root, g_clfLog.path()); }
// A refused load never moves its startup file, so it gets no "moves" note.
const char* chatlogfix::LogNote(void) { return (!m_Refused && g_clfLog.atStartupFile()) ? " (it moves into your character's log at login)" : ""; }

void chatlogfix::DiagBegin(void)
{
    m_Diag = true;
    m_DiagText.clear();
}

// The report goes into the log as one block; chat says where.
void chatlogfix::DiagEnd(void)
{
    m_Diag = false;
    char who[96];
    _snprintf_s(who, sizeof(who), _TRUNCATE, "chatlogfix %.1f build %08X", GetVersion(), plog::ownImageStamp(&g_clfLog));
    g_clfLog.writeDiag(who, m_DiagText);
    m_DiagText.clear();
    Print(k_colInfo, "Diagnostics written to " HL("%s") "%s.", LogShown().c_str(), LogNote());
}

// The character this client is playing (login status 2, party slot 0): a new one moves the log.
void chatlogfix::FollowCharacter(void)
{
    if (m_Core == nullptr) return;
    IMemoryManager* mm = m_Core->GetMemoryManager();
    IPlayer* player = mm != nullptr ? mm->GetPlayer() : nullptr;
    IParty* party = mm != nullptr ? mm->GetParty() : nullptr;
    if (player == nullptr || party == nullptr || player->GetLoginStatus() != 2) return;
    const char* name = party->GetMemberName(0);
    const uint32_t serverId = party->GetMemberServerId(0);
    if (name == nullptr || name[0] == '\0' || serverId == 0) return;
    const std::string key = plog::characterKey(name, serverId);
    if (key == m_CharKey) return;
    m_CharKey = key;
    g_clfLog.moveToCharacter(plog::characterLogPath(m_Root, "chatlogfix", key), name);
}

// Once a second: the character, and the log's own warnings. Nothing else runs per frame.
void chatlogfix::Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*)
{
    if (m_FrameDead || m_Refused) return;
    const ULONGLONG now = GetTickCount64();
    if (now < m_NextCharCheck) return;
    m_NextCharCheck = now + 1000;
    try
    {
        FollowCharacter();
        if (g_clfLog.takeWriteWarning())
            Print(k_colWarn, "can't write its log (%s).", LogShown().c_str());
        if (g_clfLog.takeTrimWarning())
            Print(k_colWarn, "its log is over 1.5 MB and cannot be trimmed (%s): is another program holding it open?", LogShown().c_str());
    }
    catch (...)
    {
        m_FrameDead = true;
        Log(true, "error: Direct3DPresent: an unexpected error; the once-a-second character check stops for this session");
        Print(k_colFail, "an unexpected error stopped its once-a-second check.");
    }
}

void chatlogfix::Usage(void)
{
    Chat(k_colInfo, HL("/chatlogfix") " [" HL("status") "|" HL("diag") "]   (or " HL("/clf") ")");
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
    if (m_Patched)
        Print(k_colFail, "The base fix is ON; the step that failed is above. Run " HL("/clf status") " for the state.");
    else
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
    if (m_Diag)
    {
        m_DiagText += warn ? "WARN  " : "      ";
        m_DiagText += buf;
        m_DiagText += '\n';
        return;
    }
    g_clfLog.write(warn ? "warn" : "info", buf);
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

    for (int i = 0; i < 2; ++i) scan_all(m, k_sigs[i].pat, k_sigs[i].len, m_Scan[i]);

    if (m_Scan[0].code != SCAN_OK || m_Scan[1].code != SCAN_OK)
    {
        for (int i = 0; i < 2; ++i)
        {
            if (m_Scan[i].code == SCAN_OK) continue;
            if (m_Scan[i].code == SCAN_FAULTED)
            {
                Fail("Reading FFXiMain's code faulted partway through the scan of %s, so the search "
                     "never completed. This is not a missing signature - its log says how far "
                     "it got before it stopped.", k_sigs[i].name);
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
// FREEZE - the DLL pins itself before its first write, and every code write happens with every other
// thread stopped and none of them inside the chat code, either cave, or this DLL (freeze.hpp)
// ================================================================================================

bool chatlogfix::Pin(void)
{
    if (m_Pinned) return true;
    m_Pinned = PinThisModule();
    if (!m_Pinned)
        FailLoud("chatlogfix could not keep itself loaded (Windows error %u), so it changes nothing in the game.",
                 static_cast<unsigned>(GetLastError()));
    return m_Pinned;
}

uintptr_t chatlogfix::AnchorCached(int idx)
{
    if (idx < 0 || idx >= RA_COUNT) return 0;
    if (!m_AnchorTried[idx]) { m_AnchorTried[idx] = true; m_AnchorAddr[idx] = Anchor(idx); }
    return m_AnchorAddr[idx];
}

size_t chatlogfix::CollectRanges(CodeRange* out, size_t max)
{
    size_t n = 0;
    auto add = [&](uintptr_t lo, uintptr_t len) { if (lo != 0 && len != 0 && n < max) out[n++] = CodeRange{ lo, lo + len }; };
    uintptr_t lo = 0, hi = 0;
    if (ModuleRangeOf(reinterpret_cast<const void*>(&PinThisModule), lo, hi)) add(lo, hi - lo);
    else add(0x10000, 0x7FFF0000);   // this DLL's span is unknown: treat every thread as busy
    for (int i = 0; i < RA_COUNT; ++i)
        if (k_anchorFnLen[i] != 0) add(AnchorCached(i), k_anchorFnLen[i]);
    if (Ready()) add(m_Site[0] - k_rebuildFnBefore, k_rebuildFnLen);
    if (AnchorCached(RA_CLEAR) != 0) add(AnchorCached(RA_CLEAR) - k_clusterABefore, k_clusterALen);
    {
        uintptr_t rent = 0;
        if (ChatSerial_ReaderEntry(rent)) add(rent - k_clusterBBefore, k_clusterBLen);
    }
    if (m_Cave != nullptr) add(reinterpret_cast<uintptr_t>(m_Cave), 0x1000);
    n += ChatSerial_Ranges(out + n, max - n);
    return n;
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
static bool ring_write_at(chatlogfix* self, bool (chatlogfix::*w)(uintptr_t, const void*, size_t), uintptr_t a, uint8_t kind, int v)
{
    if (kind == 2 || kind == 3) { uint32_t x = static_cast<uint32_t>(v); return (self->*w)(a, &x, 4); }
    if (kind == 4 || kind == 5) { uint16_t x = static_cast<uint16_t>(v); return (self->*w)(a, &x, 2); }
    if (kind == 1)              { int8_t   x = static_cast<int8_t>(v);   return (self->*w)(a, &x, 1); }
    uint8_t x = static_cast<uint8_t>(v); return (self->*w)(a, &x, 1);
}

// The constants back to stock. A site already stock is forgotten; one holding neither our value nor
// stock belongs to someone else now and is left alone (counted). Writes only.
int chatlogfix::RestoreConstsIn(void)
{
    int bad = 0;
    for (size_t i = 0; i < k_ringN; ++i)
    {
        if (m_RingAddr[i] == 0) continue;
        const uint8_t k = k_ringSites[i].kind;
        const int now = ring_read_at(m_RingAddr[i], k);
        if (now == m_RingOrig[i]) { m_RingAddr[i] = 0; continue; }
        if (now != ring_want(k, m_FillN)) { ++bad; continue; }
        ring_write_at(this, &chatlogfix::WriteAt, m_RingAddr[i], k, m_RingOrig[i]);
        if (ring_read_at(m_RingAddr[i], k) == m_RingOrig[i]) m_RingAddr[i] = 0;
        else ++bad;
    }
    return bad;
}

// The six relocated blocks back to stock; a block that is neither our jump nor stock is someone
// else's and is left alone. The cave is NEVER freed: a thread may still be inside it. Once no block
// points at it any more (and this ran with no thread in it) it is simply forgotten. Writes only.
int chatlogfix::RestoreBlocksIn(void)
{
    int left = 0;
    for (int b = 0; b < BK_COUNT; ++b)
    {
        if (m_BlkAddr[b] == 0) continue;
        const uint8_t len = k_blocks[b].len;
        uint8_t cur[32];
        if (!safe_read(m_BlkAddr[b], cur, len)) { ++left; continue; }
        if (memcmp(cur, m_BlkOrig[b], len) == 0) { m_BlkAddr[b] = 0; continue; }
        if (memcmp(cur, m_BlkPatch[b], len) != 0) { ++left; continue; }
        if (WriteAt(m_BlkAddr[b], m_BlkOrig[b], len) && safe_read(m_BlkAddr[b], cur, len) && memcmp(cur, m_BlkOrig[b], len) == 0)
            m_BlkAddr[b] = 0;
        else
            ++left;
    }
    if (left == 0) m_Cave = nullptr;
    return left;
}

// The two base-fix bytes to `value`, only over the byte we last wrote there (another tool's byte is
// left alone and counted). Never call while a relocated block is still redirected: the bytes sit
// inside its jump. Writes only.
int chatlogfix::WriteBaseIn(uint8_t value)
{
    if (!Ready()) return 0;
    int bad = 0;
    for (int b = 0; b < 2; ++b)
    {
        const uint8_t cur = *reinterpret_cast<volatile uint8_t*>(m_Site[b]);
        if (cur == value) { m_BaseNow[b] = value; continue; }
        if (cur != m_BaseNow[b]) { ++bad; continue; }
        WriteInfo w;
        if (write_byte(m_Site[b], value, &w)) m_BaseNow[b] = value;
        else ++bad;
    }
    return bad;
}

// How many sites no longer hold what chatlogfix last wrote there: the base bytes (unless a relocated block's jump now
// covers them), the constants, and the relocated blocks. A /load of this image takes its earlier load's changes back
// over only when this is 0. Reads only.
int chatlogfix::Overwritten(void)
{
    int n = 0;
    const bool covered[2] = { m_BlkAddr[BK_FILLA] != 0, m_BlkAddr[BK_FILLB] != 0 };
    for (int b = 0; b < 2; ++b)
        if (m_Dirty[b] && !covered[b] && *reinterpret_cast<volatile uint8_t*>(m_Site[b]) != m_BaseNow[b]) ++n;
    for (size_t i = 0; i < k_ringN; ++i)
        if (m_RingAddr[i] != 0 && ring_read_at(m_RingAddr[i], k_ringSites[i].kind) != ring_want(k_ringSites[i].kind, m_FillN)) ++n;
    for (int b = 0; b < BK_COUNT; ++b)
    {
        if (m_BlkAddr[b] == 0) continue;
        uint8_t cur[32];
        if (!safe_read(m_BlkAddr[b], cur, k_blocks[b].len) || memcmp(cur, m_BlkPatch[b], k_blocks[b].len) != 0) ++n;
    }
    return n;
}

// ================================================================================================
// FILL MODE - engages only on a chat window holding more than 99 rows; constants up to 127 rows,
// relocation beyond that
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
    if (m_VpAnchor == 0) m_VpAnchor = AnchorCached(RA_VPRECT);
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

bool chatlogfix::FillConst(void)
{
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
    uintptr_t addr[64] = { 0 };
    int       orig[64] = { 0 };
    for (size_t i = 0; i < k_ringN; ++i)
    {
        const uint8_t ai = k_ringSites[i].anchor;
        const uintptr_t an = AnchorCached(ai);
        if (an == 0)
        { Fail("Fill mode: %s did not resolve uniquely - refusing (nothing was patched).",
               k_anchors[ai].name); return false; }
        addr[i] = an + k_ringSites[i].off;
        orig[i] = ring_read_at(addr[i], k_ringSites[i].kind);
        if (orig[i] != k_ringSites[i].stock &&
            (k_ringSites[i].alt < 0 || orig[i] != k_ringSites[i].alt))
        { Fail("Fill mode: %s+0x%X holds %d, expected %d - refusing (nothing was patched).",
               k_anchors[ai].name, k_ringSites[i].off, orig[i], k_ringSites[i].stock); return false; }
    }
    if (!Pin()) return false;

    // ---- PASS 2: write, read back, roll the WHOLE set back on any failure; all with every other
    //      thread stopped and out of the chat code ------------------------------------------------
    m_FillN = k_constMax;
    int failAt = -1, baseBad = 0, rollbackLeft = 0;
    const bool ran = Frozen([&]
    {
        for (size_t i = 0; i < k_ringN; ++i)
        {
            m_RingAddr[i] = addr[i];
            m_RingOrig[i] = orig[i];
            const uint8_t k = k_ringSites[i].kind;
            const int w = ring_want(k, k_constMax);
            ring_write_at(this, &chatlogfix::WriteAt, addr[i], k, w);
            if (ring_read_at(addr[i], k) != w) { failAt = static_cast<int>(i); m_RingAddr[i] = 0; break; }
        }
        if (failAt < 0)
        {
            baseBad = WriteBaseIn(static_cast<uint8_t>(k_constMax - 1));
            if (baseBad) WriteBaseIn(k_target);
        }
        if (failAt >= 0 || baseBad) rollbackLeft = RestoreConstsIn();
    });
    if (!ran)
    {
        memset(m_RingAddr, 0, sizeof(m_RingAddr));
        Fail("Fill mode: no moment without a game thread in the chat code came within a second - nothing was patched.");
        return false;
    }
    if (failAt >= 0 || baseBad)
    {
        m_ConstOn = (rollbackLeft > 0);
        if (rollbackLeft == 0)
            Fail("Fill mode: write failed at %s - rolled back, nothing is patched.",
                 failAt >= 0 ? k_anchors[k_ringSites[failAt].anchor].name : "the rebuild bound");
        else
            FailLoud("Fill mode: write failed at %s, and %d site(s) of the rollback did not take (left as they are). "
                     "Run " HL("/clf diag") " and report it.",
                     failAt >= 0 ? k_anchors[k_ringSites[failAt].anchor].name : "the rebuild bound", rollbackLeft);
        return false;
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
        Fail("Base fix ON, fill mode not measured: the chat window has no size yet. Reload chatlogfix once "
             "you are in game to fill a tall window.");
        return;
    }
    if (rows <= k_ringStock - 1) return;

    FillApply();
}

bool chatlogfix::FillApply(void)
{
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
    return (rows <= k_constMax - 1) ? FillConst() : FillReloc();
}

// Resolves and verifies the six blocks, allocates and builds the cave, and prepares each site's jump
// (m_BlkPatch). Writes NOTHING into the client: FillReloc does that inside its frozen pass.
bool chatlogfix::CavePrepare(int n, int fill)
{
    uintptr_t site[BK_COUNT] = { 0 };
    uintptr_t back[BK_COUNT] = { 0 };
    uintptr_t jgeT[BK_COUNT] = { 0 };

    for (int b = 0; b < BK_COUNT; ++b)
    {
        const RingBlock& k = k_blocks[b];
        uintptr_t an = 0;
        if (k.anchor < 0)
        {
            const int which = (b == BK_FILLA) ? 0 : 1;
            if (m_Site[which] == 0) { Fail("Fill mode: the rebuild bound is not resolved - refusing."); return false; }
            site[b] = m_Site[which] - 3;
        }
        else
        {
            an = AnchorCached(k.anchor);
            if (an == 0)
            { Fail("Fill mode: %s did not resolve uniquely - refusing (nothing was patched).",
                   k_anchors[k.anchor].name); return false; }
            site[b] = an + k.off;
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
                : (k.backOff != 0) ? an + k.backOff : 0;
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
    FlushInstructionCache(GetCurrentProcess(), m_Cave, 0x1000);

    for (int b = 0; b < BK_COUNT; ++b)
    {
        const intptr_t d = static_cast<intptr_t>(entry[b]) - static_cast<intptr_t>(site[b] + 5);
        if (d > 0x7FFF0000 || d < -0x7FFF0000)
        { Fail("Fill mode: %s is out of jump range of the cave - refusing.", k_blocks[b].name);
          VirtualFree(m_Cave, 0, MEM_RELEASE); m_Cave = nullptr; return false; }   // never published
        memset(m_BlkPatch[b], 0x90, sizeof(m_BlkPatch[b]));
        m_BlkPatch[b][0] = 0xE9;
        const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(entry[b]) - static_cast<intptr_t>(site[b] + 5));
        memcpy(m_BlkPatch[b] + 1, &rel, 4);
        m_BlkSite[b] = site[b];
    }
    return true;
}

bool chatlogfix::FillReloc(void)
{
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
    uintptr_t addr[64] = { 0 };
    int       orig[64] = { 0 };
    for (size_t i = 0; i < k_ringN; ++i)
    {
        if (site_is_caved(k_ringSites[i])) continue;
        const uint8_t ai = k_ringSites[i].anchor;
        const uintptr_t an = AnchorCached(ai);
        if (an == 0)
        { Fail("Fill mode: %s did not resolve uniquely - refusing (nothing was patched).",
               k_anchors[ai].name); return false; }
        addr[i] = an + k_ringSites[i].off;
        orig[i] = ring_read_at(addr[i], k_ringSites[i].kind);
        if (orig[i] != k_ringSites[i].stock)
        { Fail("Fill mode: %s+0x%X holds %d, expected %d - refusing (nothing was patched).",
               k_anchors[ai].name, k_ringSites[i].off, orig[i], k_ringSites[i].stock); return false; }
    }

    // ---- PASS 2: the six relocations resolve and verify as a set, and the cave is built ---------
    if (!CavePrepare(n, n - 1)) return false;
    if (!Pin()) { VirtualFree(m_Cave, 0, MEM_RELEASE); m_Cave = nullptr; return false; }   // never published

    // ---- PASS 3: the jumps, then the plain sites, with every other thread stopped and out of the
    //      chat code; the whole set rolls back on any failure ----------------------------------------
    m_FillN = n;
    int failBlock = -1, failAt = -1, rollbackLeft = 0;
    const bool ran = Frozen([&]
    {
        for (int b = 0; b < BK_COUNT; ++b)
        {
            WriteAt(m_BlkSite[b], m_BlkPatch[b], k_blocks[b].len);
            m_BlkAddr[b] = m_BlkSite[b];
            uint8_t chk[32];
            if (!safe_read(m_BlkSite[b], chk, k_blocks[b].len) || memcmp(chk, m_BlkPatch[b], k_blocks[b].len) != 0)
            { failBlock = b; break; }
        }
        if (failBlock < 0)
        {
            for (size_t i = 0; i < k_ringN; ++i)
            {
                if (addr[i] == 0) continue;
                m_RingAddr[i] = addr[i];
                m_RingOrig[i] = orig[i];
                const uint8_t k = k_ringSites[i].kind;
                const int w = ring_want(k, n);
                ring_write_at(this, &chatlogfix::WriteAt, addr[i], k, w);
                if (ring_read_at(addr[i], k) != w) { failAt = static_cast<int>(i); m_RingAddr[i] = 0; break; }
            }
        }
        if (failBlock >= 0 || failAt >= 0)
        {
            rollbackLeft  = RestoreConstsIn();
            rollbackLeft += RestoreBlocksIn();
        }
    });
    if (!ran)
    {
        VirtualFree(m_Cave, 0, MEM_RELEASE); m_Cave = nullptr;   // never published
        memset(m_RingAddr, 0, sizeof(m_RingAddr));
        memset(m_BlkAddr, 0, sizeof(m_BlkAddr));
        Fail("Fill mode: no moment without a game thread in the chat code came within a second - nothing was patched.");
        return false;
    }
    if (failBlock >= 0 || failAt >= 0)
    {
        m_RelocOn = (rollbackLeft > 0);
        const char* what = failBlock >= 0 ? k_blocks[failBlock].name : k_anchors[k_ringSites[failAt].anchor].name;
        if (rollbackLeft == 0)
            Fail("Fill mode: could not write %s - rolled back, nothing is patched.", what);
        else
            FailLoud("Fill mode: could not write %s, and %d site(s)/block(s) of the rollback did not take - "
                     "left as they are (the cave is kept so they stay valid). Run " HL("/clf diag") " and report it.",
                     what, rollbackLeft);
        return false;
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
        // Loaded again this session: this image's earlier load made these changes and unload left them in. They are
        // taken back over when every site still holds what was written.
        const int lost = Overwritten();
        if (lost == 0)
        {
            Log(false, "took back over what its earlier load left in (base on, fill %s, serialization %s)",
                m_RelocOn ? "relocated" : m_ConstOn ? "on" : "off", ChatSerial_Active() ? "on" : "off");
            return true;
        }
        FailLoud("%d of the sites chatlogfix changed earlier this session hold something else now - another tool "
                 "overwrote them. Restart the game to re-take them.", lost);
        LogSiteBytes(true);
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
    m_BaseNow[0] = cur[0];
    m_BaseNow[1] = cur[1];
    if (!Pin()) return false;

    // Both bytes with every other thread stopped and none inside the view-reset routine that reads
    // them (a rebuild in flight would otherwise see one branch at 99 and the other at 50).
    WriteInfo w[2] = {}, rw[2] = {};
    int failed = -1;
    bool rolled[2] = { true, true };
    const bool ran = Frozen([&]
    {
        for (int i = 0; i < 2; ++i)
        {
            if (write_byte(m_Site[i], k_target, &w[i])) { m_Dirty[i] = true; m_BaseNow[i] = k_target; continue; }
            failed = i;
            for (int j = 0; j < i; ++j)
            {
                rolled[j] = write_byte(m_Site[j], m_Orig[j], &rw[j]);
                if (rolled[j]) { m_Dirty[j] = false; m_BaseNow[j] = m_Orig[j]; }
            }
            break;
        }
    });
    if (!ran)
    { Fail("No moment without a game thread in the chat code came within a second - nothing was patched."); return false; }
    for (int i = 0; i < 2 && (failed < 0 || i <= failed); ++i) NoteWrite(i, w[i]);
    if (failed >= 0)
    {
        const int i = failed;
        if (w[i].code == WRITE_NO_ACCESS)
            Fail("Could not make %s RVA 0x%06X writable (Windows error %u) - no change made.",
                 k_sigs[i].name, Rva(m_Site[i]), w[i].err);
        else
            Fail("Wrote %d to %s RVA 0x%06X but it reads back %d - the write did not stick, so "
                 "another tool is holding this byte. Not patching.",
                 k_target, k_sigs[i].name, Rva(m_Site[i]), w[i].got);
        for (int j = 0; j < i; ++j)
        {
            NoteWrite(j, rw[j]);
            if (!rolled[j])
                FailLoud("ROLLBACK FAILED - %s RVA 0x%06X is stuck at %d and chatlogfix cannot put it back "
                         "(Windows error %u). Restart the game to clear it.",
                         k_sigs[j].name, Rva(m_Site[j]), rw[j].got, rw[j].err);
        }
        m_Patched = m_Dirty[0] || m_Dirty[1];
        return false;
    }

    m_Patched   = true;
    m_RestoreOk = true;
    m_Fail[0]   = '\0';
    return true;
}

// Unload leaves every change in place until the game closes: narrowing the ring back is safe only if no thread holds a
// ring index anywhere up its call stack, and a thread pause sees only instruction pointers. What stays is self-contained
// (the caves call no DLL code, the DLL is pinned), and the image keeps its record of it for a later /load to take back
// over. This only records what stays. Idempotent.
bool chatlogfix::TeardownAll(char* summary, size_t n)
{
    if (m_TornDown) return true;
    m_TornDown = true;
    const bool fill = m_ConstOn || m_RelocOn;
    bool blocks = false;
    for (int b = 0; b < BK_COUNT; ++b) blocks = blocks || m_BlkAddr[b] != 0;
    const bool base = m_Dirty[0] || m_Dirty[1];
    const bool serial = ChatSerial_SitesLeft() > 0;
    if (!fill && !blocks && !base && !serial)
    {
        if (summary != nullptr) _snprintf_s(summary, n, _TRUNCATE, "nothing of chatlogfix's was in the game");
        return true;
    }
    // Unload leaves every change in place until the game closes. Narrowing
    // the ring back to stock is safe only if no thread holds a ring index anywhere up its call stack (a getter returning
    // 110 to a caller whose loop then runs against modulus 100 never ends), and a thread freeze sees only instruction
    // pointers, not what callers hold. What stays in is self-contained: the caves are process allocations that call no
    // DLL code, the constants and base bytes are plain client arithmetic, and the DLL is pinned. So chat keeps working
    // exactly as it did, and a restart gives the stock client back.
    if (summary != nullptr)
        _snprintf_s(summary, n, _TRUNCATE, "its changes stay in until the game closes (fill %s, base %s, serialization %s); a /load of this image takes them back over, a restart gives the stock chat code",
                    fill ? "on" : "off", base ? "on" : "off", serial ? "on" : "off");
    return true;
}

void chatlogfix::SerialLog(void* ctx, bool warn, const char* msg)
{
    chatlogfix* self = reinterpret_cast<chatlogfix*>(ctx);
    if (self == nullptr) return;
    if (warn) self->Print(k_colWarn, "%s Details: " HL("%s") "%s", msg, self->LogShown().c_str(), self->LogNote());
    else      self->Log(false, "%s", msg);
}

bool chatlogfix::Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id)
{
    m_Core = core;
    m_Log  = logger;   // unused: chatlogfix keeps its own log (plugin_log.h)
    m_Id   = id;

    // The log first, so everything below - a refusal too - is in it. Lines are held until the writer starts (on a /load
    // of an image that stayed mapped, its writer already ran once, and they go straight to the file instead).
    m_Root = plog::ashitaRoot(&g_clfLog);
    m_Run  = plog::thisRun();
    g_clfLog.open(m_Root, plog::startupLogPath(m_Root, "chatlogfix", m_Run));
    char ver[16], iface[16];
    _snprintf_s(ver, sizeof(ver), _TRUNCATE, "%.1f", GetVersion());
    _snprintf_s(iface, sizeof(iface), _TRUNCATE, "%.2f", GetInterfaceVersion());
    const std::string session = plog::sessionText("chatlogfix", ver, plog::ownImageStamp(&g_clfLog),
                                                  plog::imageStamp(GetModuleHandleA("FFXiMain.dll")), iface, m_Run);
    g_clfLog.setSession(session, m_Run);
    g_clfLog.write("info", session);

    // One copy per client, and the lock belongs to the image. Once chatlogfix has written anything the image stays mapped
    // until the game closes and keeps the lock, so a /load of it later in the session finds the lock already its own here
    // and takes its earlier load's changes back over (Apply). Any other chatlogfix image finds the lock taken and is
    // refused: it could not tell what this one owns.
    const bool again = (m_Sole != nullptr);
    if (!again)
    {
        char name[64];
        _snprintf_s(name, sizeof(name), _TRUNCATE, "Local\\chatlogfix-sole-instance-%u", static_cast<unsigned>(GetCurrentProcessId()));
        m_Sole = CreateMutexA(nullptr, TRUE, name);
        if (m_Sole == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (m_Sole != nullptr) { CloseHandle(m_Sole); m_Sole = nullptr; }
            m_Refused = true;
            g_clfLog.start();
            FollowCharacter();   // after login the refusal goes straight to the character's log
            Print(k_colFail, "another copy of chatlogfix is loaded, or was earlier this session and stays until the game "
                             "closes; restart the game to load this one.");
            g_clfLog.stop();     // this instance gets no more callbacks: its writer ends here
            m_Core = nullptr;
            return false;
        }
    }
    else
    {
        // The loader hands back the image that stayed mapped, whatever chatlogfix.dll is on disk now.
        Log(false, "loaded again this session: the same image as the earlier load");
        const uint32_t disk = file_stamp_of_self();
        if (disk != 0 && disk != plog::ownImageStamp(&g_clfLog))
            Print(k_colWarn, "This is the build loaded earlier this session; the chatlogfix.dll on disk is a different "
                             "build and loads after a game restart.");
    }

    if (Apply())
    {
        Usage();
        if (!again) Log(false, "applied %d -> %d at branch A RVA 0x%06X, branch B RVA 0x%06X "
                   "(FFXiMain base 0x%08X, build 0x%08X, SizeOfImage 0x%08X)",
            k_stock, k_target, Rva(m_Site[0]), Rva(m_Site[1]),
            static_cast<unsigned>(m_Base), m_ImgStamp, m_ImgSize);
    }
    else
    {
        Log(true, "INACTIVE - %s", (m_Fail[0] != '\0')
            ? m_Fail
            : "Apply() failed without recording a reason (this is a chatlogfix bug)");
    }
    AutoApply();

    // Serialize FFXiMain's chat append vs draw.
    // Independent of the fulllog fill; installs its own SIG-located lock or logs why it did not.
    ChatSerial_Install(&chatlogfix::SerialLog, this);
    if (!m_Pinned) m_Pinned = ChatSerial_Pinned();   // chatserial pinned before its first write

    // Every thread pause of chatlogfix is above: the writer thread starts only now.
    g_clfLog.start();
    const std::string root = m_Root;
    const plog::Run run = m_Run;
    g_clfLog.post([root, run]
    {
        plog::deleteFiles(root, { "logs\\chatlogfix\\chatlogfix.log", "logs\\chatlogfix\\chatlogfix.log.old", "logs\\chatlogfix_diag.log" });
        plog::cleanupStartupFiles(root, "chatlogfix", run);
    });
    FollowCharacter();   // loaded after login: the log moves to the character now
    return true;
}

void chatlogfix::Release(void)
{
    if (m_Refused) return;
    char left[200] = "";
    TeardownAll(left, sizeof(left));
    Log(false, "unloaded; %s%s", left, plog::runSuffix(m_Run).c_str());
    if (m_Pinned) Chat(k_colInfo, "unloaded; its changes stay in until the game closes, and " HL("/load chatlogfix") " takes them back over.");
    // The writer stops last. A stalled share keeps it past 2 s: the DLL must then stay mapped.
    if (!g_clfLog.stop() && !m_Pinned) m_Pinned = Pin();
    // Pinned: the image, its record of what it wrote and the lock stay until the game closes, for a /load of it to take
    // back over. Otherwise (nothing was ever written) the image unmaps, and the lock goes with it.
    if (!m_Pinned && m_Sole != nullptr) { ReleaseMutex(m_Sole); CloseHandle(m_Sole); m_Sole = nullptr; }
    m_Core = nullptr;
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

    if (ChatSerial_Active()) Print(k_colInfo, "Chat serialization " HL("ON") ".");
    else                     Print(k_colInfo, "Chat serialization " HL("OFF") ".");
    if (full) ChatSerial_Diag(&chatlogfix::SerialLog, this);   // detail -> log/diag file

    if (full)
    {
        ReportBlock((mc == MOD_OK) ? &m : nullptr, "/chatlogfix diag");
        if (mc == MOD_OK) ProbeReport(m);
    }
}

bool chatlogfix::Command(const char* command)
{
    std::vector<std::string> args;
    Ashita::Commands::GetCommandArgs(command, &args);
    if (args.empty()) return false;
    if (_stricmp(args[0].c_str(), "/chatlogfix") != 0 && _stricmp(args[0].c_str(), "/clf") != 0)
        return false;
    m_CmdOurs = true;

    const bool bare = (args.size() <= 1);
    const char* a = bare ? "status" : args[1].c_str();

    if (_stricmp(a, "status") == 0)
    {
        Status(false, bare);
        if (bare) Usage();
    }
    else if (_stricmp(a, "diag") == 0)
    {
        DiagBegin();
        Status(true);
        DiagEnd();
    }
    else
    {
        Usage();
    }
    return true;
}

bool chatlogfix::HandleCommand(int32_t mode, const char* command, bool injected)
{
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(injected);
    m_CmdOurs = false;
    try
    {
        return Command(command);
    }
    catch (...)
    {
        m_Diag = false;
        if (!m_CmdOurs) return false;   // it failed before it was known to be ours: leave it to its owner
        Log(true, "error: HandleCommand: an unexpected error");
        Print(k_colFail, "the command failed with an unexpected error.");
        return true;
    }
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
