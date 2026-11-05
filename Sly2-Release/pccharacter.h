#pragma once

// PC addition, not in the original game (debug menu only): guest characters and
// outfits that the retail game has only in one world, used in any world.
//
//   - Carmelita as the player, in her three models: the Prologue's (Cairo museum, world record 0), the snow one
//     (Episode 6's train station, record 27) and the tango dress (Episode 2's ballroom, record 11).
//   - Sly's tango outfit (the ballroom's Sly model) on the current Sly, his moves unchanged.
//
// How: the model's world file is loaded as a kept world (pcloader.h: read on a thread and instantiated between two
// steps while the current world keeps running; no reload, no fade). Then:
//   - Carmelita is a puppet: her guard object and its parts in the kept world, drawn in the current world's scene,
//     placed on the current player every step and posed by her own animations (ASEGs) chosen from the player's
//     motion (idle / walk / run / jump / fall). The player object keeps doing everything (moves, collisions,
//     camera, gadgets); it is only hidden. Square plays her shock pistol shot.
//   - Sly's tango outfit: the ballroom Sly's meshes (the main object's and its parts' glob sets) put on the current
//     Sly, with the ballroom's bone bindings re-pointed at the current Sly's bones (the two skeletons are the same).
// Nothing here writes the save or the progress. Turning it off restores the player's drawing and Sly's own meshes.
//
// The table of characters / outfits (PCCHARDEF) is data: a model's world record and object id, its animations.

#include <vector>

#include <glm/glm.hpp>

struct SW;
struct ALO;
struct LO;

enum PCCHARK
{
    PCCHARK_Puppet = 0,     // a character drawn on the player (Carmelita)
    PCCHARK_SlyOutfit = 1,  // meshes put on the current Sly
    PCCHARK_Guard = 2,      // a playable guard (pcguard.cpp's controller; the menu's Guards > Episode n page)
    PCCHARK_Boss = 3,       // a playable boss (pcguard.cpp's controller; the menu's Bosses page)
};

struct PCCHARDEF
{
    const char* pchzName;   // the menu's label
    PCCHARK kind;
    int iwrec;              // the world record whose file has the model
    int oid;                // the model's object there (Carmelita 12712 (cid 0x3F), Sly 7 (cid 0x35))
    // puppets: the animations (ASEG oids around the object; -1 none): idle, walk, run, jump (up), fall, land, attack
    int oidIdle, oidWalk, oidRun, oidJump, oidFall, oidLand, oidAttack;
    // her moveset (-1 none): the high jump's animation, the turn in place's (left, right), her shot's projectile (its template in her world's file), the
    // launcher that throws it, the shot's time in the attack animation and its muzzle point (forward, right, up of her
    // feet: where her hand holds the pistol then)
    int oidHigh, oidTurnL, oidTurnR, oidProj, oidLaunch;
    float tShot;
    float aposMuzzle[3];
    float sWalk, sRun;      // puppets: the speeds (units / s) the walk and run cycles were made for
    const int* an0Hide;     // mesh groups (GLOB::n0) not drawn, -1 ends (puppets: effects her AI shows; outfits: the ones
                            // the outfit replaces)
    const int* aoidHide;    // puppets: parts (object oids, with their children) not drawn, -1 ends
    const int* an0Show;     // outfits: the outfit's mesh groups, shown, -1 ends
    int iep = -1;           // guards / bosses: the episode (0 .. 8) whose menu page lists it (Guards > Episode n)
    const void* pvExtra = nullptr;  // guards / bosses: the controller's own data for the row (pcguard.cpp), if any
    // Puppets with their moveset on Sly: the guest's own collision size and camera. 0: computed from its body's
    // collision entry against Sly's standing shape; else the
    // value used as it is (a per-row override).
    float kCollXY = 0.0f;   // Sly's collision shapes scaled across (forward / sideways) by this
    float kCollZ = 0.0f;    // and up by this (about the shapes' bottom: the feet stay on the floor)
    float uCamDist = 0.0f;  // the follow camera's distance (the zone's) times this
    float sCamHeight = 0.0f;  // the camera's focus height above the floor (the actor's +0x1048; Sly's 125)
};

// The guest's fit as used (pccharacter.cpp): its collision scales and camera values, and the guest's body box it was
// computed from (its width across, its height; 0: none found)
struct PCCHARFIT
{
    float kCollXY = 1.0f, kCollZ = 1.0f, uCamDist = 1.0f, sCamHeight = 125.0f;
    float sBodyW = 0.0f, sBodyH = 0.0f;
};
// the current puppet's fit (computed when it is chosen); applied: it is on Sly now
const PCCHARFIT& PcCharFit(bool* pfApplied);

// The menu's rows for the guard / boss kinds are chosen through the controller registered for the kind (pcguard.cpp,
// a static initializer). No controller: the rows are shown dimmed. pfnRequest(psw, -1) turns the kind's choice off;
// pfnCurrent gives the chosen row (-1 none); pfnStatus (may be null) a short text for the row's value ("loading").
struct PCCHARCTRL
{
    void (*pfnRequest)(SW* psw, int idef) = nullptr;
    int (*pfnCurrent)() = nullptr;
    const char* (*pfnStatus)(int idef) = nullptr;
};
void RegisterPcCharController(PCCHARK kind, const PCCHARCTRL& ctrl);
const PCCHARCTRL* PPcCharController(PCCHARK kind);

int CPcCharDefs();
const PCCHARDEF& PcCharDef(int idef);

