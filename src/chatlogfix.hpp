/**
 * chatlogfix - fills the expanded chat log (fulllog) to the full height of its window (the base fix,
 * plus fill mode for windows taller than 99 rows), and serializes the client's chat append against its
 * chat draw with a recursive spinlock (chatserial) so a colored-text flood can no longer corrupt the
 * chat buffer.
 */
#define HL(s) "\x11" s "\x12"

#ifndef CHATLOGFIX_HPP_INCLUDED
#define CHATLOGFIX_HPP_INCLUDED

#include "Ashita.h"
#include "chatserial.hpp"
#include "freeze.hpp"
#include "plugin_log.h"

#include <cstdint>
#include <string>
#include <windows.h>

enum ScanCode : uint8_t { SCAN_OK = 0, SCAN_NOT_FOUND, SCAN_AMBIGUOUS, SCAN_FAULTED };

struct ScanInfo
{
    ScanCode  code;
    uint32_t  hits;        // TOTAL matches across all executable sections
    uintptr_t imm8;        // address of the byte after the pattern; valid only when hits == 1
    uintptr_t hitAt[4];    // first up-to-4 match starts
    uint32_t  sections;
    uint32_t  bytes;
    uint32_t  faulted;
};

enum ModCode : uint8_t { MOD_OK = 0, MOD_NO_MODULE, MOD_BAD_PE, MOD_NO_EXEC_SECTIONS };

struct ModuleInfo
{
    uintptr_t base;
    uint32_t  sizeOfImage;
    uint32_t  timeStamp;
    const IMAGE_SECTION_HEADER* sec;
    unsigned  nsec;
    unsigned  nexec;
};

enum WriteCode : uint8_t { WRITE_OK = 0, WRITE_NO_ACCESS, WRITE_NOT_STUCK };

struct WriteInfo
{
    WriteCode code;
    uint32_t  err;         // GetLastError() from whichever VirtualProtect failed
    uint8_t   got;         // the byte read back afterwards
    bool      reprotOk;
};

class chatlogfix final : public IPlugin
{
    IAshitaCore* m_Core;
    ILogManager* m_Log;
    uint32_t     m_Id;
    bool         m_ToldHowToReport;
    char         m_Fail[256];

    // What chatlogfix has written into the client, and where, belongs to the IMAGE, not to one load: these are static
    // (defined in chatlogfix.cpp). Unload leaves every change in, and once anything is written the DLL is pinned, so a
    // /load later in the session gets this same image back, finds its earlier load's changes recorded here, and takes
    // them back over (Apply).
    static uintptr_t m_Site[2];      // resolved addresses of the two imm8 bytes we patch
    static uint8_t   m_Orig[2];      // their stock values, captured before the first write
    static bool      m_Patched;
    static bool      m_Dirty[2];

    static uintptr_t m_Base;         // FFXiMain base at the last resolve
    static uint32_t  m_ImgSize;      // SizeOfImage, used to clamp hex dumps to the image
    static uint32_t  m_ImgStamp;
    static ScanInfo  m_Scan[2];
    static bool      m_RestoreOk;

    // Mechanism 1 - write the constants. Reaches 127, the largest value the client's imm8 sites hold.
    static bool      m_ConstOn;
    static uintptr_t m_RingAddr[64];
    static int       m_RingOrig[64];

    // Mechanism 2 - re-encode the six imm8 sites into a cave and jump to it. Reaches 200.
    static bool      m_RelocOn;
    static int       m_RelocN;
    static uint8_t*  m_Cave;                   // published caves are never freed (a thread may still be in one)
    static uintptr_t m_BlkAddr[6];
    static uint8_t   m_BlkOrig[6][32];
    static uint8_t   m_BlkPatch[6][32];        // the jmp+NOPs we wrote: removal restores only over these
    static uintptr_t m_BlkSite[6];             // where CavePrepare found each block (written by FillReloc)
    static uint8_t   m_BaseNow[2];             // what we last wrote at the two base sites (ownership check)
    static int       m_FillN;                  // the ring size the constants currently hold (127 or m_RelocN)

    // Unload safety (freeze.hpp): the DLL pins itself before its first write, and every code write happens with every
    // other thread stopped and out of the chat code. The one-copy lock belongs to the image too: a /load of this image
    // takes back over, and any other chatlogfix image is refused.
    bool      m_Refused  = false;
    bool      m_TornDown = false;
    static bool      m_Pinned;
    static HANDLE    m_Sole;
    static uintptr_t m_AnchorAddr[16];
    static bool      m_AnchorTried[16];
    bool      Pin(void);
    uintptr_t AnchorCached(int idx);
    size_t    CollectRanges(CodeRange* out, size_t max);
    template <class Act> bool Frozen(Act&& act)
    {
        CodeRange r[48];
        const size_t n = CollectRanges(r, 48);
        return WhenNoThreadIn(r, n, 1000, act);
    }
    // Write-only steps for inside a frozen pass: no logging, no allocation. Each returns how many
    // sites did NOT end up as intended (a foreign byte is left alone and counts).
    int  RestoreConstsIn(void);
    int  RestoreBlocksIn(void);
    int  WriteBaseIn(uint8_t value);
    int  Overwritten(void);   // how many sites no longer hold what chatlogfix wrote there (reads only)
    bool TeardownAll(char* summary = nullptr, size_t n = 0);

