#pragma once

// Sly 3's boot: the action queue of orig3 0x1C6EA8 (StartBootSequence; Sly 2's 0x1A4E08) and its runner 0x1C6F30
// (Sly 2's 0x1A4E80), the transition request 0x289070 (its 0x48-byte request to 0x25AD70(GAME, req), or to the
// transition queue 0x288DE8(0x4C4090, ...) when a fifth argument is given), and the card check (kind 5).
//
// The queue (GAME-side, 0x1C8 bytes: +0x0 count, +0x4 0x1C-byte actions, +0x1C4 the cursor) at the boot:
//   kind 0 movie 338 (key 0x317EB8), kind 0 movie 339 (0x317EF8), kind 5 the card check (queued when
//   0x11C330() < 2: the build-mode table 0x341EE0 indexed by 0x36B124, release mode 0 -> 1: always),
//   kind 2 world 35 (m_ext, key 0x25E070(0x23)) at the point 0x1A8.
// The runner takes the next action: kinds 0 / 1 QueueMovie 0x1DBD08 (the movie manager 0x389AC0; kind 1 also sets
// its +0x108) then a transition to the empty key 0x345820 with 0x1A8 (the movie plays during it); kind 2 the
// transition 0x289070(GAME 0x475480, key, point, iworld, -1); kind 3 RequestTaskStartByOid(0x36C700, oid, n, 2);
// kind 4 back one action; kind 5 the card (0x3751D0 +0x58 == 0: 0x1D0D30(card, 1), the card's state machine
// started: the slots read, the create-save prompt on a new card), then the next action at once.
// The runner runs again when a movie's transition completes (the main loop's transition 0x25C2A8 lands).
//
// Native: BOOTHOST3 does the effects; Boot3 (main3.cpp) runs the queue with a host that plays the movies (a window,
// or decoded headless), reads the host save slots for the card check (<user-dir>/Saves/saveN, savedata.h), and
// hands the world transition to g_pfnBootTransition3 (the world / main loop side sets it; until then the boot
// reports that the world is not ported).

#include <cstdint>
#include <functional>
#include <vector>

// orig3: 0x289070's request (0x48 bytes: +0x00 iworld, +0x04 the 64-byte world key (GAME +0x43DC, the current
// world's, when none is given), +0x44 the arrival point)
struct TRANSREQ3
{
    int iworld = -1;
    uint32_t vaKey = 0;  // the key record's ELF address (0: the current world's)
    int point = 0;       // 0x1A8 at the boot (the title over m_ext)
};

struct BOOTACTION3
{
    int kind = 0;           // +0x00 (0..5)
    uint32_t vaMovieKey = 0;  // +0x04
    uint32_t vaWorldKey = 0;  // +0x08
    int point = 0;          // +0x0C
    int iworld = -1;        // +0x10
    int oidTask = -1;       // +0x14
    int nTask = 0;          // +0x18
};

struct BOOTQUEUE3
{
    std::vector<BOOTACTION3> a;  // +0x0 count, +0x4 the actions (16 at most)
    int icur = 0;                // +0x1C4
};

struct BOOTHOST3
{
    virtual ~BOOTHOST3() = default;
    virtual void QueueMovie(uint32_t vaKey, bool fKind1) { (void)vaKey; (void)fKind1; }  // 0x1DBD08
    virtual void RequestTransition(const TRANSREQ3& req) { (void)req; }                 // 0x289070 -> 0x25AD70
    virtual void RequestTaskStart(int oid, int n) { (void)oid; (void)n; }               // 0x189E70(0x36C700, oid, n, 2)
    virtual int CardState() { return 0; }                                               // 0x3751D0 +0x58
    virtual void StartCardCheck() {}                                                    // 0x1D0D30(card, 1)
};

// orig3: 0x1C6EA8 (the queue built, then run)
void StartBootSequence3(BOOTQUEUE3& q, BOOTHOST3& host);
// orig3: 0x1C6F30 (the next action; nothing when the queue is done). Returns false when the queue is done.
bool FRunBootQueue3(BOOTQUEUE3& q, BOOTHOST3& host);
// orig3: 0x1C70D8 / 0x1C7130 / 0x1C71B0 (the queue's builders: a movie key, a world, the card check)
void QueueBootMovie3(BOOTQUEUE3& q, uint32_t vaKey);
void QueueBootWorld3(BOOTQUEUE3& q, uint32_t vaKey, int point, int iworld);
void QueueBootCard3(BOOTQUEUE3& q);

// The world transition at the end of the boot (kind 2), set by the world / main-loop side: it loads the world and
// runs the game. Returns the process exit code. Unset: Boot3 prints that the world is not ported.
extern std::function<int(const TRANSREQ3&)> g_pfnBootTransition3;
