#pragma once

// Readable splice scripts (the map editor's script view, --dump-splice): the converter's text (worldparts.h
// FSpliceToText: symN, option numbers, oids, #bif:N) with names for the symbols (the engine's events by role, the
// handlers' arguments, the variables / functions by what they hold), the options (.name), the objects ($name), the
// built-ins (<name>), the event words (~name), the job events (%name). The readable text carries its own table
// (";= name=raw" comment lines) and FSpliceRaw turns it back into the converter's text, which compiles to the same
// bytes. Mods may keep their scripts readable: the game compiles the names when it puts the world together
// (FSpliceTextToBytes; a new name gets a stable id: SpliceNewSymbolId). The game data has no symbol names (the
// compiler kept ids only): every name here is the port's (notes/SCRIPTNAMES_2026-11-17.md, notes/SCRIPTNAMES2_2026-11-17.md).

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

// The names a world's scripts give the symbols and their own options (SpliceCollectNames over every group's text): a
// symbol id is one name in all of the game's scripts, so a group that only uses one gets the name another gives it
struct SPLICEWORLDNAMES
{
    std::map<int, std::pair<std::string, int>> mpsym, mpopt;   // id -> (name, confidence)
    std::map<int, std::pair<std::string, int>> mpword;         // event word -> (name, how many handlers say it)
    std::map<int, std::map<std::string, int>> mpwordvote;      // (each name's count over the groups)
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
    // where an event word comes from (the world's animation clips' / cutscenes' / command lists' event records): a
    // name for it when the handlers say nothing ("dimitri-clip-0-5s"); empty: not known
    std::function<std::string(int word)> pfnEventWord;
    // the generated messages of the asserts (a comment line "; expects: ..." before each)
    bool fAssertNotes = true;
};

// What got a name (distinct values of the text; a symbol named from its role or what it holds counts as named, a
// kind-only name (fn-N, obj-N) as partly named; words: symbols that only appear in the asserts' quoted messages).
// Guessed: a "?name" (round 3): descriptive (what it is and where it lives, or a guessed engine field), not traced.
struct SPLICENAMESTATS
{
    int csym = 0, csymNamed = 0, csymKind = 0, csymWord = 0, csymGuess = 0;
    int cevent = 0, ceventNamed = 0;             // entry points (symbols 0..63 defined at a module's top)
    int coptEngine = 0, coptEngineNamed = 0, coptEngineGuess = 0;  // option ids of the engine (<= 1232)
    int coptScript = 0, coptScriptNamed = 0, coptScriptGuess = 0;  // script-made options (the sidebag, > 1232)
    int coid = 0, coidNamed = 0, coidGuess = 0;  // literal oids (find-object / find-objects / names lists / known oid arguments)
    int cbif = 0, cbifNamed = 0;                 // built-ins called (a readable alias or the converter's name)
    int cmsg = 0, cmsgNamed = 0, cmsgGuess = 0;  // case values of on-event handlers (event words: ~name)
    int ccls = 0, cclsNamed = 0;                 // class ids (find-objects' first argument: &name)
    int cassert = 0;                             // asserts given a generated message
    void Add(const SPLICENAMESTATS& o);
};

// The names a text got, with how sure each is (the names table: --dump-splice's names.tsv): kind (symbol, option,
// script-option, object, event-word, class, job-event), the game's number, the name, its confidence ("fixed": the
// engine's role; "traced": from the engine's code / tables or what it holds; "use": from how the scripts use it;
// "guess": a "?name"), and how it was found
struct SPLICENAMEROW
{
    std::string kind, raw, name, conf;
};

// The converter's text -> the readable text. false (and error) when the text does not parse (or has debug-info 1:
// shown as it is).
bool FSpliceReadable(const std::string& raw, const SPLICENAMECTX& ctx, std::string& readable, std::string& error,
                     SPLICENAMESTATS* pstats = nullptr, std::vector<SPLICENAMEROW>* prows = nullptr);
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
// Whether a text is a readable one (has the readable header or a ";=" table line)
bool FSpliceTextReadable(const std::string& text);
// A new name's symbol id: stable (the same name the same id in every group and mod: a hash of the name in
// 200000..999999, away from the game's symbols and oids); used by FSpliceRaw for names its table does not map
int SpliceNewSymbolId(const std::string& name);
// The readable text with its table made again: every name the text uses (its own ";=" lines, the port's tables, new
// names with their stable ids), the text above the table unchanged (its lines stay where they are). anew: the new names.
bool FSpliceNormalize(const std::string& readable, std::string& out, std::string& error, std::vector<std::string>* panew = nullptr);
// A scripts part's text (the converter's or a readable one) compiled: the readable one through FSpliceRaw; a text
// without the readable header that does not compile as the converter's is tried as a readable one too (a mod's
// hand-written names). error: "line N: why".
bool FSpliceTextToBytes(const std::string& text, std::vector<uint8_t>& ab, std::string& error);

// The port's tables (null: no name)
const char* SzSpliceEventName(int symid);          // the engine's entry points / events (symbols 0..63)
const char* SzSpliceOptionName(int optid);         // option ids as the scripts use them (geto / seto! / addo / ensureo)
const char* SzSpliceBifAlias(int bifk);            // the built-ins' readable names (without < >)
int SpliceOptionByName(const std::string& name);   // an option name (without the .) -> its id (-1: none)
int SpliceBifByName(const std::string& name);      // a built-in's name with < > (the alias or the converter's) -> bifk (-1)
const char* SzSpliceJobEvent(int n);               // the jobs' broadcast events (%name without the %)
int SpliceJobEventByName(const std::string& name);  // (without the %) -> n (-1: none)
int SpliceSymbolByName(const std::string& name);    // an engine event's / fixed parameter's name -> its symbol (-1: none)
const char* SzSpliceClassName(int cid);             // a class id's name (&name without the &; null: none)
int SpliceClassByName(const std::string& name);     // (without the &) -> cid (-1: none)
// What kind of object has all these engine options (the classes' option tables: their common class), as a name
// ("guard", "emitter" ...; empty: not one known class); splicediag.cpp (it reads the tables)
std::string SpliceKindOfOptions(const std::set<int>& aopt);
bool FSpliceReservedWord(const std::string& s);     // the special forms and the reader's own words
// the ";= name=raw" table lines of a text
void SpliceLegend(const std::string& text, std::map<std::string, std::string>& mp);
// every name the port's tables give (for suggestions): options (.name), events, built-ins (<name>), job events (%name)
void SpliceTableNames(std::vector<std::string>& a);
// What an event means, by its name (on-start ...; null: not an event)
const char* SzSpliceEventWhat(const char* name);
// What an event handler's arguments are (null past its last)
const char* SzSpliceEventArg(int symid, int iarg);
// Self-test of the tables (unique names, every name round-trips); prints failures, returns their count
int SpliceNamesSelfTest();
