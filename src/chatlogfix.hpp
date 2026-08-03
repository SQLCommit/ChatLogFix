/**
 * chatlogfix - fills the expanded chat log (fulllog) to the full height of its window.
 *
 * THE FIX. One byte, at each of the two branches of the view-reset function:
 *
 *     cmp word ptr [rebuild line count], 50      ->      cmp word ptr [...], 99
 *
 */
#define HL(s) "\x11" s "\x12"

#ifndef CHATLOGFIX_HPP_INCLUDED
#define CHATLOGFIX_HPP_INCLUDED

#include "Ashita.h"

#include <cstdint>
#include <windows.h>

enum ScanCode : uint8_t { SCAN_OK = 0, SCAN_NOT_FOUND, SCAN_AMBIGUOUS, SCAN_FAULTED };

struct ScanInfo
{
    ScanCode  code;
    uint32_t  hits;        // TOTAL matches across all executable sections
    uintptr_t imm8;        // address of the byte after the pattern; valid only when hits == 1
    uintptr_t hitAt[4];    // first up-to-4 match starts
    uint32_t  sections;    // executable sections actually scanned
    uint32_t  bytes;       // total bytes covered; the positive control for a not-found result
    uint32_t  faulted;     // sections abandoned because the read faulted
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

    uintptr_t m_Site[2];      // resolved addresses of the two imm8 bytes we patch
    uint8_t   m_Orig[2];      // their stock values, captured before the first write
    bool      m_Patched;
    bool      m_ToldHowToReport;
    bool      m_Dirty[2];

    uintptr_t m_Base;         // FFXiMain base at the last resolve
    uint32_t  m_ImgSize;      // SizeOfImage, used to clamp hex dumps to the image
    uint32_t  m_ImgStamp;     // PE TimeDateStamp: which client build this report is about
    ScanInfo  m_Scan[2];
    char      m_Fail[256];
    bool      m_RestoreOk;

    bool Resolve(void);
    bool Ready(void) const { return m_Site[0] != 0 && m_Site[1] != 0; }
    bool Apply(void);
    bool Restore(void);

    void Usage(void);
    void Print(uint8_t bodyColor, const char* fmt, ...);
    void Fail(const char* fmt, ...);
    void Log(bool warn, const char* fmt, ...);

    uint32_t Rva(uintptr_t addr) const
    { return (m_Base != 0 && addr >= m_Base) ? static_cast<uint32_t>(addr - m_Base) : 0; }

    void NoteWrite(int i, const WriteInfo& w);
    void LogSections(const ModuleInfo& m);
    void LogSiteBytes(bool warn);
    void ProbeReport(const ModuleInfo& m);
    void ReportBlock(const ModuleInfo* m, const char* title);
    void Status(bool full);

public:
    chatlogfix(void);
    ~chatlogfix(void) { if (m_Dirty[0] || m_Dirty[1]) { m_Core = nullptr; Restore(); } }

    const char* GetName(void) const override { return "chatlogfix"; }
    const char* GetAuthor(void) const override { return "SQLCommit"; }
    const char* GetDescription(void) const override { return "Fills the expanded chat log to the full window height."; }
    const char* GetLink(void) const override { return ""; }
    double GetVersion(void) const override { return 1.0; }
    double GetInterfaceVersion(void) const override { return ASHITA_INTERFACE_VERSION; }
    int32_t GetPriority(void) const override { return 0; }
    uint32_t GetFlags(void) const override
    { return static_cast<uint32_t>(Ashita::PluginFlags::UseCommands); }

    bool Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id) override;
    void Release(void) override;
    bool HandleCommand(int32_t mode, const char* command, bool injected) override;
};

#endif // CHATLOGFIX_HPP_INCLUDED
