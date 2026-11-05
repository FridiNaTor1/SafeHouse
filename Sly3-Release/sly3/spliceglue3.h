#pragma once

// Sly 3's splice <-> game glue (the Sly 3 boot to gameplay): the hooks the splice built-ins call (g_splicegame: find-object,
// geto / seto! / addo / ensureo, the global objects) on the native Sly 3 world, with Sly 3's tables:
// - the option ids: optid < 0x718 is a property of the object's class (EopidFromOptid 0x249CF8: the sorted table
//   0x2ED518[cid] = {count, {optid, EOPID*}}), else the object's sidebag (0x24AB90: geto, 0x24AC18: seto!, ...);
// - a property with grf & 1 is a method (0x249D98: a function of the object calling the EOPID's thunk +0x10 with
//   +0x18 arguments); other properties are read by getters registered per EOPID (the native structs keep the fields);
// - the messages to an object (vt slot 3, 0x1C86B8 -> 0x13BB58): the object's splice frame binds symbol `msg` (the
//   message number is the symbol id: m_ext's task 4253 binds 24 = 0x18, SetTaskState's message).
//
// Interfaces for the other parts: register a method body by its original thunk (RegisterSpliceMethod3) or a
// property getter by EOPID (RegisterSpliceGetter3); send an object a message (SendLoMessage3).

#include "../ref.h"

#include <cstdint>
#include <vector>

struct SW;
struct LO;

namespace sly3
{
using PFNSPLICEMETHOD3 = CRef (*)(LO* plo, const std::vector<CRef>& aref);
using PFNSPLICEGETTER3 = bool (*)(LO* plo, CRef* pref);

void RegisterSpliceMethod3(uint32_t pfnThunk, PFNSPLICEMETHOD3 pfn);  // the EOPID's +0x10 (Sly 3 address)
void RegisterSpliceGetter3(int eopid, PFNSPLICEGETTER3 pfn);

// orig3: 0x249CF8 EopidFromOptid (-1: none)
int EopidFromOptid3(int cid, int optid);

// g_splicegame's hooks on psw (the game's world; null uninstalls)
void InstallSpliceHooks3(SW* psw);

// orig3: 0x1C86B8 / 0x13BB58: message msg to the object's splice frame (its handler bound to symbol msg, called
// with the object and the arguments); nothing when the frame does not bind it. Returns true when a handler ran.
bool SendLoMessage3(LO* plo, int msg, int carg = 0, CRef* aref = nullptr, CRef* prefResult = nullptr);
}  // namespace sly3
