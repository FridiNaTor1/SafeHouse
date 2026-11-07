#pragma once

// Sly 3's scripts (SCPT, class 0x87, 0xB0 bytes; Sly 2's class 0x72): the command list as orig3 0x225220 reads it
// (from the class's load 0x2250C0, after LoadLoFromBrx 0x1C7D98 = the options). Sly 3 re-encoded the commands: each
// is 16 bytes in RAM (SCPT +0x40 count, +0x44 array), a header word {U8 id (0..0x77) | bit 8 a flag | bits 9..19 the
// first child | bits 20..30 the next sibling} (a tree: blocks nest, the executor 0x226D00 walks it with a stack at
// the runner's +0x118 / +0x11C), then +4 an S32 (the object), +8.. the id's arguments (the reader's table 0x326250).
// Sly 2's were a flat list of 0x50-byte commands with 91 ids.

#include <cstdint>
#include <string>
#include <vector>

class CBinaryInputStream;

struct SCPTCMD3
{
    int id = 0;              // +0 (U8)
    int32_t oid = 0;         // +4
    bool fFlag = false;      // +0 bit 8
    int ichild = 0;          // +0 bits 9..19 (0: none; an index into the list)
    int isib = 0;            // +0 bits 20..30
    std::vector<int32_t> an; // the arguments as read (S8 / S16 / U16 / S32 widened, F32 / vectors as their bits)
};

// orig3: 0x225220 (S16 count, then per command: S8 id, S32, S8 flag, U16 child, U16 sibling, the id's arguments)
std::vector<SCPTCMD3> AcmdLoadScpt3(CBinaryInputStream* pbis);

// The SCPT object (class 0x87, orig3 0x2250C0: LoadLoFromBrx, then the command list 0x225220). scptrun3.h's SCPT3
// is the executor's record of it (made at the world start: scptgame3.cpp).
#include "../lo.h"
struct LOSCPT3 : LO
{
    std::vector<SCPTCMD3> acmd;  // +0x40 count, +0x44 the 16-byte commands
};

// a short name per command id (the Sly 2 twins)
const char* SzScptCmd3(int id);
