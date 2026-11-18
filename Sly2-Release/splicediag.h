#pragma once

// Diagnostics of a group's splice scripts, as a compiler gives them (the map editor's Scripts tab: underlines and the
// problems list, on Check / Apply; the game's log when a mod's scripts load; --splice-diag): the converter's text and
// the readable one (splicenames.h). Each problem has its place (line / column), a severity, a short explanation and,
// where it is obvious, a quick fix:
//   errors   - the text does not read (a missing ')' ...); a name no table and no ";=" line maps (.option, $object,
//              ~event-word, %job-event, <built-in>); a symbol / function the module never defines; a built-in, a
//              function of the module or an object's method called with too few arguments; a call of a number /
//              string / flag; an option of a literal
//   warnings - too many arguments (the rest are not even evaluated); an option the object's class does not have; an
//              object that is not in the world; a define of an engine event's look-alike the engine never calls
//              ("on-strat"); set! of a name nothing defines (it sets nothing); unreachable code ((if #t a b)'s b, a
//              clause after else, a case value an earlier clause takes); an event handler's parameter the engine
//              never passes; arithmetic on a string / flag / object literal; a define nothing in its function uses
//   hints    - a module's own define nothing in the module uses
// The game's own scripts give no errors (the self-test checks every installed script).

#include <functional>
#include <set>
#include <string>
#include <vector>

enum SPLICESEV
{
    SPLICESEV_Error = 0,
    SPLICESEV_Warning = 1,
    SPLICESEV_Hint = 2,
};

struct SPLICEISSUE
{
    int line = 0, col = 0;          // where it starts (1-based; columns in bytes)
    int lineEnd = 0, colEnd = 0;    // where it ends (inclusive)
    int sev = SPLICESEV_Error;
    std::string code;               // "unknown-option", "arg-count" ...
    std::string message;            // the short explanation
    std::string fixLabel;           // a quick fix ("": none)
    std::string fixText;            // what replaces [line:col, lineEnd:colEnd]
    bool fFixLines = false;         // the fix removes the lines line..lineEnd (fixText unused)
    bool fFixAppend = false;        // the fix appends fixText at the text's end
};

struct SPLICEDIAGCTX
{
    // the world (null: not known, those checks skipped)
    std::function<bool(int oid)> pfnObjectExists;
    std::function<int(int oid)> pfnObjectCid;                     // -1: not known
    std::function<std::vector<int>(int imodule)> pfnModuleClasses;  // the classes of the objects running the module
    std::set<int> setoidKnown;      // oids the installed scripts look up (not reported)
    bool fEngineTables = true;      // the executable's option tables (the classes' options, the methods' counts)
};

void SpliceDiagnose(const std::string& text, const SPLICEDIAGCTX& ctx, std::vector<SPLICEISSUE>& adiag);
// "line 12:5: error: ... [code]" (+ " (quick fix: ...)")
std::string StrSpliceDiag(const SPLICEISSUE& d);
// a quick fix applied to the text (false: none / out of range)
bool FApplySpliceFix(const SPLICEISSUE& d, std::string& text);
// counts by severity
void CountSpliceDiags(const std::vector<SPLICEISSUE>& a, int& cerr, int& cwarn, int& chint);
// the classes whose option tables have the option (null: none, or the executable not open)
const std::set<int>* SpliceOptionClasses(int optid);
// The self-test: seeded errors in a small text found at their lines; prints failures, returns their count
int SpliceDiagSelfTest();
