#pragma once

// The thrown objects (class 0x4F and its variants: the guards' strikes, Bentley's darts): projectile.cpp.

struct ALO;
struct LO;

// orig 0x13BC60: the launcher that threw it (+0x200)
void SetProjectileLauncher(ALO* palo, LO* plaunch);
// (Bentley's darts, 0x1357C0) the flight ends within g (0x13B5C8); the contact kind the end sends (0: its +0x1EC)
void ExpireProjectileIn(ALO* palo, float g);
void SetProjectileFilter(ALO* palo, unsigned grf);
