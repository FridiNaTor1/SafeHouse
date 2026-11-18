#pragma once
// The Scripts tab's diagnostics state (Sly 2: editor_scriptdiag.inc; the shell's scriptdiag / scriptfix verbs).
#include "splicediag.h"

#include <map>
#include <set>
#include <string>
#include <vector>

struct SW;

namespace edimpl
{
struct SCRIPTDIAG
{
    std::vector<SPLICEISSUE> a;
    std::string textDone;          // the text they are of
    std::string textSeen;
    double tEdit = 0.0;
    int igroupDone = -2;
    int iselected = -1;
    int lineJump = 0;              // the text scrolled to this line next frame
    float hPanel = 0.0f;           // the problems list's height this frame
    // the world's facts (made again for a new world)
    const SW* psw = nullptr;
    std::map<int, int> mpoidcid;   // (-2: objects of several classes)
    std::set<int> setoidAll;
};
}  // namespace edimpl
