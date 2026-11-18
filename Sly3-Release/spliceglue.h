#pragma once

// The object model's side of the splice runtime: the object hooks of spliceobj.h (find-object(s), the option
// primitives addo / ensureo / seto! / geto, the global objects) on the world being loaded / run.

struct SW;

void InstallSpliceHooks(SW* psw);

struct LO;
// orig: 0x0015E938: kind 4 resolves in the instance namespace; kind 1 searches below context.
LO* PloFindRelative(LO* context, int oid, int kind = 4);

// (Sly 3) an SW field option the engine reads (988 light falloff c/l/q, 989 its min/max, 990 the low flight band, 1007
// the ceiling +0x4FF0): component i as a script set it, else the world file's value, else gDefault
float GSwOptionField3(int optid3, int i, float gDefault);
