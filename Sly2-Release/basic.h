#pragma once

#include <unordered_set>

// BASIC: the root of the game's objects (the original's vtable holder). The splice runtime refers to objects as
// BASIC* (splicetypes.h); the object model's LO derives from it.
// (native lifetime: the live objects are kept in a set. On the PS2 an object's memory outlives it (a script's
// scheduled callback whose self was freed, e.g. world 22's 2755 at t 28.6, still reads it); natively such a
// pointer is checked with FBasicLive before use.)
inline std::unordered_set<const void*>& SetBasicLive()
{
    static auto* s_p = new std::unordered_set<const void*>;
    return *s_p;
}
struct BASIC
{
    BASIC() { SetBasicLive().insert(this); }
    BASIC(const BASIC&) { SetBasicLive().insert(this); }
    BASIC& operator=(const BASIC&) { return *this; }
    virtual ~BASIC() { SetBasicLive().erase(this); }
};
inline bool FBasicLive(const BASIC* p) { return p && SetBasicLive().count(p) != 0; }
