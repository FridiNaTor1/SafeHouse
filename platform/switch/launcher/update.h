#pragma once
// The launcher's update (launcher/update.h's, for the Switch): when the launcher starts, GitHub's API is asked for the
// latest release of FridiNaTor1/SafeHouse; a newer version than this build's with a SafeHouse.nro is offered. Its
// download is checked against the SHA-256 GitHub lists for it, then put in the place of the running SafeHouse.nro (the
// old one kept until the new one is in place); the next start is the new version. Over libnx's sockets and the
// system's TLS service with its own certificate authorities; nothing but a generic User-Agent is sent.
// Release builds (CANE_SWITCH_VERSION set) check for updates; development builds never do.
#include <cstdint>
#include <string>

namespace CaneSwitch::Update
{
enum class State
{
    Idle,          // nothing to offer: not checked, offline, up to date, or the check failed
    Checking,
    Available,     // a newer release with a SafeHouse.nro (version)
    Downloading,   // done / total bytes
    Installed,     // the next start of SafeHouse.nro is the new version
    Failed,        // the download or its installation failed (message); the running SafeHouse.nro is unchanged
};
struct Status
{
    State state = State::Idle;
    std::string version;   // the release's ("0.1.27")
    uint64_t done = 0, total = 0;
    std::string message;
};

// The check, on a thread. `self`: the running SafeHouse.nro (empty, started without the Homebrew loader: no check).
// A development build returns at once: no thread, no network.
void Check(const std::string& self);
Status Get();
// The release found (Available) downloaded and installed, on a thread.
void Install();
// The download stopped (back to Available).
void Cancel();
// The thread stopped and the network closed (before a game takes the process).
void Stop();
}  // namespace CaneSwitch::Update
