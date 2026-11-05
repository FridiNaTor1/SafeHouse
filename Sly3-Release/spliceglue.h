#pragma once

// The object model's side of the splice runtime: the object hooks of spliceobj.h (find-object(s), the option
// primitives addo / ensureo / seto! / geto, the global objects) on the world being loaded / run.

struct SW;

void InstallSpliceHooks(SW* psw);
