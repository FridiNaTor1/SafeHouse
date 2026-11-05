#pragma once

// The world's splice side: each world group's scripts block (0x1FF858) is a table of compiled modules; each
// module runs once at load in a frame of its own (0x1FFA70); objects reach a module's frame by its index in
// the current table (0x1FFA40).

#include <memory>
#include <string>
#include <vector>

#include "frame.h"
#include "pair.h"

class CBinaryInputStream;

// A script table entry (8 bytes {node, frame}).
struct SPLC
{
    std::shared_ptr<CPair> ppairCompile;   // the module's code (a list; its first cell is evaluated)
    std::shared_ptr<CFrame> pframe;        // its frame once evaluated
    CRef refResult;                        // the value of its code (not kept by the original)
    std::string achzObjectPath;            // the three debug strings (only with g_fLoadDebugInfo)
    std::string achzFile;
    std::string achzSpliceSource;
};

// A script table (0x10 bytes {a, c, entries, next}).
struct SPLCT
{
    int a = 0;  // the group's word at +0x10
    std::vector<SPLC> asplc;
};

// The splice state of a world (SW +0x39E8 current table, +0x39EC the list, +0x3A14 the splice side's flag).
struct SWSPLICE
{
    std::vector<std::unique_ptr<SPLCT>> apsplct;  // oldest first (the original's list is newest first)
    SPLCT* psplctCur = nullptr;
    int fOn = 0;
    std::shared_ptr<CFrame> pframe;  // 0x1FF7E8's frame
};

// The SW's splice state. Until SW (the object model) embeds an SWSPLICE, it lives here keyed by the SW;
// a null SW is a valid key (the standalone checks).
SWSPLICE* PswspliceFromSw(SW* psw);
void DeleteSwSplice(SW* psw);

// orig: 0x001FF7E8 / 0x001FF828 (the splice side on: a root frame for the world / off)
void StartupSwSplice(SW* psw);
void ShutdownSwSplice(SW* psw);

// orig: 0x001FF858 LoadSwSpliceFromBrx (the group's scripts block: count, modules (0x12A3D8), each evaluated
// (0x1FFA70), the table becomes the current one). a is the group's word at +0x10.
void LoadSwSpliceFromBrx(SW* psw, int a, CBinaryInputStream* pbis);
// The parse alone (no evaluation): the table, or null for an empty block.
std::unique_ptr<SPLCT> PsplctLoadFromBrx(int a, CBinaryInputStream* pbis);
// orig: 0x001FFA70 (entry isplc's module evaluated once in a new root frame; the result is void)
CRef RefEvalModule(SPLCT* psplct, int isplc);

// The end of a group's object load (0x13DA18 clears SW +0x39E8 after LoadSwObjsFromBrx): no current table.
void EndSwSpliceGroup(SW* psw);

// orig: 0x001FFA40 (the frame of entry isplice of the current table; null for -1)
std::shared_ptr<CFrame> PframeFromIsplice(int isplice, SW* psw);
