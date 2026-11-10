#pragma once

// (perf-4) The performance paths first written for the Switch (PR #29: the pass helpers' threads for the frame's loops
// over the scene, the prefetches along the records' pointer chains, buffers kept from frame to frame, the shadow phases'
// scissor rectangles, the sort keys, ...) on every platform: each gives the same results as the
// plain code it replaces (the frame dumps, the pictures and the merge gates are identical with and without them).
// What stays the Switch's own: its GL layer (deko3d), the CPU's loading profile, GL's own near clipping, newlib's fmodf
// stand-in, the shadow prisms' shared side walls (render.cpp: a hairline can differ), the z-pass casters (render.cpp:
// an extra dark quad in the Ep8 roof chase at its start), and the step's hierarchies on
// the helpers (game.cpp: a hierarchy reading outside itself sees this frame's roots, the serial walk last frame's).
// -DSLY2_FASTPATHS=0 builds the plain code; SLY2N_HELPERS=0..2 sets the helper threads (passhelpers.h).
#ifndef SLY2_FASTPATHS
#if defined(__GNUC__) || defined(__clang__)
#define SLY2_FASTPATHS 1
#else
#define SLY2_FASTPATHS 0   // (the prefetches are __builtin_prefetch)
#endif
#endif
