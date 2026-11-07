#pragma once

// PC addition, not in the original game (debug menu only): other worlds' files
// loaded while the current world keeps running, their objects used in it. Built for the guest characters
// (pccharacter.h: Carmelita, Sly's tango outfit) and for the playable guards and bosses (pcguard.cpp).
//
// A "kept world" is a world record's file loaded into an SW of its own, never updated and never freed (its objects are
// pointed at for the rest of the run; about 40 MB each). Loading one does not touch the running world:
//   - the file is read and decompressed on a background thread;
//   - then, on the game thread between two steps (PumpPcKeptWorlds, from the debug menu's step), every global table the
//     world loader fills (worldglobals.h: guards, physics, shaders, bone bindings, splice hooks, ...) is moved aside,
//     the file is loaded into its own SW, its records' run-time parts and bone bindings made (no object's PostLoad, no
//     script, no update), its objects taken out of every table, its textures moved into the kept world, and the running
//     world's tables are moved back. A short hitch (measured 20 .. 160 ms on the game thread), no transition, no fade.
//
// The contract (stable; the guards' controllers build on it):
//
//   RequestPcKeptWorld(iwrec)    asks for a world record's file (any number; repeated calls are free). Never reloads.
//   PckwsKeptWorld(iwrec)        None / Loading / Ready / Failed. Poll it each step (no callbacks).
//   PswKeptWorld(iwrec)          the kept SW once Ready (else null). Its objects are inert: not in the world, in no
//                                update table (iupd -1, fInWorld false). Do not add them to the running world.
//   PloKeptFind(iwrec, oid, cid) its first object of the oid (not a copy), derived from cid (0: any); null if none.
//   PaabindKept(palo)            a kept ALO's rest-pose bone bindings (made at its load), to give back to the bone table
//                                (SetAloBoneBindings) each time the running world starts (a world's start clears it).
//   ShdsKept(iwrec)              (internal: its textures and shaders, for the renderer)
//
//   Two ways to use a kept object in the running world:
//   1. Drawn, not simulated (a puppet: Carmelita's model): add its ALOs to the scene (AppendPcCharSceneAlos's way),
//      pose it yourself (ApplyAseg with g_psw set to the kept SW while it is made, then driven by you), place its root
//      record. It never enters the running world.
//   2. A live copy in the running world (her pistol's projectile; a guard): PloPcCloneIntoWorld(psw, ptmpl) clones the
//      kept object and its children into psw (PloCloneLo, the cross-world copy the loader also makes) and starts them as
//      StartSw starts a loaded object (records, bone bindings, ASEGs / SMs, the class's PostLoad, the start script),
//      then takes the copy out of the world (fAdd false) or leaves it in (fAdd true). The copy is the running world's:
//      freed with it at the next transition. Make your copies again after every world start (OnPcWorldStarted below).
//      Copies cannot be deleted while the world runs: keep and reuse them (RemoveLo / AddLo), or leave them out of the
//      world when switching back.
//
//   Hooks for the controllers (called by transition.cpp / debugmenu.cpp; register with RegisterPcWorldHooks):
//     pfnLoaded(psw)   after a world's load, before its start (copies made here start with the world's own objects)
//     pfnStarted(psw)  after a world's start
//     pfnStep(psw, fDebugKeys)  each step after the world's update (Debug mode off: turn everything off)
//
// Nothing here writes the save or the progress.

#include <cstdint>
#include <vector>

struct SW;
struct LO;
struct ALO;
struct SHDS;
struct GLOBBONEBIND;
using CID = int;

enum PCKWS
{
    PCKWS_None = 0,     // never asked for
    PCKWS_Loading = 1,  // being read (background) / waiting for the game thread's instantiation
    PCKWS_Ready = 2,
    PCKWS_Failed = 3,   // no file for the record, or its load failed (the running world is unchanged)
};

void RequestPcKeptWorld(int iwrec);
PCKWS PckwsKeptWorld(int iwrec);
bool FPcKeptWorldsLoading();   // any request not Ready / Failed yet
SW* PswKeptWorld(int iwrec);
LO* PloKeptFind(int iwrec, int oid, CID cidBase = 0);
const std::vector<std::vector<GLOBBONEBIND>>* PaabindKept(const ALO* palo);
SHDS* PshdsKept(int iwrec);

// A sound of a kept world (its sound id, in its own bank when that is not the running world's): for a puppet's
// animation events. The copies' own sounds (PloPcCloneIntoWorld) find their bank by themselves.
class CSnd;
CSnd* PsndNewPcKept(int iwrec, int sfxid);

// A live copy of a kept object (and its children) in the running world, started as a loaded object; fAdd: left in
// the world, else taken out (RemoveLo) like a proxy's spare. Null if the template is null.
LO* PloPcCloneIntoWorld(SW* psw, LO* ptmpl, bool fAdd);
// a copy's kept world (-1: not a copy). A copy's emitters draw with its kept world's shaders, and the objects their
// options name are found among its own copies, then in its kept world near its template (emitter.h's hooks)
int IwrecOfPcCopy(const LO* plo);

// The game thread's part: a kept world whose file has been read is instantiated (between two steps; the debug menu's
// step calls it). Also loads synchronously what is asked for while no world runs (inside a transition).
void PumpPcKeptWorlds(SW* psw);

struct PCWORLDHOOKS
{
    void (*pfnLoaded)(SW* psw) = nullptr;
    void (*pfnStarted)(SW* psw) = nullptr;
    void (*pfnStep)(SW* psw, bool fDebugKeys) = nullptr;
};
void RegisterPcWorldHooks(const PCWORLDHOOKS& hooks, const char* pszFile = __builtin_FILE());   // (static initializers: any order;
                                                                                              // called in the source files' name order)

// transition.cpp / debugmenu.cpp
void OnPcWorldLoaded(SW* psw);
void OnPcWorldStarted(SW* psw);
void OnPcWorldStep(SW* psw, bool fDebugKeys);
