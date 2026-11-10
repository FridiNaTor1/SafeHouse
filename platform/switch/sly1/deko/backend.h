#pragma once
#include <glad/glad.h>

namespace CaneDeko
{
bool Initialize(unsigned width, unsigned height);
void ResizeOutput(unsigned width, unsigned height);
void Shutdown();
void InstallEntryPoints();
// A GL entry point by name for glad's loader, nullptr if not implemented.
void* ProcAddress(const char* name);
// Override the next presentation's telemetry with its scene render size.
// Frames without an override (installer, movies) use the output size.
void SetFrameResolution(unsigned width, unsigned height);
// With the alpha test off (default on) and early depth on (default off), programs with a NOKILL fragment shader use it.
void SetAlphaTest(bool enabled);
void SetEarlyDepth(bool enabled);
void Present();
void SetSwapInterval(int interval);
GLuint CreateProgram(const char* vertex, const char* geometry, const char* fragment);
}
