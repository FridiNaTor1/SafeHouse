#pragma once
// The editor's names for Sly 2's numbers (the executable has none): the classes cls.cpp leaves unnamed and ~150 options
// (EOPIDs) from the port's code comments. Sly 2's editor uses them as they are; Sly 3's maps its own numbers to Sly 2's
// (game/gen/cidmap.inc / eopidmap.inc) and falls back on these after its own names (Sly3-Release/editor_data3.cpp).

#include "editor_objtypes.h"   // (GAMEAREA)

namespace edimpl
{

// null: not named here
inline const char* SzClassName2(int cid)
{
    switch (cid)
    {
    case 0x04: return "LIGHT";
    case 0x09: return "JUMPTARGET";
    case 0x0B: return "PIPE";
    case 0x0C: return "LEDGE";
    case 0x0D: return "RAIL";
    case 0x0E: return "NAVPOLY";
    case 0x10: return "DETECTOR";
    case 0x13: return "GRABTARGET";
    case 0x18: return "VOLTRIGGER";
    case 0x1A: return "EXIT";
    case 0x1B: return "CRAWLSPACE";   // (interact.cpp: the circle there starts the crawl, kind 8)
    case 0x1C: return "EMITTERFX";
    case 0x1E: return "BREAKABLEBASE";
    case 0x23: return "USE";
    case 0x24: return "USE2";
    case 0x25: return "BOOTY";
    case 0x28: return "JUMPPOINT";
    case 0x29: return "WARPPOINT";
    case 0x2A: return "CHECKPOINT";
    case 0x2C: return "CAMTARGET";
    case 0x45: return "TURRET";
    case 0x4E: return "VAULT";
    case 0x4F: return "PROJECTILE";
    case 0x53: return "PAD";
    case 0x55: return "LASER";
    case 0x56: return "BARRIER";
    case 0x58: return "HIDESPOT";
    case 0x59: return "WATER";
    case 0x5A: return "PICKUP";
    case 0x5B: return "COIN";
    case 0x5C: return "PRIZE";
    case 0x60: return "BIRD";
    case 0x62: return "BREAKABLE";
    case 0x6A: return "ROPE";
    case 0x6B: return "JOBMARKER";
    case 0x71: return "CMDLIST";
    case 0x72: return "SCENE";
    case 0x73: return "SPACEWARP";
    case 0x77: return "SPAWNER";
    default: return nullptr;
    }
}

inline const char* SzOptionName2(int eopid)
{
    switch (eopid)
    {
    case 8: return "instance binding";
    case 13: return "activation radius";
    case 14: return "activation kind";
    case 15: return "oid";
    case 25: return "add at load";
    case 26: return "remove at start";
    case 29: return "velocity";
    case 30: return "angular velocity";
    case 32: return "own mask";
    case 58: return "fade rate";
    case 75: return "velocity (anim)";
    case 76: return "angular velocity (anim)";
    case 107: return "animation params";
    case 114: return "animation end time";
    case 140: return "dialogue time";
    case 146: return "push music";
    case 151: return "light kind (0 sun, 1 point)";
    case 152: return "light colour (HSV)";
    case 153: return "light strength";
    case 154: return "light angle";
    case 155: return "light falloff near / far";
    case 156: return "casts shadow direction";
    case 157: return "not a context light";
    case 158: return "not baked";
    case 161: return "camera: mode";
    case 162: return "camera: distance";
    case 163: return "camera: height";
    case 164: return "camera: pitch";
    case 165: return "camera: field of view";
    case 166: return "camera: look ahead";
    case 167: return "camera: speed";
    case 168: return "camera: limits";
    case 169: return "camera: target offset";
    case 170: return "camera: collision";
    case 171: return "camera: zone";
    case 172: return "camera: blend";
    case 173: return "camera: flags";
    case 232: return "guard: Carmelita immunity";
    case 233: return "guard: attack action";
    case 235: return "guard attack: range min";
    case 236: return "guard attack: range max";
    case 237: return "guard attack: angle";
    case 238: return "guard attack: damage";
    case 239: return "guard attack: delay";
    case 240: return "guard attack: speed";
    case 241: return "guard attack: kind";
    case 242: return "guard attack: flags";
    case 243: case 244: case 245: case 246: case 247: case 248: return "guard: kind entry";
    case 255: return "guard: health";
    case 260: return "guard: target";
    case 273: return "guard: patrol (command list)";
    case 278: return "guard: state mask";
    case 312: return "detector: flags";
    case 313: return "detector: always sees";
    case 314: return "detector: never sees";
    case 315: case 316: return "guard: alert setter";
    case 341: return "guard: behaviour word";
    case 366: return "shape: point inside";
    case 394: return "projectile: owner";
    case 415: case 416: return "pad: state";
    case 430: case 431: return "volume: state";
    case 432: return "trigger: objects";
    case 433: return "trigger: classes";
    case 434: return "trigger: recheck time";
    case 436: case 437: case 438: return "trigger: fields";
    case 441: return "trigger: script";
    case 442: return "trigger: state";
    case 444: return "trigger: tracked members";
    case 447: return "trigger: first object inside";
    case 455: return "laser: path / ends";
    case 459: return "barrier: field";
    case 642: return "scene: set task state";
    case 758: return "unseen by detectors";
    case 769: return "cane reach";
    case 770: return "hypnosis request";
    case 778: return "warp point";
    case 779: return "checkpoint";
    case 786: return "exit: use";
    case 788: return "exit: task states";
    case 897: return "pickup: value";
    case 947: case 948: case 949: case 950: case 951: case 952: case 953: case 954: case 955: case 956: case 957: case 958:
    case 959: case 960: case 961: return "space warp: entry";
    case 964: return "space warp: current entry";
    case 997: return "turret: target";
    case 998: return "turret: follow";
    case 999: return "turret: aim";
    case 1000: return "turret: pool";
    case 1001: return "turret: launch";
    case 1002: return "turret: pan";
    case 1003: return "turret: tilt";
    case 1004: return "turret: mount";
    case 1005: return "turret: smooth";
    case 1055: return "camera target";
    case 1072: return "use: players";
    case 1073: return "use: classes";
    case 1074: return "use: flags";
    case 1076: return "use: task states";
    case 1155: case 1161: case 1162: case 1163: return "booty record";
    case 1182: return "crawl space: floor point";
    case 805: case 815: case 825: case 837: case 849: case 857: case 869: case 1024: case 1084: case 1190: return "players' mask (1 Sly, 2 Bentley, 4 Murray)";
    case 308: return "pocket: coins";
    case 309: case 310: return "pocket: prize";
    case 906: return "prize: its task";
    case 1225: return "vault: combination";
    case 1204: return "photo: completed";
    case 1208: case 1209: return "rope: end";
    case 1210: return "rope: length";
    case 1211: return "rope: force";
    case 1212: return "rope: mode";
    case 1236: return "job: closing events";
    case 1237: return "job: end events";
    case 1243: return "job: first task";
    case 1244: return "job: last task";
    case 1245: return "job: name (text id)";
    case 1246: return "job: description (text id)";
    case 1251: return "task/job: mode";
    case 1252: return "task/job: context";
    case 1253: return "task/job: state";
    case 1254: return "task/job: played";
    case 1258: return "job: complete sprite";
    case 1259: return "job: exit at the end";
    case 1260: case 1261: case 1262: case 1263: return "job: save progress";
    case 1264: return "task: job";
    case 1265: return "task: prompt (text id)";
    case 1267: case 1268: case 1269: case 1270: return "task: state fields";
    case 1271: return "task: flags";
    case 1272: return "task: auto-activate";
    case 1274: return "task: start checkpoint";
    case 1275: return "task: world";
    case 1276: return "task: player (7 Sly, 8 Bentley, 9 Murray)";
    case 1278: return "task: exit";
    case 1279: return "task: marker";
    case 1282: case 1283: case 1284: case 1286: return "task: progress";
    case 1285: return "task: time limit";
    case 1287: return "task: object changes";
    case 1288: return "task: state machine changes";
    case 1295: case 1296: case 1297: case 1298: case 1299: return "task: save progress";
    case 1354: return "dance: state";
    case 1355: return "dance: score";
    case 1447: return "nav link";
    default: return nullptr;
    }
}


// A Sly 2 class's gameplay area (fGuard: derived from the guards' 0x38)
inline GAMEAREA AreaOfCid2(int cid, bool fGuard)
{
    if (cid == 0x84 || cid == 0x85 || cid == 0x6B || cid == 0x72) return GA_Missions;
    if ((cid >= 0x09 && cid <= 0x0D) || cid == 0x13 || cid == 0x1B || cid == 0x28 || cid == 0x6A) return GA_Traversal;
    if (fGuard || cid == 0x77 || cid == 0x71) return GA_Guards;
    if (cid == 0x05 || cid == 0x18 || cid == 0x53 || cid == 0x23 || cid == 0x24 || cid == 0x16) return GA_Triggers;
    if (cid == 0x55 || cid == 0x10 || cid == 0x45 || cid == 0x56) return GA_Lasers;
    if (cid == 0x1A || cid == 0x2A || cid == 0x29 || cid == 0x73) return GA_Warps;
    if ((cid >= 0x5A && cid <= 0x5C) || cid == 0x25 || cid == 0x4E || cid == 0x58) return GA_Pickups;
    if (cid == 0x87 || cid == 0x0F || cid == 0x2C) return GA_Cameras;
    if (cid == 0x04) return GA_Lights;
    return GA_None;
}

}  // namespace edimpl
