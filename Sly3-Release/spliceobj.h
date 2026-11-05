#pragma once

// The splice runtime's view of the game's objects: the hooks the object model installs so the object
// built-ins work (addo / ensureo / seto! / geto, find-object(s), the global objects). Without a hook the
// built-in counts a stub call (g_splicediag) and returns Nil.

#include <glm/glm.hpp>

#include "ref.h"

struct SPLICEGAME
{
    // vector-ballistic-velocity's optional object: its acceleration (the original: +0xE8's +0x80)
    void (*pfnVecAcceleration)(BASIC* pbasic, glm::vec3* pacc) = nullptr;
    // The option primitives on an object (0x123EB0): bifk BIFK_AddO (0x200CA8 RefAddOption), BIFK_EnsureO
    // (0x200D10), BIFK_SetO (0x200C28 RefSetOption): pref the value, result void; BIFK_GetO (0x200BA0
    // RefGetOption; the class tables of spliceobj 0x1FFCF8): pref null, result the option's value.
    CRef (*pfnRefOption)(int bifk, BASIC* pbasic, int optid, const CRef* pref) = nullptr;
    // FindObject 0x15E958 (G, kind, name, relative object, scope's +0x3C / the game's +0x3F0C): kind 1
    // (relative to an object), 4, 5 (global)
    BASIC* (*pfnFindObject)(int kind, SYMID symidName, BASIC* pbasicRel, BASIC* pbasicScope) = nullptr;
    // Find by name, 0x15E678 (built-ins 113/116; up to cMax).
    int (*pfnFindObjects)(int kind, SYMID symidName, BASIC* pbasicRel, int cMax, BASIC** apbasic) = nullptr;
    // Find by class, 0x15E9C8 (built-in 120; kind 0x401 relative / 0x405 global).
    int (*pfnFindObjectsByClass)(int kind, int cid, BASIC* pbasicRel, int cMax, BASIC** apbasic) = nullptr;
    // The global objects of the built-ins without arguments (100..102: the objects in 0x3E0F9C / 0x3E0FA0 /
    // 0x3E0FA4; 117 the player; 118 the world 0x3DD040; 119 the camera)
    BASIC* (*pfnPbasicGlobal)(int bifk) = nullptr;
};

extern SPLICEGAME g_splicegame;
