#pragma once

// Sly 3's option-name ids (the splice's addo / ensureo / seto! / geto keys).
// - Ids up to OPTID_LAST are the classes' options (EopidFromOptid 0x249CF8 on the class's list 0x2ED518); above it they
//   are the object's own bindings (its sidebag). orig3 0x24AB90 / 0x24AC18: `slti a2, 0x718` (Sly 2: 0x4D1).
// - The handlers of the non-object targets copied from Sly 2 (spliceglue.cpp: the camera, the sound, the world, the
//   prompt, the controller state, ASEGAs, jobs) compare Sly 2 ids: Optid2FromOptid3 converts at their entry
//   (game/optidmap3.inc from maps/optids.tsv; -1 when the Sly 3 id has no Sly 2 counterpart).

namespace sly3
{
inline constexpr int OPTID_LAST = 0x717;

namespace gen
{
struct OPTIDROW { int optid2, optid3; };
inline constexpr OPTIDROW aoptid[] = {
#define OPTIDMAP(a, b) {a, b},
#include "optidmap3.inc"
#undef OPTIDMAP
};
}  // namespace gen

constexpr int Optid2FromOptid3(int optid3)
{
    if (optid3 > OPTID_LAST) return optid3;  // (a binding: its symbol id, unchanged)
    for (const gen::OPTIDROW& r : gen::aoptid)
        if (r.optid3 == optid3) return r.optid2;
    return -1;
}
constexpr int Optid3FromOptid2(int optid2)
{
    for (const gen::OPTIDROW& r : gen::aoptid)
        if (r.optid2 == optid2) return r.optid3;
    return -1;
}
}  // namespace sly3
