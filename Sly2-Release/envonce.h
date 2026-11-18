#pragma once

// A debug switch (SLY2N_*) read once per call site: getenv scans the whole environment, and the code that tests these
// runs every step, for many objects (about a tenth of a step's time in a cutscene's fast-forward, skipcut.h). The
// switches are set before the game starts; none changes while it runs.

#include <cstdlib>

#define SLY2N_ENV(name) ([]() -> const char* { static const char* const s_szEnv = std::getenv(name); return s_szEnv; }())
