#pragma once

#include <memory>
#include <vector>

struct ACTOR;
struct AIJOB;
struct GUARD;

// The actor's owned stack (orig +0x26C: owner, count, eight job slots).
// Lifetime operations live beside AIJOB so actors need not include command-list definitions.
struct AISTACK
{
    AISTACK();
    ~AISTACK();
    GUARD* pguard = nullptr;
    std::vector<std::unique_ptr<AIJOB>> ajob;
};

void ClearJobs(ACTOR* pactor);
