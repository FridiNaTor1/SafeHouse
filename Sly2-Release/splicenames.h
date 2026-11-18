#pragma once

// Readable splice scripts (the map editor's script view, --dump-splice): the converter's text (worldparts.h
// FSpliceToText: symN, option numbers, oids, #bif:N) with names for the symbols (the engine's events by role, the
// handlers' arguments, the variables / functions by what they hold), the options (.name), the objects ($name), the
// built-ins (<name>). Display only: the readable text carries its own table (";= name=raw" comment lines) and
// FSpliceRaw turns it back into the converter's text, which compiles to the same bytes (the game never sees a name).
// The game data has no symbol names (the compiler kept ids only): every name here is the port's
// (notes/SCRIPTNAMES_2026-11-17.md).

#include <functional>
#include <map>
#include <string>
#include <vector>

// The names a world's scripts give the symbols and their own options (SpliceCollectNames over every group's text): a
// symbol id is one name in all of the game's scripts, so a group that only uses one gets the name another gives it
struct SPLICEWORLDNAMES
{
    std::map<int, std::pair<std::string, int>> mpsym, mpopt;   // id -> (name, confidence)
};

struct SPLICENAMECTX
{
    // a short name for the object oid as the world has it (the editor: its outliner name; --dump-splice: its group's
    // name and class); empty: none. The namer makes a "$name" token of it (unique in the text, the oid added when two
    // objects share a name).
    std::function<std::string(int oid)> pfnObject;
    // the title line ("; group N name")
    std::string title;
    // the world's other groups' names (null: this text alone)
    const SPLICEWORLDNAMES* pworld = nullptr;
};

// What got a name (distinct values of the text; a symbol named from its role or what it holds counts as named, a
// kind-only name (fn-N, obj-N) as partly named; words: symbols that only appear in the asserts' quoted messages)
struct SPLICENAMESTATS
{
    int csym = 0, csymNamed = 0, csymKind = 0, csymWord = 0;
    int cevent = 0, ceventNamed = 0;             // entry points (symbols 0..63 defined at a module's top)
    int coptEngine = 0, coptEngineNamed = 0;     // option ids of the engine (<= 1232)
    int coptScript = 0, coptScriptNamed = 0;     // script-made options (the sidebag, > 1232)
    int coid = 0, coidNamed = 0;                 // literal oids (find-object / find-objects / names lists / known oid arguments)
    int cbif = 0, cbifNamed = 0;                 // built-ins called (a readable alias or the converter's name)
    int cmsg = 0;                                // case values of event handlers (event words; kept as numbers)
    void Add(const SPLICENAMESTATS& o);
};

// The converter's text -> the readable text. false (and error) when the text does not parse (or has debug-info 1:
// shown as it is).
bool FSpliceReadable(const std::string& raw, const SPLICENAMECTX& ctx, std::string& readable, std::string& error,
                     SPLICENAMESTATS* pstats = nullptr);
// The readable text -> the converter's text (names back to symN / numbers by the ";=" lines and the port's tables;
// a new bare name becomes a new symbol (anew lists them)). false (and error with the line) for a name it cannot map.
bool FSpliceRaw(const std::string& readable, std::string& raw, std::string& error, std::vector<std::string>* panew = nullptr);
// What a group's scripts do to objects (the "when does it exist" of the editor): the calls of .add (39) / .remove (40)
// / .fire-activate (12) / .fire-deactivate (13) and a proxy's .instantiate (1019) / .keep-instance (1023) /
// .release-instance (1024), with the object they act on (its oid; 0: the object running the script; -1: found
// otherwise), the event or function they run in (readable name) and the conditions around them (readable text).
struct SPLICEOBJACT
{
    int imodule = 0;
    int oid = -1;
    int optid = 0;
    std::string event;
    std::string cond;
    std::string who;    // the object expression (readable)
};
bool FSpliceObjectActions(const std::string& raw, const SPLICENAMECTX& ctx, std::vector<SPLICEOBJACT>& aact, std::string& error);
// One group's names into the world's (call for every group, then name each with ctx.pworld set)
void SpliceCollectNames(const std::string& raw, const SPLICENAMECTX& ctx, SPLICEWORLDNAMES& world);
// Whether a text is a readable one (has the readable header)
bool FSpliceTextReadable(const std::string& text);

// The port's tables (null: no name)
const char* SzSpliceEventName(int symid);          // the engine's entry points / events (symbols 0..63)
const char* SzSpliceOptionName(int optid);         // option ids as the scripts use them (geto / seto! / addo / ensureo)
const char* SzSpliceBifAlias(int bifk);            // the built-ins' readable names (without < >)
// What an event means, by its name (on-start ...; null: not an event)
const char* SzSpliceEventWhat(const char* name);
// What an event handler's arguments are (null past its last)
const char* SzSpliceEventArg(int symid, int iarg);
// Self-test of the tables (unique names, every name round-trips); prints failures, returns their count
int SpliceNamesSelfTest();
