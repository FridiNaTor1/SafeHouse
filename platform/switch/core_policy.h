#pragma once
// The core of ProjectCane's helper threads (io_worker.cpp's settings writer, Sly 1's audio prefetch), from the
// runtime's plan (runtime.cpp): the game thread keeps core 0, file work runs on core 1.
namespace CaneThreads
{
int IoCore();
}
