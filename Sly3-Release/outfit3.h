#pragma once

// Sly 3's outfits (the disguises): the class-0x96 object (outfit3.cpp; s3-decomp).

struct LO;
struct SM;
struct GAIT;

void ResetOutfits3();
bool FTakeOutfitSceneRebuild3();             // an outfit's meshes were streamed: the scene to rebuild (game.cpp)
int IdOutfitWorn3(LO* ploPlayer);             // the worn outfit's id (0x759..0x75B the disguises), -1 none
bool FWearOutfit3(LO* ploPlayer, int id);     // wear the outfit (id -1: none); false when the player has no outfits / no such one
bool FOutfitChanging3(LO* ploPlayer);          // the change is under way (+0x48 != -1: SCPT 0x6A waits for it)
void OnOutfitEvent3(LO* ploPlayer, int n);     // the player's animation event (0x757: the change's moment, 0x173308)
void UpdateOutfit3(LO* ploPlayer);
// orig3 0x152720's 0x172D10: the game's player entry's outfit (stored by 0x172E60) worn again, once per world (also
// done by UpdateOutfit3's first call); RestoreOutfits3: every player of the world (the world start)
struct SW;
void RestoreOutfit3(LO* ploPlayer);
void RestoreOutfits3(SW* psw);
int IdOutfitEntry3(LO* ploPlayer);             // the game's player entry's outfit (-1 none)             // each frame: the change's end
SM* PsmOutfitIdle3(LO* ploPlayer);             // the worn outfit's idle SM (0x1733F8), null: the player's own
GAIT* PgaitOutfit3(LO* ploPlayer, int sfx);
// orig3 0x156D48: the player wears an outfit (+0x1108: a record worn, 0x173428) that is a disguise in this episode (the
// record's episode, 0x172CF8, from the table 0x321138: 0x759 Venice, 0x75A China, 0x75B the pirates; -1 any)
bool FPlayerDisguised3(LO* ploPlayer);    // its gait (0x173408), null: the player's own
// orig3 0x16F2B0: group id of a class-0x97 object (ploOwner; null: the first of the world's that has it) streamed in,
// the one it had before unloaded (false: none)
bool FStreamGroup97(int id, LO* ploOwner = nullptr);
// The shader oid of the i-th kind-2 item of a streamed group (the group's shaders bound to its rows: dvd_menu's list
// pictures, 165160.. for the Two Player rows); -1 none / the group not streamed
int OidStreamedShader97(int id, int i);
