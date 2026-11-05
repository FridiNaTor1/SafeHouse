#pragma once

// The compiled splice code in world files: a list of cells, each a byte (0xFF: no cell; bit 7: a next cell
// follows; bits 0..6: the tag), with g_fLoadDebugInfo a debug header (S32 + 6 x S16), then the payload.

#include "pair.h"

class CBinaryInputStream;

// orig: 0x0012A3D8
std::shared_ptr<CPair> PpairSerializeIn(CBinaryInputStream* pbis);

// The world group's debug flag (0x3E1C34: the S32 before each group's scripts block); 0 in the shipped data.
extern int g_fLoadDebugInfo;

// Statistics of the loaded cells (for --parse-splice): cells per tag, and objects (tag 33) with a nonzero id.
struct SERIALIZESTATS
{
    long acpairTagk[128] = {};
    long cbasicNonzero = 0;
};
extern SERIALIZESTATS g_serializestats;
