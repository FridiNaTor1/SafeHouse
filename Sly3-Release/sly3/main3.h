#pragma once

// The Sly 3 commands (main3.cpp).

#include <string>

struct DUMPWORLD3OPTS
{
    std::string out;        // --out: the decompressed file
    bool fGroups = false;   // --groups: one line per group
    bool fObjects = false;  // --objects: every top-level object header
    bool fCensus = false;   // --census (--load-world): every object created
    int keyA = -1, keyB = -1;
    std::string asegCheck;  // --aseg-check3 FILE (--load-world): tools/asegram3.py's segments set up natively (anim3.h)
    std::string fields;     // --fields OID,OID... (--load-world): the loaded fields of these objects (census3.py fields)  // --arrival A:B (--load-world): the arrival keys (orig3 GAME +0x1498 / +0x14A0)
};

int ListKeys3();
int ListFiles3();
// W: a world index (0..39), its name (m_ext), its key's address, SECTOR:CB, or "all" (a summary line per world)
int DumpWorld3Command(const std::string& arg, const DUMPWORLD3OPTS& o);
// The objects of world W created (sly3/load3.h): a report of the load, its errors, and with fCensus every object
int LoadWorld3Command(const std::string& arg, const DUMPWORLD3OPTS& o);
// The launcher's start: orig3 0x1C6EA8's two logo movies (fHeadless: decoded without a window)
int Boot3(bool fHeadless);
