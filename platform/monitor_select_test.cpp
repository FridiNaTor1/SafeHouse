// The Monitor option's identifiers, matching and fallback (platform/monitor_select.h), without a display.
#include "monitor_select.h"

#include <cstdio>
#include <string>

using cane::monitor::Info;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}
Info mon(const char* name, int x, int y, int w, int h, int hz, bool primary = false) {
    Info m;
    m.name = name, m.x = x, m.y = y, m.width = w, m.height = h, m.refreshRate = hz, m.maxRefreshRate = hz, m.primary = primary;
    return m;
}
}  // namespace

int main() {
    namespace mo = cane::monitor;
    // two different monitors, the second primary
    const std::vector<Info> two{ mon("DELL U2720Q", 0, 0, 3840, 2160, 60), mon("LG 27GL850", 3840, 0, 2560, 1440, 144, true) };
    check(mo::idOf(two, 0) == "DELL U2720Q | 1 | 0,0", "identifier: " + mo::idOf(two, 0));
    check(mo::idOf(two, 1) == "LG 27GL850 | 1 | 3840,0", "identifier of the second");
    check(mo::idOf(two, 5).empty() && mo::idOf(two, -1).empty(), "out of range: Primary");
    check(mo::find(two, "") == -1 && mo::resolve(two, "") == 1, "Primary: the primary monitor");
    check(mo::find(two, "Primary") == -1 && mo::find(two, " primary ") == -1, "\"Primary\" spelled out");
    check(mo::find(two, mo::idOf(two, 0)) == 0 && mo::find(two, mo::idOf(two, 1)) == 1, "round trip");
    check(mo::find(two, "LG 27GL850") == 1, "a name alone");
    check(mo::find(two, "2") == 1 && mo::find(two, "3") == -1, "a plain number (1-based)");
    // the monitors rearranged (positions changed): still found by name
    const std::vector<Info> moved{ mon("LG 27GL850", 0, 0, 2560, 1440, 144, true), mon("DELL U2720Q", 2560, 0, 3840, 2160, 60) };
    check(mo::find(moved, "DELL U2720Q | 1 | 0,0") == 1, "found by name after a rearrangement");
    // unplugged: the primary one, shown as not connected, kept as a choice
    const std::vector<Info> one{ mon("LG 27GL850", 0, 0, 2560, 1440, 144, true) };
    check(mo::find(one, "DELL U2720Q | 1 | 0,0") == -1 && mo::resolve(one, "DELL U2720Q | 1 | 0,0") == 0, "unplugged: the primary");
    check(mo::missing(one, "DELL U2720Q | 1 | 0,0") && !mo::missing(one, "") && !mo::missing(one, mo::idOf(one, 0)), "missing()");
    check(mo::storedName("DELL U2720Q | 1 | 0,0") == "DELL U2720Q" && mo::storedName("").empty(), "storedName()");
    {
        const auto c = mo::choices(one, "DELL U2720Q | 1 | 0,0");
        check(c.size() == 3 && c[0].empty() && c[2] == "DELL U2720Q | 1 | 0,0", "choices keep a missing monitor");
        check(mo::choiceIndex(one, "DELL U2720Q | 1 | 0,0") == 2, "the missing one's index");
        check(mo::step(one, "DELL U2720Q | 1 | 0,0", 1).empty(), "after the missing one: Primary");
        // once another is chosen the missing one is gone from the list
        check(mo::choices(one, "").size() == 2, "no missing entry once another is chosen");
    }
    // no monitor at all (headless)
    check(mo::resolve({}, "") == -1 && mo::resolve({}, "X | 1 | 0,0") == -1, "no monitors");
    check(mo::choices({}, "").size() == 1 && mo::step({}, "", 1).empty(), "no monitors: only Primary");
    // stepping
    check(mo::step(two, "", 1) == mo::idOf(two, 0) && mo::step(two, mo::idOf(two, 0), 1) == mo::idOf(two, 1), "step forward");
    check(mo::step(two, mo::idOf(two, 1), 1).empty() && mo::step(two, "", -1) == mo::idOf(two, 1), "step wraps");
    // two monitors of the same model (Windows' "Generic PnP Monitor"), and a third
    const std::vector<Info> same{ mon("Generic PnP Monitor", 0, 0, 1920, 1080, 60, true), mon("Generic PnP Monitor", 1920, 0, 1920, 1080, 75),
                                  mon("Other", -1280, 0, 1280, 1024, 60) };
    check(mo::idOf(same, 1) == "Generic PnP Monitor | 2 | 1920,0", "the second of a name");
    check(mo::find(same, mo::idOf(same, 0)) == 0 && mo::find(same, mo::idOf(same, 1)) == 1, "same names told apart");
    check(mo::shortLabel(same, 1) == "Generic PnP Monitor (2)" && mo::shortLabel(same, 2) == "Other", "labels number same names");
    check(mo::label(same, 1) == "Generic PnP Monitor (2) - 1920x1080, 75 Hz", "label with the mode: " + mo::label(same, 1));
    // same-named, the order changed (the system listed them the other way round): the position wins
    const std::vector<Info> swapped{ mon("Generic PnP Monitor", 1920, 0, 1920, 1080, 75), mon("Generic PnP Monitor", 0, 0, 1920, 1080, 60, true) };
    check(mo::find(swapped, "Generic PnP Monitor | 2 | 1920,0") == 0, "same names: the position wins over the order");
    // same-named, both moved: the n-th
    const std::vector<Info> bothMoved{ mon("Generic PnP Monitor", 0, 100, 1920, 1080, 60, true), mon("Generic PnP Monitor", 1920, 100, 1920, 1080, 75) };
    check(mo::find(bothMoved, "Generic PnP Monitor | 2 | 1920,0") == 1, "same names moved: the n-th");
    // one of the two unplugged: the remaining one with that name
    const std::vector<Info> oneLeft{ mon("Generic PnP Monitor", 0, 0, 1920, 1080, 60, true) };
    check(mo::find(oneLeft, "Generic PnP Monitor | 2 | 1920,0") == 0, "the second gone: the first of that name");
    // Wayland: every output at 0,0; names differ (connector + model) or not
    const std::vector<Info> wayland{ mon("eDP-1", 0, 0, 1280, 800, 60, true), mon("HEADLESS-2", 0, 0, 1920, 1080, 60) };
    check(mo::find(wayland, mo::idOf(wayland, 1)) == 1, "Wayland: by name, positions all 0,0");
    const std::vector<Info> waylandSame{ mon("X", 0, 0, 1920, 1080, 60, true), mon("X", 0, 0, 1920, 1080, 60) };
    check(mo::find(waylandSame, mo::idOf(waylandSame, 1)) == 1, "Wayland, same names at 0,0: the n-th");
    // names with the separator in them
    const std::vector<Info> odd{ mon("A | B", 10, 20, 800, 600, 60, true) };
    check(mo::idOf(odd, 0) == "A | B | 1 | 10,20" && mo::find(odd, mo::idOf(odd, 0)) == 0, "a name with '|'");
    // an empty name
    const std::vector<Info> unnamed{ mon("", 0, 0, 800, 600, 60, true), mon("", 800, 0, 800, 600, 60) };
    check(mo::idOf(unnamed, 1) == "Monitor | 2 | 800,0" && mo::find(unnamed, mo::idOf(unnamed, 1)) == 1, "unnamed monitors");
    // the fixed field (SystemSettings::monitor)
    {
        char field[16];
        mo::store(field, "short");
        check(mo::load(field) == "short", "store / load");
        mo::store(field, "0123456789abcdefghij");
        check(mo::load(field) == "0123456789abcde", "store cuts at the field's size");
        mo::store(field, "0123456789abcd\xC3\xA9");   // (é across the end: not cut inside)
        check(mo::load(field) == "0123456789abcd", "store never cuts a UTF-8 letter");
    }
    if (failures == 0) std::printf("monitor-select: all passed\n");
    return failures == 0 ? 0 : 1;
}