// The current choices (-1: none / Sly's own outfit) and the ones waiting for their world file's load.
int IdefPcCharPuppet();
int IdefPcCharSlyOutfit();
bool FPcCharPending();
bool FPcCharLoaded(int idef);   // its world's file is kept (the choice applies at once)

// The debug menu's actions: a puppet (-1: off) / Sly's outfit (-1: his own). A model whose world is not kept yet has it
// loaded while the current world runs (pcloader.h: no reload); the choice applies when it is ready.
void RequestPcCharPuppet(SW* psw, int idef);
void RequestPcCharSlyOutfit(SW* psw, int idef);

// pcloader.h's world hooks (registered by pccharacter.cpp): the choices applied to the new world (after its start).
void PcCharOnWorldStarted(SW* psw);
// a choice waiting for its kept world's load (-1 none)
int IdefPcCharPuppetWanted();
int IdefPcCharSlyOutfitWanted();

// debugmenu.cpp UpdateDebugTools (each step after the world's update, Debug keys on): the puppet placed and posed.
// With Debug keys off everything is turned off.
void UpdatePcChar(SW* psw, bool fDebugKeys);

// Carmelita's moveset (debug menu "Carmelita's moves", on by default): with her puppet chosen and Sly the current
// player, Sly's object is driven by her restricted set of moves: walk, run (R1),
// jump and double jump, Circle her high jump (or the interaction in reach), Square her shock pistol, Triangle held her aim
// mode (strafe, crosshair); no ledge grabs,
// ropes, poles, hooks, spires, pickpocketing, cane moves or gadgets. All false / no-ops otherwise.
bool FPcCarmMovesetOn();
void SetPcCarmMoveset(bool f);
bool FPcCarm(const LO* plo);                        // plo is the player whose moves are hers
void PcCarmJumped(ALO* palo, bool fHigh, bool fDouble = false);  // sly.cpp / jump.cpp: after its jump's start (kind 1,
                                                    // her double jump kind 6): her jump's height
bool FPcCarmDoubleJump(const LO* plo);              // jump.cpp: Cross in the air is her double jump (Carmelita, not the
                                                    // guards)
// Her aim mode (Triangle held, Sly 3's strafe): she faces where the camera looks and strafes by the stick, a crosshair
// (locked on the guard nearest to it) shows where Square shoots. sly.cpp UpdateSlyInput calls PcCarmAimInput every step
// with Triangle's state and the player's state; true: aiming this step (then PcCarmAimHeading is her heading and
// PcCarmStrafeVel her velocity in her heading's frame, by the stick's deflection uJoy and direction radJoy, R1 faster).
bool PcCarmAimInput(ALO* palo, bool fTriangle, int state);
bool FPcCarmAiming();
float PcCarmAimHeading();
void PcCarmFaceAim(ALO* palo);                      // her aim mode: she faces PcCarmAimHeading (turned now)
glm::vec3 PcCarmStrafeVel(float uJoy, float radJoy, bool fR1);
// debugmenu.cpp DrawDebugOverlay (in its 2D pass): the crosshair while she aims
bool FPcCarmCrosshair();
void DrawPcCarmCrosshair();
bool FPcCarmShooting();                             // her shot's animation: no ground moves meanwhile
void PcCarmFire(ALO* palo);                         // sly.cpp: Square
float PcCarmMoveScale(float sGait, float uJoy, bool fR1);  // sly.cpp state 12: Sly's gait velocity (its speed sGait at
                                                    // the stick's deflection uJoy) times this: her walk by the stick
                                                    // (a slight push a slow walk), R1 her run (guards: their walk / run)
bool FPcCarmCanJump();                              // Cross jumps (guards that have no jump: nothing)
bool FPcGuardMoves();                               // the puppet is a guard / boss with its own moves (pcguard.cpp)
bool FPcCarmCircle(ALO* palo);                      // sly.cpp: Circle with nothing to use: true for her high jump
                                                    // (guards: their special, false)
bool FPcCarmShotHits(const LO* pproj, const LO* po);  // projectile.cpp: her shot does not hit her own player
// transition.cpp, after the world's load and before its start: her shot's projectiles (clones of the template in her
// world's file) made in the new world, so they start with it
void PcCharOnWorldLoaded(SW* psw);

// scenesw.cpp: the puppet's objects added to the scene; the draw override of an object (-1 none, 0 hidden (the
// player under a puppet), 1 drawn (the puppet)). game.cpp: the scene is built again (a mesh changed).
void AppendPcCharSceneAlos(std::vector<ALO*>& apalo);
int NPcCharDrawOverride(const ALO* palo);
bool FPcCharTakeSceneRebuild();

// The puppet, for pcguard.cpp (the guards' movesets drive it): its kept world, its root object, its animation by oid
// (played from its start; looped or held), the animation's time, the attack's time left (no ground moves meanwhile).
SW* PswPcPuppetKept();
ALO* PaloPcPuppetRoot();
LO* PloPcPuppetPlayer();
bool FPcPuppetPlay(int oid, bool fLoop);
float TPcPuppetAnim();
int OidPcPuppetAnim();
void SetPcPuppetAttack(float dt);
bool FShowPcPuppetPart(int oid, bool fOn, float dtFade);   // a drawn part of the puppet (its oid) added / removed (faded over dtFade); false: none
LO* PloPcPuppetNear(int oid);   // an object of the puppet's tree (else the first of the oid) in the kept world
