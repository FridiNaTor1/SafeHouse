#pragma once

// The splice built-ins (0x11F440..0x126000, table 0x2BBB90). The language's own (arithmetic, predicates,
// lists, vectors / matrices, CLQ / LM, math) are implemented here; the game's (objects, options, sounds,
// camera, progress, ...: 97..168 and the object parts of 108..128) go through hooks the game modules install
// with SetBifGameHook; without a hook they are stubs that count their calls (g_splicediag) and return Nil.

#include <vector>

#include "eval.h"

// The arguments of a built-in as a flat list: the fixed ones, then the rest list's elements.
std::vector<CRef> ArefFlatten(BIFK bifk, int argc, CRef* argv);

void SetBifGameHook(BIFK bifk, PFNBIF pfnbif);
[[nodiscard]] bool FBifImplemented(BIFK bifk);

// The current time for current-time (96; the game's clock sets it).
extern float g_tSplice;
// Random numbers for random (53): the game may install its generator (default: a fixed LCG).
extern float (*g_pfnGRandomSplice)(float gMin, float gMax);
extern int (*g_pfnNRandomSplice)();