    uintptr_t Anchor(int idx);          // resolve one anchor; 0 = not found / ambiguous / faulted
    int       WindowRows(void);         // rows the chat window holds (viewport H/16 - 5); -1 unknown
    bool      WriteAt(uintptr_t addr, const void* src, size_t len);
    static uintptr_t m_VpAnchor;
    void      AutoApply(void);

    bool      FillApply(void);   // measure the window, pick the cheaper mechanism, report which
    bool      FillConst(void);   // mechanism 1: constants only, no code moved
    bool      FillReloc(void);   // mechanism 2: six blocks relocated
    bool      CavePrepare(int n, int fill);   // resolve and verify all six blocks, build the cave; writes nothing

    bool Resolve(void);
    bool Ready(void) const { return m_Site[0] != 0 && m_Site[1] != 0; }
    bool Apply(void);

    void Usage(void);
    void Print(uint8_t bodyColor, const char* fmt, ...);
    void Fail(const char* fmt, ...);
    void FailLoud(const char* fmt, ...);
    void Log(bool warn, const char* fmt, ...);
    static void SerialLog(void* ctx, bool warn, const char* msg);

    // The log (plugin_log.h): one file per character, the diag report inside it.
    std::string m_Root;                  // the Ashita folder
    plog::Run   m_Run;                   // this run of the game: the run tag on the session and unload lines
    std::string m_CharKey;               // the character the log follows; "" before login
    ULONGLONG   m_NextCharCheck = 0;     // the once-a-second character read in Direct3DPresent
    bool        m_FrameDead = false;     // Direct3DPresent failed once: its once-a-second work stops for the session
    bool        m_CmdOurs = false;       // the command in hand is ours (the guard answers for it)
    bool        m_Diag = false;          // a diag report is being built: Print and Log go into m_DiagText
    std::string m_DiagText;
    void DiagBegin(void);
    void DiagEnd(void);
    void FollowCharacter(void);          // a new character moves the log
    std::string LogShown(void);          // the log's path under the Ashita folder, as chat names it
    const char* LogNote(void);           // " (it moves into your character's log at login)" while at the startup file
    void Chat(uint8_t bodyColor, const char* text);   // chat only, never the log (the usage and unload lines)
    bool Command(const char* command);   // HandleCommand's body, inside the guard

    uint32_t Rva(uintptr_t addr) const
    { return (m_Base != 0 && addr >= m_Base) ? static_cast<uint32_t>(addr - m_Base) : 0; }

    void NoteWrite(int i, const WriteInfo& w);
    void LogSections(const ModuleInfo& m);
    void LogSiteBytes(bool warn);
    void ProbeReport(const ModuleInfo& m);
    void ReportBlock(const ModuleInfo* m, const char* title);
    void Status(bool full, bool terse = false);

public:
    chatlogfix(void);
    // Release records what stays in; this does the same if Release did not run.
    ~chatlogfix(void)
    {
        m_Core = nullptr;
        if (!m_Refused && !m_TornDown) TeardownAll();
    }

    const char* GetName(void) const override { return "chatlogfix"; }
    const char* GetAuthor(void) const override { return "SQLCommit"; }
    const char* GetDescription(void) const override { return "Fills the expanded chat log, and serializes chat append vs draw."; }
    const char* GetLink(void) const override { return "https://github.com/SQLCommit/ChatLogFix"; }
    double GetVersion(void) const override { return 1.3; }
    double GetInterfaceVersion(void) const override { return ASHITA_INTERFACE_VERSION; }
    int32_t GetPriority(void) const override { return 0; }
    uint32_t GetFlags(void) const override
    { return static_cast<uint32_t>(Ashita::PluginFlags::UseCommands) | static_cast<uint32_t>(Ashita::PluginFlags::UseDirect3D); }

    bool Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id) override;
    void Release(void) override;
    // UseDirect3D makes Ashita call this, and the SDK's default returns false: the load then fails "for Direct3D".
    bool Direct3DInitialize(IDirect3DDevice8*) override { return true; }
    bool HandleCommand(int32_t mode, const char* command, bool injected) override;
    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override;
};

#endif // CHATLOGFIX_HPP_INCLUDED
