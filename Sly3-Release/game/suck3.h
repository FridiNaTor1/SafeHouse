#pragma once

// Sly 3's dynamic difficulty ("the suck", orig3 0x171AA0..0x171E48 on the record D_0036BD20: its parameters +0x14 =
// {0, 1 the limits, +0.1 a death / a failed job, -0.2 a task first done}): splice built-in 107 reads it, e.g. the
// dogfights scale the planes' hit points by it (lerp 1 .. 0.625), the race / the director their scores.

class LO;

// orig3 0x171C98: 1 in world 22 (the two-player hack); 0 in a challenge of mode 0 or 2; else the current mission's
// float (its +0xAC: the progress value of its EOPID 1387), else the history's share of deaths (a save key's bits over
// 32: not kept natively, 0)
float GSuck3();
// orig3 0x171DD8: the mission's float set (limited to 0..1; not while a challenge runs)
void SetSuck3(float g);
// orig3 0x171AA0 (from the broadcast 0x1854C8): 0xC the player's death +0.1 (in a mission that survives it), 0x20 a
// job failed +0.1, 0x29 a task of the current job done the first time -0.2
void SuckOnEvent3(int code, LO* plo);
