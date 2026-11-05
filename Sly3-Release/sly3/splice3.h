#pragma once

// The splice runtime's self-test over Sly 3's worlds (splice3.cpp).

#include <string>

// W: a world index (0..39), its name, or "all": every module of every group deserialized by the runtime and evaluated
int SpliceWorld3Command(const std::string& arg, bool fVerbose);
