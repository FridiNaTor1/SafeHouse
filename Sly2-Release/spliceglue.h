#pragma once

// The object model's side of the splice runtime: the object hooks of spliceobj.h (find-object(s), the option
// primitives addo / ensureo / seto! / geto, the global objects) on the world being loaded / run.

struct SW;

void InstallSpliceHooks(SW* psw);

struct LO;
// orig: 0x0015E938: kind 4 resolves in the instance namespace; kind 1 searches below context.
LO* PloFindRelative(LO* context, int oid, int kind = 4);
