// Sly 3's SCPT command list (scpt3.h).

#include "scpt3.h"

#include "../bis.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>

namespace
{
constexpr int CID3_SCPT = 0x87;

// The argument reads of each id (orig3 0x225220's jump table 0x326250, 48 distinct cases; the ids m_ext's first
// scripts use are checked read for read against the reference's SLY2_LOADTRACE of the m_ext load). Letters: b S8, h S16,
// u U16, w S32 / U32, f F32, v a vector (ReadVector, 12 bytes), c ReadCb12, e ReadCb8; "B" an S8 count n then n S32;
// "P" an S8 count n then n {S32, S32}.
const char* SzArgs(int id)
{
    switch (id)
    {
    case 0x00: return "u";
    case 0x0C: return "B";
    case 0x0D: return "Bw";
    case 0x19: return "wwwb";
    case 0x1D: return "cef";
    case 0x1F: return "bb";
    case 0x22: return "wbb";
    case 0x2E: return "www";
    case 0x2F: return "ew";
    case 0x30: return "wffwb";
    case 0x33: return "wbbfw";
    case 0x34: return "wfbfw";
    case 0x35: return "wff";  // (0x2256E4: S32, F32, then the shared F32 tail 0x225824)
    case 0x37: return "wff";
    case 0x36: return "wbbb";
    case 0x3E: case 0x44: case 0x59: case 0x5A: return "h";
    case 0x55: case 0x56: return "hb";
    case 0x3F: case 0x4F: case 0x57: case 0x73: return "b";
    case 0x40: return "fb";
    case 0x58: return "fw";   // (0x225BC0: F32, then the S32 of 0x225C68's second read)
    case 0x4B: case 0x5B: return "f";
    case 0x41: case 0x42: return "wwf";
    case 0x43: return "hwwwfff";
    case 0x45: return "vvfffuwwvw";
    case 0x46: return "wwvvfwww";
    case 0x4A: return "vvf";
    case 0x4E: return "ww";
    case 0x52: return "Bbwwwf";   // 0x52 only: after the list an S8, three S32 and an F32 (0x225B3C)
    case 0x53: return "B";
    case 0x5C: return "wbwb";
    case 0x26: case 0x38: case 0x39: case 0x60: return "wf";
    case 0x04: case 0x07: case 0x08: case 0x0F: case 0x10: case 0x1A: case 0x28: case 0x2A: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x62: case 0x63: case 0x68: return "ww";
    case 0x6B: return "wwubw";
    case 0x6C: return "wP";
    case 0x0E: case 0x13: case 0x1C: case 0x1E: case 0x6F: case 0x70: case 0x74: return "wb";
    case 0x16: case 0x75: return "wwb";
    case 0x76: return "wbbbb";
    case 0x0B: case 0x11: case 0x12: case 0x15: case 0x17: case 0x18: case 0x1B: case 0x20: case 0x21: case 0x24:
    case 0x25: case 0x29: case 0x2B: case 0x2C: case 0x2D: case 0x48: case 0x49: case 0x4D: case 0x51: case 0x61:
    case 0x65: case 0x66: case 0x67: case 0x6A: case 0x71: case 0x72: case 0x77: return "w";
    default: return "";  // 0x01..0x03, 0x05, 0x06, 0x09, 0x0A, 0x14, 0x23, 0x27, 0x31, 0x32, 0x47, 0x4C, 0x50, ...: none
    }
}
}  // namespace

const char* SzScptCmd3(int id)
{
    static const char* const s_asz[0x78] = {
        "spawn", "block1", "block2", "block3", "while", "block5", "if", "case7", "case8", "else9",          // 00
        "blockA", "retire", "blockC", "blockD", "blockE", "alarmF", "alarm10", "stopscpt", "checkpoint", "x13",  // 0A
        "label?", "vcall15", "aseg", "startscpt", "x18", "dock", "near1A", "near1B", "near1C", "x1D",       // 14
        "chkptasks", "x1F", "block20", "x21", "interact", "cutcm", "x24", "x25", "nearcrv", "block27",       // 1E
        "x28", "interact5", "carry", "interact9", "effect", "action5", "prize", "rand", "anim30", "x31",     // 28
        "x32", "job33", "job34", "x35", "drop36", "launch", "add", "remove", "x3A", "x3B",                  // 32
        "geomon", "geomoff", "byclass", "joltcm", "x40", "x41", "eyes", "asegsnd", "asegsndstop", "block45", // 3C
        "openbinoc", "x47", "cmfocus", "cmfocus49", "cmtargets", "cmzoom", "cmsnap", "x4D", "x4E", "block4F", // 46
        "title", "block51", "dialog", "dialogwait", "subreset", "holdfx", "releasefx", "x57", "x58", "pushmusic", // 50
        "popmusic", "block5B", "murrayhold", "x5D", "x5E", "block5F", "hint", "hintclear", "guardcmd", "x63", // 5A
        "x64", "x65", "label66", "transition", "exit", "x69", "x6A", "x6B", "x6C", "x6D",                  // 64
        "closebinoc", "proxy", "clue70", "clue71", "clue72", "x73", "x74", "booty", "cnstr", "face"};       // 6E
    return id >= 0 && id < 0x78 ? s_asz[id] : "?";
}

std::vector<SCPTCMD3> AcmdLoadScpt3(CBinaryInputStream* pbis)
{
    const int c = pbis->S16Read();
    std::vector<SCPTCMD3> acmd(std::size_t(std::max(c, 0)));
    for (SCPTCMD3& cmd : acmd)
    {
        cmd.id = pbis->S8Read() & 0xFF;
        cmd.oid = pbis->S32Read();
        cmd.fFlag = (pbis->S8Read() & 1) != 0;
        cmd.ichild = pbis->U16Read() & 0x7FF;
        cmd.isib = pbis->U16Read() & 0x7FF;
        if (cmd.id >= 0x78) continue;  // (the original's switch reads nothing past its table)
        for (const char* pch = SzArgs(cmd.id); *pch; ++pch)
            switch (*pch)
            {
            case 'b': cmd.an.push_back(pbis->S8Read()); break;
            case 'h': cmd.an.push_back(pbis->S16Read()); break;
            case 'u': cmd.an.push_back(pbis->U16Read()); break;
            case 'w': cmd.an.push_back(pbis->S32Read()); break;
            case 'f': cmd.an.push_back(std::bit_cast<int32_t>(pbis->F32Read())); break;
            case 'v': for (int i = 0; i < 3; ++i) cmd.an.push_back(std::bit_cast<int32_t>(pbis->F32Read())); break;
            case 'c': for (int i = 0; i < 3; ++i) cmd.an.push_back(pbis->S32Read()); break;
            case 'e': for (int i = 0; i < 2; ++i) cmd.an.push_back(pbis->S32Read()); break;
            case 'B':
            {
                const int n = pbis->S8Read();
                cmd.an.push_back(n);
                for (int i = 0; i < n; ++i) cmd.an.push_back(pbis->S32Read());
                break;
            }
            case 'P':
            {
                const int n = pbis->S8Read();
                cmd.an.push_back(n);
                for (int i = 0; i < 2 * n; ++i) cmd.an.push_back(pbis->S32Read());
                break;
            }
            }
    }
    return acmd;
}

