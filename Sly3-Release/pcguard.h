#pragma once

// PC addition, not in the original game (debug menu only): the guards and
// bosses of every episode as playable characters.

#include <cstdio>

struct SW;

// Debug survey (main.cpp --load-world W with SLY2N_GUARDSURVEY=1): the world's guard templates, their animation
// tables, actions, projectiles.
void SurveyGuards(SW* psw, FILE* pf);

#include <vector>
#include <glm/vec3.hpp>
struct PCCHARDEF;
struct ALO;
struct LO;

// The table (pcguard.cpp): its entries appended to pccharacter.cpp's character table (kind puppet, iguard set)
void AppendPcGuardCharDefs(std::vector<PCCHARDEF>& adef);
// The model's first use (pccharacter.cpp FBeginPuppet): its animations, actions, launcher and projectile read from its
// own AI data in the kept world (the kind table, the action objects, their animations' events)
void PcGuardBegin(PCCHARDEF* pdef, SW* pswKept, ALO* proot);
float PcGuardRunScale(const PCCHARDEF& def, bool fR1);   // the player's run speed scale (its walk; R1: its run)
float PcGuardJumpHeight(const PCCHARDEF& def);           // 0: no jump
// Square (0) / Circle (1): its attack / special (the puppet's animation; hits and shots at their times)
void PcGuardAttack(ALO* paloPlayer, int ibtn);
// each step of the puppet with its moveset on (pccharacter.cpp StepPuppet): the attack's hits and shots
void UpdatePcGuardAttack(SW* psw, ALO* paloPlayer);
// after a world's load, before its start (pccharacter.cpp PcCharOnWorldLoaded): its projectiles cloned into the world
// (pdef null: none)
void PcGuardOnWorldLoaded(SW* psw, PCCHARDEF* pdef, SW* pswKept, ALO* proot);
bool FPcGuardProjReady(SW* psw, const PCCHARDEF& def);
bool FPcGuardProjectile(const LO* plo);
// a guard whose body is loaded hidden (the spawned ones: their instance's load shows it): its groups shown
void PcGuardFixGroups(ALO* proot);
// its attack's root motion (world velocity, horizontal; none: zero): sly.cpp, while the attack holds the player
glm::vec3 PcGuardAttackVel();
const char* PchzPcGuardMoves(int iguard);   // what its Square / Circle do (the menu's status line)
int IPcGuardOf(const PCCHARDEF& def);
// the puppet's animation events (pccharacter.cpp: all but its sounds and mesh groups): the guard's effects
struct ASEGEV;
void PcGuardPuppetEvent(const ASEGEV& ev);       // its index in pcguard.cpp's table (-1: not a guard / boss row)
