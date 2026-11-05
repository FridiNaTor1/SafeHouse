#pragma once

// Load trace: which loader function read which bytes of a world file, in the format of the reference's parse
// maps (Sly2-Reference/Sly2-Release/tools/loadmap.py: "start end 0xFUNC name readers"), so a native load can be diffed with
// the original's.
//
// Every loader function opens a LOADFN(0xADDR) scope (its original address); every CBinaryInputStream reader
// reports (kind, offset) to the innermost scope, like the reference's trace attributes a read to the function
// that called the stream's reader.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace loadtrace
{
extern bool g_fOn;
extern std::vector<uint32_t> g_afnStack;

void Read(const char* kind, std::size_t ib);
// Starts a new file (a stream whose reads start at offset 0).
void Begin();
// The end offset of the stream (the last run ends there).
void End(std::size_t ib);
bool Write(const std::string& path);
}

struct LoadScope
{
    explicit LoadScope(uint32_t fn) { loadtrace::g_afnStack.push_back(fn); }
    ~LoadScope() { loadtrace::g_afnStack.pop_back(); }
    LoadScope(const LoadScope&) = delete;
    LoadScope& operator=(const LoadScope&) = delete;
};

#define LOADFN_CAT2(a, b) a##b
#define LOADFN_CAT(a, b) LOADFN_CAT2(a, b)
#define LOADFN(addr) LoadScope LOADFN_CAT(loadscope_, __LINE__)(addr)
