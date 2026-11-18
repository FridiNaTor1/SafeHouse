// Readable splice scripts (splicenames.h): parse the converter's text, name what the port knows, print it back with
// its own name table; and the way back.
#include "splicenames.h"

#include "worldparts.h"
#include "splicesexp.h"
#include "splicediag.h"
#include "cls.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace
{
#include "splicenames_tables.inc"

// the converter's built-in names (worldparts.cpp's table, without < >); the bifs it prints as #bif:N have none
const char* SzBifNameRaw(int bifk)
{
    return SzSpliceBifRawName(uint32_t(bifk));
}

using namespace splicesexp;

// the words the reader keeps for itself (worldparts.cpp's tag words) and the names we never give
bool FReserved(const std::string& s)
{
    static const std::set<std::string> s_set = {"#none", "set!", "define", "assert", "debug", "if", "or", "and", "cond", "else", "case", "let",
                                                "while", "lambda", "begin", "import", "quote", ".", "!", "module", "debug-info"};
    return s_set.count(s) != 0;
}

// a guessed name ("?name": descriptive, from how the scripts use it; its meaning not traced in the engine)
bool FGuess(const std::string& n) { return !n.empty() && n[0] == '?'; }
std::string Unq(const std::string& n) { return FGuess(n) ? n.substr(1) : n; }
// a name built from others: a guess when any part is
std::string Cat(std::initializer_list<std::string> a)
{
    bool fGuess = false;
    std::string r;
    for (const std::string& p : a) { fGuess = fGuess || FGuess(p); r += Unq(p); }
    return fGuess ? "?" + r : r;
}

std::string Slug(const std::string& in)
{
    if (FGuess(in)) { const std::string r = Slug(in.substr(1)); return r.empty() ? r : "?" + r; }
    std::string s;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (std::isalnum(c)) s += char(std::tolower(c));
        else if (c == '?' && i + 1 == in.size()) { while (!s.empty() && s.back() == '-') s.pop_back(); s += '?'; }
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0]))) s = "n" + s;
    return s;
}

// plural / singular of a name (for lists and their elements)
std::string Plural(const std::string& s)
{
    if (s.empty()) return "objects";
    if (FGuess(s)) return "?" + Plural(s.substr(1));
    if (s.back() == 's') return s;
    if (s.back() == 'y' && s.size() > 1 && !std::strchr("aeiou", s[s.size() - 2])) return s.substr(0, s.size() - 1) + "ies";
    return s + "s";
}
std::string Singular(const std::string& s)
{
    if (FGuess(s)) return "?" + Singular(s.substr(1));
    if (s.size() > 3 && s.compare(s.size() - 3, 3, "ies") == 0) return s.substr(0, s.size() - 3) + "y";
    if (s.size() > 1 && s.back() == 's' && s[s.size() - 2] != 's') return s.substr(0, s.size() - 1);
    return s + "-item";
}

// ---------------------------------------------------------------------------------------------------------
// the tables as maps
const std::unordered_map<int, const char*>& MpOptName()
{
    static const std::unordered_map<int, const char*> s_mp = [] {
        std::unordered_map<int, const char*> mp;
        for (const SPLICEOPTNAME& o : c_aopt) mp[o.optid] = o.name;
        return mp;
    }();
    return s_mp;
}
const std::unordered_map<std::string, int>& MpOptByName()
{
    static const std::unordered_map<std::string, int> s_mp = [] {
        std::unordered_map<std::string, int> mp;
        for (const SPLICEOPTNAME& o : c_aopt) mp[o.name] = o.optid;
        return mp;
    }();
    return s_mp;
}
const SPLICEEVENTNAME* PeventOf(int sym)
{
    for (const SPLICEEVENTNAME& e : c_aevent)
        if (e.symid == sym) return &e;
    return nullptr;
}
// the bif token the converter prints for bifk, and the readable one
std::string RawBif(int bifk)
{
    if (const char* sz = SzBifNameRaw(bifk)) return std::string("<") + sz + ">";
    return "#bif:" + std::to_string(bifk);
}
std::string ReadableBif(int bifk)
{
    for (const SPLICEBIFALIAS& b : c_abif)
        if (b.bifk == bifk) return std::string("<") + b.alias + ">";
    return RawBif(bifk);
}
// a raw bif token's number (-1: not a bif)
int BifOfRaw(const std::string& s)
{
    if (s.rfind("#bif:", 0) == 0) return std::atoi(s.c_str() + 5);
    if (s.size() > 2 && s.front() == '<' && s.back() == '>')
    {
        static const std::unordered_map<std::string, int> s_mp = [] {
            std::unordered_map<std::string, int> mp;
            for (int i = 0; i < 256; ++i)
                if (const char* sz = SzBifNameRaw(i)) mp[std::string("<") + sz + ">"] = i;
            return mp;
        }();
        const auto it = s_mp.find(s);
        return it == s_mp.end() ? -1 : it->second;
    }
    return -1;
}
const char* SzJobEvent(int n)
{
    for (const SPLICEJOBEVENT& e : c_ajobevent)
        if (e.n == n) return e.name;
    return nullptr;
}
// what kind of object has an engine option (a symbol whose .goal-state is set holds a state machine runner)
const char* SzObjectKindOfOption(int optid)
{
    switch (optid)
    {
    case 478: case 481: return "state-machine";
    case 253: case 252: case 250: case 251: case 237: case 255: case 236: return "animation";
    case 1104: case 1105: case 1080: case 1081: case 1082: case 1093: case 1111: return "task";
    case 1067: case 1068: case 1069: case 1079: return "job";
    case 264: case 265: case 266: case 267: case 273: case 274: return "sound";
    case 560: case 561: case 562: return "emitter";
    case 1019: case 1020: case 1021: case 1023: case 1024: case 1025: return "proxy";
    case 985: case 986: case 987: case 980: case 981: case 982: case 983: return "screen";
    case 638: case 646: case 647: case 639: return "cutscene";
    case 381: case 380: case 386: case 405: case 359: case 360: case 369: return "guard";
    case 844: case 845: case 846: return "world";
    default: return nullptr;
    }
}
const char* SzMethodArg(int optid, int iarg)
{
    for (const SPLICEMETHODARG& m : c_amethodarg)
        if (m.optid == optid && m.iarg == iarg) return m.name;
    return nullptr;
}
bool FOidArg(int optid, int iarg)
{
    for (const SPLICEOIDARG& o : c_aoidarg)
        if (o.optid == optid && o.iarg == iarg) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------------
// the namer
enum CONF
{
    CONF_Kind = 10,      // only what it is (fn / obj / list)
    CONF_Guess = 15,     // "?name": a descriptive guess (its kind and where it lives, or a guessed engine name)
    CONF_Use = 20,       // from how it is used
    CONF_Arg = 30,       // a call's argument passed to it
    CONF_Role = 50,      // an event handler's argument by its role
    CONF_Value = 60,     // what it holds (an assignment)
    CONF_Fixed = 100,
};

struct CAND
{
    std::string name;
    int conf = 0;
    int count = 0;
};

struct NAMER
{
    const SPLICENAMECTX& ctx;
    SPLICENAMESTATS st;
    std::map<int, std::vector<CAND>> mpsymcand;
    std::map<int, std::vector<CAND>> mpsymcandPrev;   // the previous pass's (what a name built from another one reads)
    std::map<int, int> mpsymFirst;                 // order of first appearance
    std::set<int> setsymWordOnly, setsymNonWord;
    std::map<int, std::vector<CAND>> mpoptcand;    // the scripts' own options (sidebag)
    std::map<int, std::vector<CAND>> mpoptcandPrev;
    std::set<int> setsymEvKey;                     // on-event's event parameters: their case values are event words
    std::map<int, std::vector<CAND>> mpwordcand;   // event word -> what its clauses do
    std::set<int> setword;
    std::map<int, std::string> mpword;             // event word -> ~name
    std::set<int> setoid;                          // literal oids
    std::set<int> setcls;                          // class ids (find-objects)
    std::set<int> setoptEngine, setoptScript, setbif, setevent;
    std::set<std::string> setcase;
    std::map<int, int> mpsymOid;                   // a symbol that holds one object: its oid (find-object N)
    std::set<int> setsymOidKey;                    // symbols holding an oid: their case values are oids
    std::set<int> setsymJobEvKey;                  // symbols holding a job event number (%name in their case values)
    int iseq = 0;

    // the final maps
    std::map<int, std::string> mpsym, mpopt, mpoid, mpcls;
    std::map<int, std::pair<std::string, int>> mpsymBase, mpoptBase;   // the names before made unique, their confidence
    std::map<int, int> mpsymConf, mpoptConf, mpwordConf;               // the chosen names' confidence (the names table)
    std::map<std::string, std::string> mpLegend;   // name -> raw (the ";=" lines)

    explicit NAMER(const SPLICENAMECTX& c) : ctx(c) {}

    void Note(int sym)
    {
        if (!mpsymFirst.count(sym)) mpsymFirst[sym] = iseq++;
    }
    void Cand(int sym, const std::string& name, int conf)
    {
        if (sym < 0 || name.empty() || name == "?") return;
        if (FGuess(name)) conf = std::min(conf, int(CONF_Guess));
        Note(sym);
        std::vector<CAND>& a = mpsymcand[sym];
        for (CAND& c : a)
            if (c.name == name) { c.count++; c.conf = std::max(c.conf, conf); return; }
        a.push_back({name, conf, 1});
    }
    void OptCand(int optid, const std::string& name, int conf)
    {
        if (name.empty() || name == "?") return;
        if (FGuess(name)) conf = std::min(conf, int(CONF_Guess));
        std::vector<CAND>& a = mpoptcand[optid];
        for (CAND& c : a)
            if (c.name == name) { c.count++; c.conf = std::max(c.conf, conf); return; }
        a.push_back({name, conf, 1});
    }

    static const CAND* PBest(const std::vector<CAND>& a)
    {
        if (a.empty()) return nullptr;
        const CAND* pbest = &a[0];
        for (const CAND& c : a)
            if (c.conf > pbest->conf || (c.conf == pbest->conf && c.count > pbest->count)) pbest = &c;
        return pbest;
    }
    // a script-made option's name so far (the previous pass's, then the world's): empty when only its kind is known
    std::string ScriptOptName(int opt) const
    {
        const std::map<int, std::vector<CAND>>& mp = mpoptcandPrev.empty() ? mpoptcand : mpoptcandPrev;
        if (const auto it = mp.find(opt); it != mp.end())
            if (const CAND* p = PBest(it->second); p && p->conf > CONF_Kind) return p->name;
        if (ctx.pworld)
            if (const auto it = ctx.pworld->mpopt.find(opt); it != ctx.pworld->mpopt.end() && it->second.second > CONF_Kind) return it->second.first;
        return std::string();
    }
    void WordCand(int word, const std::string& name, int conf)
    {
        if (name.empty()) return;
        std::vector<CAND>& a = mpwordcand[word];
        for (CAND& c : a)
            if (c.name == name) { c.count++; c.conf = std::max(c.conf, conf); return; }
        a.push_back({name, conf, 1});
    }

    std::string ObjName(int oid) const
    {
        std::string s = ctx.pfnObject ? Slug(ctx.pfnObject(oid)) : std::string();
        return s;
    }
    std::string OptName(int optid) const
    {
        if (const auto it = MpOptName().find(optid); it != MpOptName().end()) return it->second;
        return std::string();
    }

    // a name for what an expression gives (empty: nothing to say)
    std::string NameOfExpr(const NODE& e, int depth = 0)
    {
        if (depth > 4) return std::string();
        if (!e.fList)
        {
            if (const int s = SymOf(e.atom); s >= 0) return NameFromSymOrExpr(e, depth);   // (a copy: the source's name)
            if (e.atom == "#t" || e.atom == "#f") return "flag";
            if (e.atom.rfind("#vec", 0) == 0) return "vec";
            if (e.atom == "#obj:0") return "obj";
            return std::string();
        }
        if (!e.atom.empty()) return e.atom == "#vec" ? "vec" : std::string();
        if (e.a.empty()) return "list";
        const NODE& h = e.a[0];
        if (h.fList)
        {
            // ((<geto> X opt) args): what the method gives
            if (FIsHead(h, "<geto>") && h.a.size() >= 3 && !h.a[2].fList)
            {
                long opt = 0;
                if (FInt(h.a[2].atom, &opt))
                {
                    if (opt == 182 || opt == 183)
                    {
                        long oid = 0;
                        if (e.a.size() >= 2 && FInt(e.a[1].atom, &oid) && oid > 0)
                            if (const std::string n = ObjName(int(oid)); !n.empty()) return n;
                        return opt == 182 ? "animation" : "state-machine";
                    }
                    if (opt == 1019 || opt == 1020) return "instance";
                }
            }
            return std::string();
        }
        const std::string& f = h.atom;
        if ((f == "<find-object>" || f == "<find-object-kind-4>") && e.a.size() >= 2)
        {
            long oid = 0;
            if (FInt(e.a[1].atom, &oid))
            {
                if (const std::string n = ObjName(int(oid)); !n.empty()) return n;
                return "obj";
            }
            std::string n = NameOfExpr(e.a[1], depth + 1);
            // (an object found by an oid an option holds: the option's name without its "-oid")
            while (n.size() > 4 && n.compare(n.size() - 4, 4, "-oid") == 0) n.resize(n.size() - 4);
            return n.empty() || n == "oid" || n == "?oid" ? "obj" : n;
        }
        if (f == "<find-objects>")
        {
            long cid = 0;
            if (e.a.size() >= 2 && FInt(e.a[1].atom, &cid))
                if (const char* sz = SzSpliceClassName(int(cid))) return Plural(sz);
            return "objects";
        }
        if (f == "#bif:113" || f == "<find-object-113 (0x1241E8)>" || f == "<find-nearest-objects>")
        {
            long oid = 0;
            if (e.a.size() >= 2 && FInt(e.a[1].atom, &oid))
                if (const std::string n = ObjName(int(oid)); !n.empty()) return Plural(n);
            return "objects";
        }
        if (f == "<find-objects-of-names>")
        {
            if (e.a.size() >= 2 && FIsHead(e.a[1], "quote") && e.a[1].a.size() >= 2 && e.a[1].a[1].fList && !e.a[1].a[1].a.empty())
            {
                long oid = 0;
                if (FInt(e.a[1].a[1].a[0].atom, &oid))
                    if (const std::string n = ObjName(int(oid)); !n.empty()) return Plural(n);
            }
            return "objects";
        }
        if (f == "<find-player-object>") return "world";
        if (f == "<find-world-object>") return "sound";
        if (f == "<find-camera-object>") return "camera";
        if (f == "<object-3E0F9C>") return "current-job";
        if (f == "<object-3E0FA0>") return "current-task";
        if (f == "<object-3E0FA4>") return "checkpoint-task";
        if (f == "<bif-137>") return "prompt";
        if (f == "#bif:129" || f == "<new-sound (0x1251E8)>") return "sound";
        if (f == "<geto>" && e.a.size() >= 3)
        {
            long opt = 0;
            if (FInt(e.a[2].atom, &opt))
            {
                if (const std::string n = OptName(int(opt)); !n.empty() && n.back() != '?') return n;
                if (opt > 1232)
                {
                    std::string n = ScriptOptName(int(opt));
                    if (!n.empty() && n.back() == '?') n.pop_back();
                    return n;
                }
            }
            return std::string();
        }
        if (f == "lambda") return "fn";
        if (f == "<->" && e.a.size() == 3)
        {
            const std::string a = NameFromSymOrExpr(e.a[1], depth), b = NameFromSymOrExpr(e.a[2], depth);
            if (a == "time" || a.rfind("time-", 0) == 0) return "elapsed";
            if (a == "position" || b == "position") return "offset";
            return std::string();
        }
        if (f == "<get-element>" && e.a.size() >= 2)
        {
            const std::string a = NameFromSymOrExpr(e.a[1], depth);
            return a.empty() ? std::string() : Singular(a);
        }
        if (f == "<atan2>") return "angle";
        if (f == "<vector-length>" || f == "<vector-distance-xy>") return "distance";
        if (f == "<inherited>") return "inherited";
        if (f == "<sound>" || f == "<find-world-object>") return "sound";
        if (f == "<world-index>") return "place";
        if (f == "<get-progress>") return "progress";
        if (f == "<item-acquired?>") return "acquired";
        if (f == "<clq>") return "curve";
        if (f == "<lm>") return "range";
        if (f == "quote") return e.a.size() >= 2 && e.a[1].fList ? "list" : std::string();
        if (f == "<list>" || f == "<cons>" || f == "<append>" || f == "<shuffle>") return "list";
        if (f == "<filter>" && e.a.size() >= 3) { const std::string n = NameOfExpr(e.a[2], depth + 1); return n.empty() ? "list" : n; }
        if (f == "<map>") return "list";
        if (f == "<car>" || f == "<nth>")
            if (e.a.size() >= 2) { const std::string n = NameFromSymOrExpr(e.a[1], depth); return n.empty() ? std::string("item") : Singular(n); }
        if (f == "<cdr>") return "rest";
        if ((f == "<+>" || f == "<->") && e.a.size() >= 3)
        {
            const std::string a = NameFromSymOrExpr(e.a[1], depth);
            if (a == "position" || a == "vec" || a == "offset") return a;
        }
        if (f == "<*>" && e.a.size() >= 2)
        {
            const std::string a = NameFromSymOrExpr(e.a[1], depth);
            if (a == "mat" || a == "vec") return a;
        }
        // (round 3) arithmetic of named values: what it computes ("distance-per-launch-speed", "sine-of-angle")
        {
            static const std::map<std::string, const char*> s_mpop = {{"</>", "-per-"}, {"<*>", "-times-"}, {"<+>", "-plus-"}, {"<->", "-minus-"},
                                                                      {"<min>", "-at-most-"}, {"<max>", "-at-least-"}, {"<modulo>", "-modulo-"}};
            static const std::map<std::string, const char*> s_mpfn = {{"<sin>", "sine-of-"}, {"<cos>", "cosine-of-"}, {"<abs>", "size-of-"},
                                                                      {"<sqrt>", "root-of-"}, {"<round>", "rounded-"}};
            auto operand = [&](const NODE& x) -> std::string {
                if (!x.fList && !x.atom.empty() && (std::isdigit(static_cast<unsigned char>(x.atom[0])) || (x.atom[0] == '-' && x.atom.size() > 1)))
                {
                    std::string v = x.atom;
                    if (v.find('.') != std::string::npos) { while (v.back() == '0') v.pop_back(); if (v.back() == '.') v.pop_back(); }
                    for (char& c : v) if (c == '.') c = '-';
                    if (v[0] == '-') v = "minus" + v;
                    return v;
                }
                const std::string n = NameFromSymOrExpr(x, depth + 1);
                return FTelling(n) ? n : std::string();
            };
            if (const auto it = s_mpop.find(f); it != s_mpop.end() && e.a.size() == 3)
            {
                const std::string a = operand(e.a[1]), b = operand(e.a[2]);
                const bool fNa = !a.empty() && std::isdigit(static_cast<unsigned char>(Unq(a)[0])), fNb = !b.empty() && std::isdigit(static_cast<unsigned char>(Unq(b)[0]));
                const bool fCommutes = f == "<*>" || f == "<+>" || f == "<min>" || f == "<max>";
                if (!a.empty() && !b.empty() && !(fNa && fNb) && Unq(a).size() + Unq(b).size() <= 36)
                {
                    if (!fNa) return Cat({a, it->second, b});
                    if (fCommutes) return Cat({b, it->second, a});
                }
            }
            if (const auto it = s_mpfn.find(f); it != s_mpfn.end() && e.a.size() == 2)
                if (const std::string a = operand(e.a[1]); !a.empty() && !std::isdigit(static_cast<unsigned char>(Unq(a)[0])) && Unq(a).size() <= 30)
                    return Cat({it->second, a});
        }
        {
            // (what only says what it is: the namer gives these as kind names, name-N)
            static const std::map<std::string, std::string> s_mpkind = {
                {"<sin>", "sine"}, {"<cos>", "cosine"}, {"<min>", "min"}, {"<max>", "max"}, {"<*>", "product"}, {"</>", "ratio"},
                {"<+>", "sum"}, {"<->", "difference"}, {"<modulo>", "remainder"}, {"<abs>", "magnitude"}, {"<sqrt>", "root"},
                {"if", "value"}, {"cond", "value"}, {"<clq-evaluate>", "curve-value"}, {"<vector-radian-normal>", "angle"},
                {"<bits-and>", "bits"}, {"<bits-or>", "bits"}};
            if (const auto it = s_mpkind.find(f); it != s_mpkind.end()) return it->second;
            if (f.rfind("<bif-", 0) == 0 || f.rfind("#bif:", 0) == 0) return "value";
        }
        if (f == "<vector>" || f == "<vector-normalize>" || f == "<vector-cross-product>" || f == "<convert-object-position>" ||
            f == "<convert-object-vector>")
            return "vec";
        if (f == "<matrix>" || f == "<matrix-look-at>") return "mat";
        if (f == "<schedule-callback>") return "callback";
        if (f == "<random>") return "random";
        if (f == "<current-time>") return "time";
        if (f == "<length>") return "count";
        if (f == "<vector-distance>" || f == "<vector-lth>") return "distance";
        return std::string();
    }
    // a symbol's best candidate so far, else the expression's name
    std::string NameFromSymOrExpr(const NODE& e, int depth)
    {
        if (!e.fList)
            if (const int s = SymOf(e.atom); s >= 0)
            {
                const std::map<int, std::vector<CAND>>& mp = mpsymcandPrev.empty() ? mpsymcand : mpsymcandPrev;
                const auto it = mp.find(s);
                if (it == mp.end() || it->second.empty()) return std::string();
                const CAND* pbest = &it->second[0];
                for (const CAND& c : it->second)
                    if (c.conf > pbest->conf || (c.conf == pbest->conf && c.count > pbest->count)) pbest = &c;
                return pbest->conf > CONF_Kind && (!FGuess(pbest->name) || FTelling(pbest->name)) ? pbest->name : std::string();
            }
        return NameOfExpr(e, depth + 1);
    }

    // a function's name from its body (a lambda node)
    // what one statement does, as a name (empty: nothing to say)
    std::string NameOfStatement(const NODE& b, int depth)
    {
        if (!b.fList || depth > 3) return std::string();
        if (FIsHead(b, "<seto!>") && b.a.size() >= 3)
        {
            long opt = 0;
            if (FInt(b.a[2].atom, &opt))
                if (const std::string n = OptName(int(opt)); !n.empty())
                {
                    std::string r = Cat({"set-", n.back() == '?' ? n.substr(0, n.size() - 1) : n});
                    // (a literal value: on / off / the number)
                    if (b.a.size() >= 4 && !b.a[3].fList)
                    {
                        if (b.a[3].atom == "#t") r += "-on";
                        else if (b.a[3].atom == "#f") r += "-off";
                        else if (long v = 0; FInt(b.a[3].atom, &v) && v >= -99 && v <= 999) r += "-" + std::string(v < 0 ? "minus-" : "") + std::to_string(std::labs(v));
                    }
                    return r;
                }
        }
        if (FIsHead(b, "set!") && b.a.size() >= 3 && !b.a[1].fList && b.a[2].fList && !b.a[2].a.empty() && FIsHead(b.a[2].a[0], "<geto>"))
            if (std::string n = NameOfStatement(b.a[2], depth + 1); !n.empty()) return n;
        if (FIsHead(b, "set!") && b.a.size() >= 3 && !b.a[1].fList)
        {
            const std::string v = NameFromSymOrExpr(b.a[1], 0);
            if (!v.empty()) return Cat({b.a[2].atom == "#obj:0" ? "clear-" : "set-", v});
        }
        // ((<geto> X .opt) args): the method's name and who it is called on
        if (!b.a.empty() && FIsHead(b.a[0], "<geto>") && b.a[0].a.size() >= 3)
        {
            long opt = 0;
            if (FInt(b.a[0].a[2].atom, &opt))
                if (std::string n = opt > 1232 ? ScriptOptName(int(opt)) : OptName(int(opt)); !n.empty() && n.back() != '?')
                {
                    const std::string who = NameFromSymOrExpr(b.a[0].a[1], 0);
                    if (!who.empty() && who != "self" && who != "this" && who != "owner") n = Cat({n, "-", who});
                    return n;
                }
        }
        // (F args): a wrapper of another function
        if (!b.a.empty() && !b.a[0].fList)
            if (const int sf = SymOf(b.a[0].atom); sf >= 0)
                if (const std::string n = NameFromSymOrExpr(b.a[0], 0); !n.empty()) return n;
        // the built-ins that do something by themselves (by their number: the converter prints some as #bif:N)
        if (!b.a.empty() && !b.a[0].fList)
        {
            static const std::map<int, std::string> s_mp = {
                {130, "rumble"}, {132, "set-progress"}, {139, "set-item-acquired"}, {106, "cancel-callback"}, {105, "schedule"},
                {107, "defer-update"}, {129, "start-sound"}, {131, "read-progress"}};
            const int bk = BifOfRaw(b.a[0].atom);
            if (const auto it = s_mp.find(bk); it != s_mp.end())
            {
                if (it->second == "rumble" && b.a.size() >= 2 && !b.a[1].fList) return "rumble-" + b.a[1].atom;
                // (a callback scheduled: what it runs)
                if (bk == 105 && b.a.size() >= 3)
                {
                    std::string n = FIsHead(b.a[2], "lambda") ? NameOfLambda(b.a[2]) : NameFromSymOrExpr(b.a[2], 0);
                    if (!n.empty() && n != "fn" && n != "callback") return Cat({"schedule-", n});
                }
                return it->second;
            }
            if (b.a[0].atom == "<print>") return "print";
        }
        // (<seto!> X .opt V): the method-less setters of the scripts' own options
        if (FIsHead(b, "<seto!>") && b.a.size() >= 3)
        {
            long opt = 0;
            if (FInt(b.a[2].atom, &opt) && opt > 1232)
                if (std::string n = ScriptOptName(int(opt)); !n.empty()) { if (n.back() == '?') n.pop_back(); return Cat({"set-", n}); }
        }
        if (FIsHead(b, "if") && b.a.size() >= 3) return NameOfStatement(b.a[2], depth + 1);
        if (FIsHead(b, "begin"))
            for (std::size_t k = 1; k < b.a.size(); ++k)
                if (std::string n = NameOfStatement(b.a[k], depth + 1); !n.empty()) return n;
        return std::string();
    }
    // a function's name from its body (a lambda node): its first statement that says something (asserts skipped)
    std::string NameOfLambda(const NODE& lam)
    {
        // (lambda () x): a getter
        if (lam.a.size() == 3 && !lam.a[2].fList && SymOf(lam.a[2].atom) >= 0)
            if (const std::string n = NameFromSymOrExpr(lam.a[2], 0); !n.empty()) return Cat({"get-", n});
        for (std::size_t k = 2; k < lam.a.size(); ++k)
        {
            const NODE& b = lam.a[k];
            if (FIsHead(b, "assert") || FIsHead(b, "define")) continue;
            if (std::string n = NameOfStatement(b, 0); !n.empty()) return n;
        }
        return std::string();
    }

    // names that only say what a value is (fn-N, obj-N, sum-N ...)
    static bool FKindName(const std::string& n)
    {
        static const std::set<std::string> s_set = {"obj", "list", "flag", "item", "sine", "cosine", "min", "max", "product", "ratio", "sum",
                                                    "difference", "remainder", "magnitude", "root", "value", "curve-value", "bits", "rest", "n", "arg"};
        return s_set.count(n) != 0;
    }
    // ---- the analysis: one pass gathering candidates (run twice: the second sees the first's names)
    struct FN
    {
        const NODE* plam = nullptr;
    };
    std::map<int, const NODE*> mpfn;   // symbols defined as lambdas: their lambda
    // where each symbol / script-made option lives (round 3: the last resort names "?kind-in-home"): the define it is
    // defined in (a symbol), or -1 - module (a module's top)
    std::map<int, int> mpsymHome, mpoptHome;
    std::map<int, int> mpsymParam;         // a function's parameter: its place (1-based)
    std::map<int, std::set<int>> mpsymOpts; // the engine options taken of what a symbol holds (its class)
    std::map<int, const NODE*> mpsymQuote; // symbols defined as a quoted list: the list
    std::set<const NODE*> setquoteOid;     // quoted lists whose numbers are oids (find-objects-of-names)
    std::set<int> setsymNumber;            // symbols defined as a constant number
    std::vector<int> stkHome;
    int imodCur = 0;
    int Home() const { return stkHome.empty() ? -1 - imodCur : stkHome.back(); }
    void NoteHome(int s) { if (s >= 0 && !mpsymHome.count(s)) mpsymHome[s] = Home(); }
    // a symbol's best name of the previous pass (empty: none past its kind)
    std::string PrevName(int s) const
    {
        const std::map<int, std::vector<CAND>>& mp = mpsymcandPrev.empty() ? mpsymcand : mpsymcandPrev;
        if (const auto it = mp.find(s); it != mp.end())
            if (const CAND* p = PBest(it->second); p && p->conf > CONF_Kind) return !FGuess(p->name) || FTelling(p->name) ? p->name : std::string();
        if (ctx.pworld)
            if (const auto it = ctx.pworld->mpsym.find(s); it != ctx.pworld->mpsym.end() && it->second.second > CONF_Kind)
                return !FGuess(it->second.first) || FTelling(it->second.first) ? it->second.first : std::string();
        return std::string();
    }
    // a symbol's best confidence of the previous pass (0: none)
    int PrevConf(int s) const
    {
        const std::map<int, std::vector<CAND>>& mp = mpsymcandPrev.empty() ? mpsymcand : mpsymcandPrev;
        if (const auto it = mp.find(s); it != mp.end())
            if (const CAND* p = PBest(it->second)) return p->conf;
        return 0;
    }
    // a name that says more than its kind
    static bool FTelling(const std::string& n)
    {
        if (n.empty() || FKindName(Unq(n)) || Unq(n) == "self" || Unq(n) == "this" || Unq(n) == "objects") return false;
        // (a last-resort name "?kind-in-home" / "?param-N-of-f" says nothing to build on)
        if (FGuess(n))
        {
            static const char* const s_akind[] = {"param-", "value-", "object-", "function-", "list-", "flag-", "number-", "element-", "oid-",
                                                  "mode-", "item-", "sum-", "product-", "ratio-", "difference-", "event-of-"};
            const std::string u = Unq(n);
            for (const char* k : s_akind)
                if (u.rfind(k, 0) == 0 && (u.find("-in-") != std::string::npos || u.find("-of-") != std::string::npos)) return false;
        }
        return true;
    }

    void Scan(const NODE& n, int ievent, bool fTopBegin)
    {
        if (!n.fList)
        {
            if (const int s = SymOf(n.atom); s >= 0) { Note(s); setsymNonWord.insert(s); }
            return;
        }
        if (!n.atom.empty())
        {
            for (const NODE& c : n.a) Scan(c, ievent, false);
            return;
        }
        if (n.a.empty()) return;
        const NODE& h = n.a[0];
        const std::string f = h.fList ? std::string() : h.atom;
        // (define S E) / (set! S E)
        if ((f == "define" || f == "set!") && n.a.size() >= 3 && !n.a[1].fList)
        {
            const int s = SymOf(n.a[1].atom);
            const int s0 = s;
            const NODE& e = n.a[2];
            if (s >= 0)
            {
                setsymNonWord.insert(s);
                Note(s);
                if (f == "define" && fTopBegin && s < 64 && FIsHead(e, "lambda"))
                {
                    setevent.insert(s);
                    if (const SPLICEEVENTNAME* pe = PeventOf(s)) Cand(s, pe->name, CONF_Fixed);
                    else Cand(s, "on-event-" + std::to_string(s), CONF_Fixed);
                    // its parameters by role
                    if (e.a.size() >= 2 && e.a[1].fList)
                        for (std::size_t i = 0; i < e.a[1].a.size(); ++i)
                            if (const int p = SymOf(e.a[1].a[i].atom); p >= 0)
                            {
                                if (s == 24 && i == 1) setsymOidKey.insert(p);   // (can-act?'s action: an oid)
                                if (s == 17 && i == 1) setsymJobEvKey.insert(p); // (on-job-event's event: a broadcast number)
                                if (s == 4 && i == 1) setsymEvKey.insert(p);     // (on-event's event word)
                                if (i == 0) Cand(p, p == 990 ? "this" : "self", CONF_Role);
                                else if (const SPLICEEVENTNAME* pe = PeventOf(s); pe && i - 1 < 4 && pe->args[i - 1]) Cand(p, pe->args[i - 1], CONF_Role);
                                else Cand(p, "arg" + std::to_string(i), CONF_Kind);
                            }
                    stkHome.push_back(s);
                    for (std::size_t i = 0; e.a.size() >= 2 && e.a[1].fList && i < e.a[1].a.size(); ++i) NoteHome(SymOf(e.a[1].a[i].atom));
                    for (std::size_t k = 2; k < e.a.size(); ++k) Scan(e.a[k], s, false);
                    stkHome.pop_back();
                    if (s == 4)   // the event words of on-event's case
                        CollectCases(e);
                    return;
                }
                if (f == "define") NoteHome(s);
                if (FIsHead(e, "lambda"))
                {
                    mpfn[s] = &e;
                    if (const std::string nl = NameOfLambda(e); !nl.empty()) Cand(s, nl, CONF_Value);
                    else Cand(s, "fn", CONF_Kind);
                    // (what its last form gives: a guess, its value may not be used)
                    if (e.a.size() >= 3)
                        if (const std::string nr = NameOfExpr(e.a.back()); FTelling(nr) && nr != "fn" && nr != "list" && nr != "vec")
                            Cand(s, Cat({"?get-", nr}), CONF_Guess);
                }
                else if ((FIsHead(e, "<+>") || FIsHead(e, "<->")) && e.a.size() == 3 && e.a[1].atom == n.a[1].atom && FInt(e.a[2].atom)) Cand(s, "counter", CONF_Value);
                else if (const std::string ne = NameOfExpr(e); !ne.empty()) Cand(s, ne, FKindName(ne) ? CONF_Kind : CONF_Value);
                else if (!e.fList && (FInt(e.atom) || e.atom.find('.') != std::string::npos) && std::isdigit(static_cast<unsigned char>(e.atom.back()))) { Cand(s, "n", CONF_Kind); setsymNumber.insert(s); }
                else Cand(s, "value", CONF_Kind - 1);
                // (define S (quote (...))): a constant list (its use says what it holds)
                if (FIsHead(e, "quote") && e.a.size() >= 2 && e.a[1].fList) mpsymQuote[s] = &e.a[1];
                // (set! S (<nth> L i)) / (<car> L): L holds what S is, plural
                if ((FIsHead(e, "<nth>") || FIsHead(e, "<car>") || FIsHead(e, "<get-element>")) && e.a.size() >= 2)
                    if (const int sl = SymOf(e.a[1].atom); sl >= 0)
                        if (const std::string ns = PrevName(s); FTelling(ns)) Cand(sl, Plural(ns), CONF_Use);
                // (set! L (<cons> X L)): L gathers what X is
                if (FIsHead(e, "<cons>") && e.a.size() == 3 && e.a[2].atom == n.a[1].atom)
                    if (const std::string nx = NameFromSymOrExpr(e.a[1], 0); FTelling(nx)) Cand(s, Plural(nx), CONF_Use);
                // (set! S (F ...)): F gives what S holds
                if (e.fList && !e.a.empty() && !e.a[0].fList)
                    if (const int sf = SymOf(e.a[0].atom); sf >= 0 && mpfn.count(sf))
                        if (const std::string ns = PrevName(s); FTelling(ns)) Cand(sf, Cat({"get-", ns}), CONF_Use);
                // (set! F #t / #f) away from its define: a flag the function sets
                if (f == "set!" && !e.fList && (e.atom == "#t" || e.atom == "#f") && !stkHome.empty() && stkHome.back() != s)
                    if (const std::string nh = PrevName(stkHome.back()); FTelling(nh)) Cand(s, Cat({"?", nh, "-flag"}), CONF_Guess);
                // (set! S (<find-object> (<geto> X opt) ...)): the option holds the oid of what S holds
                if ((FIsHead(e, "<find-object>") || FIsHead(e, "<find-object-kind-4>")) && e.a.size() >= 2 && FIsHead(e.a[1], "<geto>") && e.a[1].a.size() >= 3)
                {
                    long opt = 0;
                    const std::string ns = NameFromSymOrExpr(n.a[1], 0);
                    if (FInt(e.a[1].a[2].atom, &opt) && opt > 1232 && FTelling(ns) && ns != "oid" && ns.find("-oid") == std::string::npos)
                        OptCand(int(opt), Cat({ns, "-oid"}), CONF_Use);
                }
                // (set! S (<geto> X opt)): a script-made option takes the symbol's name
                if (FIsHead(e, "<geto>") && e.a.size() >= 3)
                {
                    long opt = 0;
                    if (FInt(e.a[2].atom, &opt) && opt > 1232)
                    {
                        (void)0;
                        const std::string ns = NameFromSymOrExpr(n.a[1], 0);
                        if (!ns.empty() && ns != "obj" && ns != "self" && ns != "this") OptCand(int(opt), ns, CONF_Use);
                    }
                }
                // a symbol holding one object found by its oid
                if ((FIsHead(e, "<find-object>") || FIsHead(e, "<find-object-kind-4>")) && e.a.size() >= 2)
                {
                    long oid = 0;
                    if (FInt(e.a[1].atom, &oid)) { mpsymOid[s] = int(oid); NoteOid(e.a[1], PrevName(s)); }
                }
            }
            const bool fFn = s0 >= 0 && FIsHead(n.a[2], "lambda");
            if (fFn) stkHome.push_back(s0);
            for (std::size_t k = 2; k < n.a.size(); ++k) Scan(n.a[k], ievent, false);
            if (fFn) stkHome.pop_back();
            return;
        }
        if (f == "lambda" && n.a.size() >= 2)
        {
            if (n.a[1].fList)
                for (std::size_t i = 0; i < n.a[1].a.size(); ++i)
                    if (const int s = SymOf(n.a[1].a[i].atom); s >= 0)
                    {
                        Note(s);
                        setsymNonWord.insert(s);
                        Cand(s, "arg", CONF_Kind - 1);
                        NoteHome(s);
                        if (!mpsymParam.count(s)) mpsymParam[s] = int(i) + 1;
                    }
            for (std::size_t k = 2; k < n.a.size(); ++k) Scan(n.a[k], ievent, false);
            return;
        }
        if (f == "assert" && n.a.size() >= 3)
        {
            Scan(n.a[1], ievent, false);
            // the message: a quoted list of words (symbols: their names are not in the game)
            const NODE& m = n.a[2];
            if (FIsHead(m, "quote") && m.a.size() >= 2 && m.a[1].fList)
                for (const NODE& w : m.a[1].a)
                    if (const int s = SymOf(w.atom); s >= 0) { Note(s); setsymWordOnly.insert(s); }
            for (std::size_t k = 3; k < n.a.size(); ++k) Scan(n.a[k], ievent, false);
            return;
        }
        if (f == "case" && n.a.size() >= 3 && !n.a[1].fList && setsymOidKey.count(SymOf(n.a[1].atom)))
            for (std::size_t k = 2; k < n.a.size(); ++k)
                if (n.a[k].fList && !n.a[k].a.empty() && n.a[k].a[0].fList)
                    for (const NODE& v : n.a[k].a[0].a) NoteOid(v, "action");
        if (f == "case" && n.a.size() >= 3 && !n.a[1].fList && setsymJobEvKey.count(SymOf(n.a[1].atom)))
            for (std::size_t k = 2; k < n.a.size(); ++k)
                if (n.a[k].fList && !n.a[k].a.empty() && n.a[k].a[0].fList)
                    for (const NODE& v : n.a[k].a[0].a)
                    {
                        long e = 0;
                        if (FInt(v.atom, &e) && SzJobEvent(int(e))) mpLegend[std::string("%") + SzJobEvent(int(e))] = v.atom;
                    }
        if (f == "case" && n.a.size() >= 3 && !n.a[1].fList && setsymEvKey.count(SymOf(n.a[1].atom)))
            for (std::size_t k = 2; k < n.a.size(); ++k)
                if (n.a[k].fList && !n.a[k].a.empty() && n.a[k].a[0].fList)
                {
                    std::string nb;
                    for (std::size_t j = 1; j < n.a[k].a.size() && nb.empty(); ++j) nb = NameOfStatement(n.a[k].a[j], 0);
                    for (const NODE& v : n.a[k].a[0].a)
                    {
                        long w = 0;
                        if (!FInt(v.atom, &w)) continue;
                        setword.insert(int(w));
                        if (!nb.empty()) WordCand(int(w), nb, CONF_Value);
                    }
                }
        if (f == "quote")
        {
            for (std::size_t k = 1; k < n.a.size(); ++k) Scan(n.a[k], ievent, false);
            return;
        }
        // (begin ...) at a module's top
        if (f == "begin")
        {
            for (std::size_t k = 1; k < n.a.size(); ++k) Scan(n.a[k], ievent, fTopBegin);
            return;
        }
        // options
        if ((f == "<geto>" || f == "<seto!>" || f == "<addo>" || f == "<ensureo>") && n.a.size() >= 3)
        {
            long opt = 0;
            if (FInt(n.a[2].atom, &opt))
            {
                (opt > 1232 ? setoptScript : setoptEngine).insert(int(opt));
                if (opt > 1232 && f != "<geto>" && n.a.size() >= 4)
                {
                    std::string nv = FIsHead(n.a[3], "lambda") ? NameOfLambda(n.a[3]) : NameFromSymOrExpr(n.a[3], 0);
                    if (FIsHead(n.a[3], "lambda") && nv.empty()) nv = "fn";
                    // (<seto!> X opt (<+> (<geto> X opt) 1)): a counter
                    if ((FIsHead(n.a[3], "<+>") || FIsHead(n.a[3], "<->")) && n.a[3].a.size() == 3 && FIsHead(n.a[3].a[1], "<geto>") &&
                        n.a[3].a[1].a.size() >= 3 && n.a[3].a[1].a[2].atom == n.a[2].atom)
                        nv = "count";
                    else if (nv == "elapsed" || nv == "time") nv = "time";
                    OptCand(int(opt), nv, nv == "fn" || nv == "obj" || nv == "list" || nv == "flag" ? CONF_Kind : CONF_Value);
                }
                if (f == "<seto!>" && FOidArg(int(opt), -1) && n.a.size() >= 4)
                {
                    const std::string nh = NameFromSymOrExpr(n.a[1], 0);
                    NoteOid(n.a[3], opt == 478 || opt == 481 ? Cat({FTelling(nh) ? nh : std::string("state-machine"), "-state"}) : OptName(int(opt)));
                }
            }
            // (<geto> X opt) where X is a symbol: an object
            if (const int s = SymOf(n.a[1].atom); s >= 0)
            {
                Cand(s, "obj", CONF_Kind);
                if (FInt(n.a[2].atom, &opt) && opt <= 1232) mpsymOpts[s].insert(int(opt));
            }
            if (FInt(n.a[2].atom, &opt) && opt > 1232 && f != "<geto>" && !mpoptHome.count(int(opt))) mpoptHome[int(opt)] = Home();
            // (<addo> X .opt F): the function F is what the option is for
            if (f != "<geto>" && n.a.size() >= 4 && FInt(n.a[2].atom, &opt))
                if (const int sf = SymOf(n.a[3].atom); sf >= 0 && mpfn.count(sf))
                {
                    std::string no = opt > 1232 ? ScriptOptName(int(opt)) : OptName(int(opt));
                    if (!no.empty() && no.back() == '?') no.pop_back();
                    if (FTelling(no) && no != "fn") Cand(sf, no, CONF_Use);
                }
        }
        if ((f == "<find-object>" || f == "<find-object-kind-4>" || f == "#bif:113" || f == "<find-object-113 (0x1241E8)>") && n.a.size() >= 2)
        {
            NoteOid(n.a[1]);
            if (const int s = SymOf(n.a[1].atom); s >= 0) Cand(s, "oid", CONF_Use);   // (a symbol holding an oid)
        }
        // (<find-objects> CID X): the objects of a class (round 3: not an oid)
        if (f == "<find-objects>" && n.a.size() >= 2)
        {
            long cid = 0;
            if (!n.a[1].fList && FInt(n.a[1].atom, &cid)) setcls.insert(int(cid));
            if (const int s = SymOf(n.a[1].atom); s >= 0) Cand(s, "class", CONF_Use);
        }
        // (<seto!> X .opt S): S holds what the option takes
        if (f == "<seto!>" && n.a.size() >= 4 && !n.a[3].fList)
            if (const int s = SymOf(n.a[3].atom); s >= 0)
            {
                long opt = 0;
                if (FInt(n.a[2].atom, &opt))
                    if (const std::string on = OptName(int(opt)); !on.empty() && on.back() != '?') Cand(s, Cat({"new-", on}), CONF_Use);
            }
        if (f == "<find-objects-of-names>" && n.a.size() >= 2 && FIsHead(n.a[1], "quote") && n.a[1].a.size() >= 2 && n.a[1].a[1].fList)
            for (const NODE& o : n.a[1].a[1].a) NoteOid(o);
        if (!f.empty() && (f[0] == '<' || f.rfind("#bif:", 0) == 0)) setbif.insert(BifOfRaw(f));
        // a method call ((<geto> X opt) args): oid arguments; the arguments' names
        if (h.fList && FIsHead(h, "<geto>") && h.a.size() >= 3)
        {
            long opt = 0;
            if (FInt(h.a[2].atom, &opt))
            {
                for (std::size_t i = 1; i < n.a.size(); ++i)
                {
                    if (FOidArg(int(opt), int(i) - 1))
                        NoteOid(n.a[i], opt <= 1232 && SzMethodArg(int(opt), int(i) - 1) ? std::string(SzMethodArg(int(opt), int(i) - 1)) : std::string());
                    if (const char* szArg = opt <= 1232 ? SzMethodArg(int(opt), int(i) - 1) : nullptr)
                    {
                        if (const int sa = SymOf(n.a[i].atom); sa >= 0 && sa != 1002 && sa != 990) Cand(sa, szArg, CONF_Arg);
                        long so = 0;
                        if (FIsHead(n.a[i], "<geto>") && n.a[i].a.size() >= 3 && FInt(n.a[i].a[2].atom, &so) && so > 1232) OptCand(int(so), szArg, CONF_Arg);
                    }
                }
                // a script-made option called in an event handler: what the handler is for
                if (opt > 1232 && ievent >= 0)
                    if (const SPLICEEVENTNAME* pe = PeventOf(ievent)) OptCand(int(opt), std::string(pe->name) + "-hook", CONF_Use - 1);
            }
        }
        // an engine option of what a symbol / script-made option holds: what kind of object it is
        {
            const NODE* pholder = nullptr;   // (the expression the engine option is taken of)
            long eopt = 0;
            if ((f == "<seto!>" || f == "<geto>") && n.a.size() >= 3 && FInt(n.a[2].atom, &eopt)) pholder = &n.a[1];
            if (pholder && eopt <= 1232)
                if (const char* szk = SzObjectKindOfOption(int(eopt)))
                {
                    if (const int sh = SymOf(pholder->atom); sh >= 0 && sh != 1002 && sh != 990) Cand(sh, szk, CONF_Use);
                    long so = 0;
                    if (FIsHead(*pholder, "<geto>") && pholder->a.size() >= 3 && FInt(pholder->a[2].atom, &so) && so > 1232) OptCand(int(so), szk, CONF_Use);
                }
            // (<seto!> Y .eopt (<geto> X so)): the value flows into the engine option
            long so = 0;
            if (f == "<seto!>" && n.a.size() >= 4 && eopt > 0 && eopt <= 1232 && FIsHead(n.a[3], "<geto>") && n.a[3].a.size() >= 3 &&
                FInt(n.a[3].a[2].atom, &so) && so > 1232)
                if (std::string on = OptName(int(eopt)); !on.empty()) { if (on.back() == '?') on.pop_back(); OptCand(int(so), on, CONF_Arg); }
        }
        // (if (<geto> X so) ...): a flag
        if ((f == "if" || f == "and" || f == "or" || f == "<not>") && n.a.size() >= 2)
            for (std::size_t k = 1; k < (f == "if" ? std::size_t(2) : n.a.size()); ++k)
            {
                long so = 0;
                if (FIsHead(n.a[k], "<geto>") && n.a[k].a.size() >= 3 && FInt(n.a[k].a[2].atom, &so) && so > 1232) OptCand(int(so), "flag", CONF_Kind);
            }
        // (<find-object> (<geto> X opt) ...): the option holds an oid
        if ((f == "<find-object>" || f == "<find-object-kind-4>" || f == "<find-objects>") && n.a.size() >= 2 && FIsHead(n.a[1], "<geto>") &&
            n.a[1].a.size() >= 3)
        {
            long so = 0;
            if (FInt(n.a[1].a[2].atom, &so) && so > 1232) OptCand(int(so), "oid", CONF_Kind);
        }
        // (case (<geto> X opt) ...): the option holds one of a few values
        if (f == "case" && n.a.size() >= 3 && FIsHead(n.a[1], "<geto>") && n.a[1].a.size() >= 3)
        {
            long so = 0;
            if (FInt(n.a[1].a[2].atom, &so) && so > 1232) OptCand(int(so), "mode", CONF_Kind);
        }
        // (for-each / map / filter (lambda (x) ...) L): x is one of L
        if ((f == "<for-each>" || f == "<map>" || f == "<filter>") && n.a.size() >= 3 && FIsHead(n.a[1], "lambda") && n.a[1].a.size() >= 2 &&
            n.a[1].a[1].fList && n.a[1].a[1].a.size() == 1)
        {
            const std::string nl = NameFromSymOrExpr(n.a[2], 0);
            if (const int p = SymOf(n.a[1].a[1].a[0].atom); p >= 0 && !nl.empty()) Cand(p, Singular(nl), CONF_Arg);
        }
        // (for-each (lambda (x) ...) L): L holds what x is
        if ((f == "<for-each>" || f == "<map>" || f == "<filter>") && n.a.size() >= 3 && FIsHead(n.a[1], "lambda") && n.a[1].a.size() >= 2 &&
            n.a[1].a[1].fList && n.a[1].a[1].a.size() == 1)
            if (const int sl = SymOf(n.a[2].atom); sl >= 0)
                if (const std::string np = PrevName(SymOf(n.a[1].a[1].a[0].atom)); FTelling(np) && np != Singular(PrevName(sl))) Cand(sl, Plural(np), CONF_Arg);
        // (<member?> X L): L holds what X is
        if (f == "<member?>" && n.a.size() >= 3)
            if (const int sl = SymOf(n.a[2].atom); sl >= 0)
            {
                std::string nx = NameFromSymOrExpr(n.a[1], 0);
                if (FIsHead(n.a[1], "<geto>") && n.a[1].a.size() >= 3 && n.a[1].a[2].atom == "28")
                {
                    const std::string nw = NameFromSymOrExpr(n.a[1].a[1], 0);
                    nx = FTelling(nw) ? Cat({nw, "-oid"}) : std::string("oid");
                }
                if (FTelling(nx) || nx == "oid") Cand(sl, Plural(nx), CONF_Use);
            }
        // (<find-objects-of-names> L ...): L's numbers are oids
        if (f == "<find-objects-of-names>" && n.a.size() >= 2)
            if (const int sl = SymOf(n.a[1].atom); sl >= 0)
                if (const auto it = mpsymQuote.find(sl); it != mpsymQuote.end())
                {
                    setquoteOid.insert(it->second);
                    std::string no;
                    const std::string nl = PrevName(sl);
                    for (const NODE& o : it->second->a)
                    {
                        NoteOid(o, FTelling(nl) ? Singular(nl) : std::string());
                        long oid = 0;
                        if (no.empty() && FInt(o.atom, &oid)) no = ObjName(int(oid));
                    }
                    Cand(sl, no.empty() ? std::string("object-oids") : Cat({Plural(no), "-oids"}), CONF_Use);
                }
        // (<schedule-callback> D F): D is a delay, F what runs then
        if (f == "<schedule-callback>" && n.a.size() >= 3)
        {
            if (const int sd = SymOf(n.a[1].atom); sd >= 0) Cand(sd, "delay", CONF_Use);
            if (const int sf = SymOf(n.a[2].atom); sf >= 0 && mpfn.count(sf) && !stkHome.empty())
                if (const std::string nh = PrevName(stkHome.back()); FTelling(nh)) Cand(sf, Cat({"?after-", nh}), CONF_Guess);
        }
        // (F a b): an argument that says nothing takes F's parameter's name
        if (const int sf = SymOf(f); sf >= 0)
            if (const auto it = mpfn.find(sf); it != mpfn.end() && it->second->a.size() >= 2 && it->second->a[1].fList)
            {
                const std::vector<NODE>& ap = it->second->a[1].a;
                for (std::size_t i = 0; i < ap.size() && i + 1 < n.a.size(); ++i)
                    if (const int sa = SymOf(n.a[i + 1].atom); sa >= 0 && sa != 1002 && sa != 990 && PrevConf(sa) <= CONF_Arg - 1)
                        if (const std::string np = PrevName(SymOf(ap[i].atom)); FTelling(np)) Cand(sa, np, CONF_Arg - 1);
            }
        // (round 3) a constant number in arithmetic with a named value: its scale / offset / limit (a guess)
        {
            static const std::map<std::string, const char*> s_mpsuf = {{"<*>", "-scale"}, {"</>", "-divisor"}, {"<+>", "-offset"}, {"<->", "-offset"},
                                                                       {"<min>", "-limit"}, {"<max>", "-limit"}};
            if (const auto it = s_mpsuf.find(f); it != s_mpsuf.end() && n.a.size() == 3)
                for (int k = 1; k <= 2; ++k)
                    if (const int sn = SymOf(n.a[std::size_t(k)].atom); sn >= 0 && setsymNumber.count(sn) && PrevConf(sn) <= CONF_Guess)
                        if (const std::string no = NameFromSymOrExpr(n.a[std::size_t(3 - k)], 0); FTelling(no) && Unq(no).size() <= 30)
                            Cand(sn, Cat({"?", no, f == "</>" && k == 1 ? "-dividend" : it->second}), CONF_Guess);
        }
        // (if (<geto> X so) A ...): a flag option turns A on (a guess: "?A-enabled")
        if (f == "if" && n.a.size() >= 3 && FIsHead(n.a[1], "<geto>") && n.a[1].a.size() >= 3)
        {
            long so = 0;
            if (FInt(n.a[1].a[2].atom, &so) && so > 1232)
                if (const std::string ns = NameOfStatement(n.a[2], 0); FTelling(ns) && Unq(ns).size() <= 32) OptCand(int(so), Cat({"?", ns, "-enabled"}), CONF_Guess);
        }
        // comparisons with a number that says nothing: its limit
        if ((f == "<<>" || f == "<>>" || f == "<<=>" || f == "<>=>") && n.a.size() == 3)
            for (int k = 1; k <= 2; ++k)
                if (const int sn = SymOf(n.a[std::size_t(k)].atom); sn >= 0 && PrevConf(sn) <= CONF_Guess)
                    if (const std::string no = NameFromSymOrExpr(n.a[std::size_t(3 - k)], 0); FTelling(no)) Cand(sn, Cat({"?", no, "-limit"}), CONF_Guess);
        // (F a b): F's parameters from the arguments
        if (const int sf = SymOf(f); sf >= 0)
            if (const auto it = mpfn.find(sf); it != mpfn.end() && it->second->a.size() >= 2 && it->second->a[1].fList)
            {
                const std::vector<NODE>& ap = it->second->a[1].a;
                for (std::size_t i = 0; i < ap.size() && i + 1 < n.a.size(); ++i)
                    if (const int p = SymOf(ap[i].atom); p >= 0)
                    {
                        const std::string na = NameFromSymOrExpr(n.a[i + 1], 0);
                        if (!na.empty() && na != "self" && na != "this") Cand(p, na, FKindName(Unq(na)) ? CONF_Kind : CONF_Arg);
                        else if (na == "self" || na == "this") Cand(p, "obj", CONF_Kind);
                        // (a script-made option passed: it takes the parameter's name)
                        long so = 0;
                        if (FIsHead(n.a[i + 1], "<geto>") && n.a[i + 1].a.size() >= 3 && FInt(n.a[i + 1].a[2].atom, &so) && so > 1232)
                            if (const std::string np = NameFromSymOrExpr(ap[i], 0); !np.empty() && !FKindName(np)) OptCand(int(so), np, CONF_Arg);
                    }
            }
        for (const NODE& c : n.a) Scan(c, ievent, false);
    }
    void NoteOid(const NODE& o, const std::string& hint = std::string())
    {
        long oid = 0;
        if (!o.fList && FInt(o.atom, &oid) && oid > 0)
        {
            setoid.insert(int(oid));
            if (FTelling(hint) && !mpoidHint.count(int(oid))) mpoidHint[int(oid)] = hint;
        }
    }
    std::map<int, std::string> mpoidHint;   // (round 3) an oid the world does not name: what its use says (the last resort)
    void CollectCases(const NODE& lam)
    {
        // (case event ((v ...) ...) ...)
        std::vector<const NODE*> stack{&lam};
        while (!stack.empty())
        {
            const NODE* p = stack.back();
            stack.pop_back();
            if (!p->fList) continue;
            if (FIsHead(*p, "case"))
                for (std::size_t k = 2; k < p->a.size(); ++k)
                    if (p->a[k].fList && !p->a[k].a.empty() && p->a[k].a[0].fList)
                        for (const NODE& v : p->a[k].a[0].a) setcase.insert(v.atom);
            for (const NODE& c : p->a) stack.push_back(&c);
        }
    }

    // the group's name (the title's "group N name"), as a name
    std::string GroupWord() const
    {
        std::string t = ctx.title;
        if (t.rfind("group ", 0) == 0)
        {
            const std::size_t sp = t.find(' ', 6);
            t = sp == std::string::npos ? std::string() : t.substr(sp + 1);
        }
        return Slug(t);
    }
    // where a symbol / option lives, as a name (its home define's, or the module's group)
    std::string HomeWord(int home) const
    {
        if (home >= 0)
        {
            if (const auto it = mpsymBase.find(home); it != mpsymBase.end() && !it->second.first.empty()) return Unq(it->second.first);
            if (const std::string n = PrevName(home); !n.empty()) return Unq(n);
        }
        const std::string g = GroupWord();
        const int imod = home < 0 ? -1 - home : 0;
        return g.empty() ? "module-" + std::to_string(imod) : g + (imod ? "-module-" + std::to_string(imod) : std::string());
    }
    static std::string KindWord(const std::string& k)
    {
        static const std::map<std::string, std::string> s_mp = {{"fn", "function"}, {"obj", "object"}, {"n", "number"}, {"arg", "param"},
                                                                {"", "value"}, {"item", "element"}, {"rest", "rest-of-list"}};
        const auto it = s_mp.find(k);
        return it == s_mp.end() ? k : it->second;
    }
    // the last resort: "?kind-in-home" (a function's parameter: "?param-N-of-function")
    std::string FallbackName(int s, const std::string& kind) const
    {
        std::string k = KindWord(Unq(kind));
        if (k.size() > 3 && k.compare(0, 3, "arg") == 0 && std::isdigit(static_cast<unsigned char>(k[3]))) k = "param-" + k.substr(3);
        const auto ith = mpsymHome.find(s);
        const int home = ith == mpsymHome.end() ? -1 : ith->second;
        if (const auto ip = mpsymParam.find(s); ip != mpsymParam.end() && (k == "param" || k == "value" || k == "object"))
            return "?" + (k == "param" ? std::string("param") : k) + "-" + std::to_string(ip->second) + "-of-" + HomeWord(home);
        return "?" + k + "-in-" + HomeWord(home);
    }

    // ---- the names chosen
    void Choose()
    {
        std::set<std::string> setUsed;
        auto reserve = [&](std::string s, const std::string& unique) {
            if (s.empty() || FReserved(s) || SymOf(s) >= 0) s = (s.empty() ? std::string("v") : s) + "-v";
            if (!setUsed.count(s)) { setUsed.insert(s); return s; }
            std::string t = s + "-" + unique;
            for (int k = 2; setUsed.count(t); ++k) t = s + "-" + unique + "-" + std::to_string(k);
            setUsed.insert(t);
            return t;
        };
        // the fixed names first, then the events, then the others by their first appearance
        std::vector<std::pair<int, int>> aorder;   // (priority, sym)
        for (const auto& [s, seq] : mpsymFirst) aorder.push_back({seq, s});
        std::sort(aorder.begin(), aorder.end());
        std::vector<int> asym;
        for (const SPLICEFIXEDSYM& f : c_afixed)
            if (mpsymFirst.count(f.symid)) asym.push_back(f.symid);
        for (int s : setevent) asym.push_back(s);
        for (const auto& [seq, s] : aorder)
            if (std::find(asym.begin(), asym.end(), s) == asym.end()) asym.push_back(s);
        for (int s : asym)
        {
            std::string name;
            int conf = 0;
            for (const SPLICEFIXEDSYM& f : c_afixed)
                if (f.symid == s) { name = f.name; conf = CONF_Fixed; }
            if (name.empty())
                if (const auto it = mpsymcand.find(s); it != mpsymcand.end() && !it->second.empty())
                {
                    const CAND* pbest = &it->second[0];
                    for (const CAND& c : it->second)
                        if (c.conf > pbest->conf || (c.conf == pbest->conf && c.count > pbest->count)) pbest = &c;
                    name = pbest->name;
                    conf = pbest->conf;
                }
            // (the other groups' scripts of the world: a symbol is one name everywhere)
            if (ctx.pworld && conf < CONF_Value)
                if (const auto it = ctx.pworld->mpsym.find(s); it != ctx.pworld->mpsym.end() && it->second.second > conf && it->second.second > CONF_Kind)
                {
                    name = it->second.first;
                    conf = it->second.second;
                }
            const bool fWord = setsymWordOnly.count(s) && !setsymNonWord.count(s);
            // (round 3: what still only says its kind: the class its options say, else "?kind-in-home")
            if (!fWord && conf <= CONF_Kind)
            {
                if (const auto it = mpsymOpts.find(s); it != mpsymOpts.end())
                    if (const std::string k = SpliceKindOfOptions(it->second); !k.empty()) { name = k; conf = FGuess(k) ? CONF_Guess : CONF_Use - 1; }
                if (conf <= CONF_Kind) { name = FallbackName(s, name); conf = CONF_Guess; }
            }
            if (!name.empty()) mpsymBase[s] = {name, conf};
            ++st.csym;
            if (fWord) ++st.csymWord;
            if (name.empty()) continue;   // (stays symN: an assert's word)
            if (conf <= CONF_Kind)
            {
                if (!fWord) ++st.csymKind;
                name = reserve(name + "-" + std::to_string(s), std::to_string(s));
            }
            else
            {
                if (!fWord) ++(FGuess(name) ? st.csymGuess : st.csymNamed);
                name = reserve(name, std::to_string(s));
            }
            mpsymConf[s] = conf;
            mpsym[s] = name;
            mpLegend[name] = "sym" + std::to_string(s);
        }
        for (int s : setevent)
        {
            ++st.cevent;
            if (PeventOf(s)) ++st.ceventNamed;
        }
        // options: the engine's by the table, the scripts' own by what they hold
        for (int o : setoptEngine)
        {
            ++st.coptEngine;
            if (const std::string n = OptName(o); !n.empty())
            {
                ++(FGuess(n) ? st.coptEngineGuess : st.coptEngineNamed);
                mpopt[o] = "." + n;
                mpLegend["." + n] = std::to_string(o);
                setUsed.insert("." + n);
            }
        }
        for (int o : setoptScript)
        {
            ++st.coptScript;
            std::string n;
            int conf = 0;
            if (const auto it = mpoptcand.find(o); it != mpoptcand.end() && !it->second.empty())
            {
                const CAND* pbest = &it->second[0];
                for (const CAND& c : it->second)
                    if (c.conf > pbest->conf || (c.conf == pbest->conf && c.count > pbest->count)) pbest = &c;
                n = pbest->name;
                conf = pbest->conf;
            }
            if (ctx.pworld && conf < CONF_Value)
                if (const auto it = ctx.pworld->mpopt.find(o); it != ctx.pworld->mpopt.end() && it->second.second > conf && it->second.second > CONF_Kind)
                {
                    n = it->second.first;
                    conf = it->second.second;
                }
            // (round 3: only its kind, or nothing: "?kind-of-home", home: where it is first added / set)
            if (conf <= CONF_Kind)
            {
                const auto ith = mpoptHome.find(o);
                std::string k = KindWord(Unq(n));
                if (k == "list" || k == "mode" || k == "oid" || k == "flag" || k == "function" || k == "object" || k == "value" || k == "number")
                    ;
                else k = "value";
                n = "?" + k + "-of-" + HomeWord(ith == mpoptHome.end() ? -1 : ith->second);
                conf = CONF_Guess;
            }
            mpoptBase[o] = {n, conf};
            if (conf <= CONF_Kind) n += "-" + std::to_string(o);
            else ++(FGuess(n) ? st.coptScriptGuess : st.coptScriptNamed);
            mpoptConf[o] = conf;
            // (not one of the engine's names: those map back by the table)
            std::string t = "." + n;
            if (MpOptByName().count(n) || setUsed.count(t)) t = "." + n + "-" + std::to_string(o);
            setUsed.insert(t);
            mpopt[o] = t;
            mpLegend[t] = std::to_string(o);
        }
        // objects: their world names; two objects of one name get their oids
        std::map<std::string, std::vector<int>> mpnameoids;
        for (int oid : setoid)
        {
            ++st.coid;
            std::string n = ObjName(oid);
            if (n.empty())
            {
                // (round 3: no object of the game has it (or not one the place loads): what its use says, a guess)
                if (std::getenv("SLY2N_NAMESDEBUG")) std::fprintf(stderr, "names: oid %d has no name (%s)\n", oid, ctx.title.c_str());   // (debug)
                const auto it = mpoidHint.find(oid);
                n = "?" + Slug(it == mpoidHint.end() ? std::string("object") : Unq(it->second));
                if (n.find(std::to_string(oid)) == std::string::npos) n += "-" + std::to_string(oid);
                ++st.coidGuess;
            }
            else ++st.coidNamed;
            mpnameoids[n].push_back(oid);
        }
        for (const auto& [n, aoid] : mpnameoids)
            for (int oid : aoid)
            {
                std::string t = "$" + n;
                if (aoid.size() > 1 && n.find(std::to_string(oid)) == std::string::npos) t += "-" + std::to_string(oid);
                for (int k = 2; setUsed.count(t); ++k) t = "$" + n + "-" + std::to_string(oid) + (k > 2 ? "-" + std::to_string(k) : "");
                setUsed.insert(t);
                mpoid[oid] = t;
                mpLegend[t] = std::to_string(oid);
            }
        // classes: &name
        for (int c : setcls)
        {
            ++st.ccls;
            const char* sz = SzSpliceClassName(c);
            if (!sz) continue;
            ++st.cclsNamed;
            mpcls[c] = std::string("&") + sz;
            mpLegend[mpcls[c]] = std::to_string(c);
        }
        for (int b : setbif)
            if (b >= 0)
            {
                ++st.cbif;
                const std::string r = ReadableBif(b);
                if (r.rfind("#bif:", 0) != 0 && r.find(' ') == std::string::npos && r.rfind("<bif-", 0) != 0) ++st.cbifNamed;
            }
        // the event words: what their clauses do (the world's handlers' first), else where they come from
        std::map<std::string, std::vector<int>> mpnamewords;
        for (int w : setword)
        {
            ++st.cmsg;
            std::string n;
            if (ctx.pworld)
                if (const auto it = ctx.pworld->mpword.find(w); it != ctx.pworld->mpword.end()) n = it->second.first;
            if (n.empty())
                if (const auto it = mpwordcand.find(w); it != mpwordcand.end())
                    if (const CAND* p = PBest(it->second)) n = p->name;
            if (n.empty() && ctx.pfnEventWord) n = Slug(ctx.pfnEventWord(w));
            // (round 3: nothing says what it is: the group whose handler takes it)
            if (n.empty()) n = "?event-of-" + (GroupWord().empty() ? std::string("module") : GroupWord());
            mpwordConf[w] = FGuess(n) ? CONF_Guess : CONF_Value;
            ++(FGuess(n) ? st.cmsgGuess : st.cmsgNamed);
            mpnamewords[n].push_back(w);
        }
        for (const auto& [n, aw] : mpnamewords)
            for (int w : aw)
            {
                std::string t = "~" + n;
                if (aw.size() > 1) t += "-" + std::to_string(w);
                for (int k = 2; setUsed.count(t); ++k) t = "~" + n + "-" + std::to_string(w) + "-" + std::to_string(k);
                setUsed.insert(t);
                mpword[w] = t;
                mpLegend[t] = std::to_string(w);
            }
    }

    // ---- printing
    std::string Atom(const std::string& a) const
    {
        if (const int s = SymOf(a); s >= 0)
            if (const auto it = mpsym.find(s); it != mpsym.end()) return it->second;
        if (const int b = BifOfRaw(a); b >= 0)
        {
            const std::string r = ReadableBif(b);
            return r;
        }
        return a;
    }
    // the readable form of a node (contexts: option positions, oid positions)
    NODE Rename(const NODE& n) const
    {
        NODE r;
        r.fList = n.fList;
        r.line = n.line;
        r.atom = n.fList ? n.atom : Atom(n.atom);
        if (!n.fList) return r;
        for (const NODE& c : n.a) r.a.push_back(Rename(c));
        if (!n.atom.empty() || n.a.empty()) return r;
        const std::string f = n.a[0].fList ? std::string() : n.a[0].atom;
        auto optAt = [&](std::size_t i) {
            long o = 0;
            if (i < n.a.size() && !n.a[i].fList && FInt(n.a[i].atom, &o))
                if (const auto it = mpopt.find(int(o)); it != mpopt.end()) r.a[i].atom = it->second;
        };
        auto oidAt = [&](NODE& rr, const NODE& nn) {
            long o = 0;
            if (!nn.fList && FInt(nn.atom, &o))
                if (const auto it = mpoid.find(int(o)); it != mpoid.end()) rr.atom = it->second;
        };
        if (f == "<geto>" || f == "<seto!>" || f == "<addo>" || f == "<ensureo>")
        {
            optAt(2);
            long o = 0;
            if (f == "<seto!>" && n.a.size() >= 4 && FInt(n.a[2].atom, &o) && FOidArg(int(o), -1)) oidAt(r.a[3], n.a[3]);
        }
        if ((f == "<find-object>" || f == "<find-object-kind-4>" || f == "#bif:113" || f == "<find-object-113 (0x1241E8)>") && n.a.size() >= 2)
            oidAt(r.a[1], n.a[1]);
        if (f == "<find-objects>" && n.a.size() >= 2)
        {
            long c = 0;
            if (!n.a[1].fList && FInt(n.a[1].atom, &c))
                if (const auto it = mpcls.find(int(c)); it != mpcls.end()) r.a[1].atom = it->second;
        }
        // (a constant list find-objects-of-names takes: its oids)
        if (f == "quote" && n.a.size() >= 2 && setquoteOid.count(&n.a[1]))
            for (std::size_t k = 0; k < n.a[1].a.size(); ++k) oidAt(r.a[1].a[k], n.a[1].a[k]);
        if (f == "<find-objects-of-names>" && n.a.size() >= 2 && FIsHead(n.a[1], "quote") && n.a[1].a.size() >= 2 && n.a[1].a[1].fList)
            for (std::size_t k = 0; k < n.a[1].a[1].a.size(); ++k) oidAt(r.a[1].a[1].a[k], n.a[1].a[1].a[k]);
        if (f == "case" && n.a.size() >= 3 && !n.a[1].fList && setsymOidKey.count(SymOf(n.a[1].atom)))
            for (std::size_t k = 2; k < n.a.size(); ++k)
                if (n.a[k].fList && !n.a[k].a.empty() && n.a[k].a[0].fList)
                    for (std::size_t j = 0; j < n.a[k].a[0].a.size(); ++j) oidAt(r.a[k].a[0].a[j], n.a[k].a[0].a[j]);
        if (f == "case" && n.a.size() >= 3 && !n.a[1].fList && setsymJobEvKey.count(SymOf(n.a[1].atom)))
            for (std::size_t k = 2; k < n.a.size(); ++k)
                if (n.a[k].fList && !n.a[k].a.empty() && n.a[k].a[0].fList)
                    for (std::size_t j = 0; j < n.a[k].a[0].a.size(); ++j)
                    {
                        long v = 0;
                        if (FInt(n.a[k].a[0].a[j].atom, &v))
                            if (const char* sz = SzJobEvent(int(v))) r.a[k].a[0].a[j].atom = std::string("%") + sz;
                    }
        if (f == "case" && n.a.size() >= 3 && !n.a[1].fList && setsymEvKey.count(SymOf(n.a[1].atom)))
            for (std::size_t k = 2; k < n.a.size(); ++k)
                if (n.a[k].fList && !n.a[k].a.empty() && n.a[k].a[0].fList)
                    for (std::size_t j = 0; j < n.a[k].a[0].a.size(); ++j)
                    {
                        long v = 0;
                        if (FInt(n.a[k].a[0].a[j].atom, &v))
                            if (const auto it = mpword.find(int(v)); it != mpword.end()) r.a[k].a[0].a[j].atom = it->second;
                    }
        if (n.a[0].fList && FIsHead(n.a[0], "<geto>") && n.a[0].a.size() >= 3)
        {
            long o = 0;
            if (FInt(n.a[0].a[2].atom, &o))
                for (std::size_t i = 1; i < n.a.size(); ++i)
                    if (FOidArg(int(o), int(i) - 1)) oidAt(r.a[i], n.a[i]);
        }
        return r;
    }
};

// ---- the readable layout: Lisp style, special forms with their body indented by 2
thread_local bool t_fAssertNotes = false;   // (a list holding an assert is never flat: the assert gets its note line)
thread_local int t_cassert = 0;
std::size_t CchFlat(const NODE& n)
{
    if (!n.fList) return n.atom.size();
    std::size_t c = n.atom.size() + 2;
    for (std::size_t i = 0; i < n.a.size(); ++i) c += CchFlat(n.a[i]) + (i ? 1 : 0) + (t_fAssertNotes && FIsHead(n.a[i], "assert") ? 10000 : 0);
    return c;
}
void PrintFlat(const NODE& n, std::string& out);
// an assert's generated message: what its condition expects, in words (the original's words are lost)
std::string Words(const NODE& c, bool fNeg, int depth = 0);
std::string FlatText(const NODE& n)
{
    std::string s;
    PrintFlat(n, s);
    if (s.size() > 70) s = s.substr(0, 67) + "...";
    return s;
}
std::string Words(const NODE& c, bool fNeg, int depth)
{
    if (depth > 6) return FlatText(c);
    if (!c.fList)
    {
        if (c.atom == "#t") return fNeg ? "never (it always stops here)" : "always";
        if (c.atom == "#f") return fNeg ? "always" : "never (it always stops here)";
        return c.atom + (fNeg ? " is false" : " is true");
    }
    if (!c.atom.empty() || c.a.empty()) return FlatText(c);
    const NODE& h = c.a[0];
    const std::string f = h.fList ? std::string() : h.atom;
    auto arg = [&](std::size_t i) { return i < c.a.size() ? FlatText(c.a[i]) : std::string("?"); };
    if (f == "<not>" && c.a.size() == 2) return Words(c.a[1], !fNeg, depth + 1);
    if ((f == "and" || f == "or") && c.a.size() >= 2)
    {
        const bool fAnd = (f == "and") != fNeg;
        std::string r;
        for (std::size_t i = 1; i < c.a.size(); ++i) r += (i > 1 ? (fAnd ? " and " : " or ") : "") + Words(c.a[i], fNeg, depth + 1);
        return r;
    }
    if (f == "<null-obj?>" && c.a.size() == 2) return arg(1) + (fNeg ? " is an object" : " is no object (none set)");
    if (f == "<null?>" && c.a.size() == 2) return arg(1) + (fNeg ? " is not empty" : " is empty");
    if ((f == "<=>" || f == "<eqv?>" || f == "<equal?>") && c.a.size() == 3) return arg(1) + (fNeg ? " is not " : " is ") + arg(2);
    static const std::map<std::string, std::pair<const char*, const char*>> s_mpcmp = {
        {"<<>", {" is below ", " is at least "}}, {"<>>", {" is above ", " is at most "}},
        {"<<=>", {" is at most ", " is above "}}, {"<>=>", {" is at least ", " is below "}}};
    if (const auto it = s_mpcmp.find(f); it != s_mpcmp.end() && c.a.size() == 3) return arg(1) + (fNeg ? it->second.second : it->second.first) + arg(2);
    if (f == "<object?>" && c.a.size() == 2) return arg(1) + (fNeg ? " is not an object" : " is an object");
    if (f == "<list?>" && c.a.size() == 2) return arg(1) + (fNeg ? " is not a list" : " is a list");
    // ((<geto> X .opt?) args): X's question
    if (h.fList && FIsHead(h, "<geto>") && h.a.size() >= 3)
    {
        std::string q = FlatText(h.a[1]) + " " + h.a[2].atom;
        for (std::size_t i = 1; i < c.a.size(); ++i) q += " " + arg(i);
        return q + (fNeg ? " is false" : "");
    }
    return FlatText(c) + (fNeg ? " is false" : "");
}
void AssertNote(const NODE& a, std::size_t indent, std::string& out)
{
    if (!t_fAssertNotes || !FIsHead(a, "assert") || a.a.size() < 2) return;
    ++t_cassert;
    out += "; expects: " + Words(a.a[1], false) + "\n";
    out.append(indent, ' ');
}
void PrintFlat(const NODE& n, std::string& out)
{
    out += n.atom;
    if (!n.fList) return;
    out += '(';
    for (std::size_t i = 0; i < n.a.size(); ++i)
    {
        if (i) out += ' ';
        PrintFlat(n.a[i], out);
    }
    out += ')';
}
constexpr std::size_t c_cchLine = 110;
// how many arguments a special form keeps on its head's line
int CHeadArgs(const NODE& n)
{
    if (!n.fList || !n.atom.empty() || n.a.empty() || n.a[0].fList) return -1;
    const std::string& f = n.a[0].atom;
    if (f == "define" || f == "set!" || f == "lambda" || f == "if" || f == "case" || f == "while" || f == "let" || f == "assert") return 1;
    if (f == "begin" || f == "cond" || f == "else" || f == "module") return 0;
    return -1;
}
void PrintPretty(const NODE& n, std::size_t indent, std::string& out)
{
    if (!n.fList || indent + CchFlat(n) <= c_cchLine)
    {
        PrintFlat(n, out);
        return;
    }
    out += n.atom;
    out += '(';
    const int chead = CHeadArgs(n);
    std::size_t i = 0;
    std::size_t indentChild;
    if (chead >= 0)
    {
        // (head arg...  then the body indented by 2
        std::size_t col = indent + n.atom.size() + 1;
        for (; i < n.a.size() && int(i) <= chead; ++i)
        {
            if (i) { out += ' '; ++col; }
            PrintPretty(n.a[i], col, out);
            col += CchFlat(n.a[i]);
        }
        indentChild = indent + 2;
    }
    else
    {
        // a call: its head, the arguments under the first (or by 2 under a long head)
        if (!n.a.empty() && !n.a[0].fList && n.a[0].atom.size() < 24 && n.a.size() > 1)
        {
            out += n.a[0].atom;
            out += ' ';
            indentChild = indent + n.atom.size() + 1 + n.a[0].atom.size() + 1;
            PrintPretty(n.a[1], indentChild, out);
            i = 2;
        }
        else
        {
            indentChild = indent + n.atom.size() + 1;
            if (!n.a.empty()) PrintPretty(n.a[0], indentChild, out);
            i = 1;
            indentChild = indent + 2;
        }
    }
    for (; i < n.a.size(); ++i)
    {
        out += '\n';
        out.append(indentChild, ' ');
        AssertNote(n.a[i], indentChild, out);
        PrintPretty(n.a[i], indentChild, out);
    }
    out += ')';
}

const char* const c_szHeader = "; (readable names: the \";=\" lines at the end tie each name to the game's number)";
const char* const c_szHeaderOld = "; (readable names: display only; the \";=\" lines tie each name to the game's number)";
}  // namespace

void SPLICENAMESTATS::Add(const SPLICENAMESTATS& o)
{
    csym += o.csym; csymNamed += o.csymNamed; csymKind += o.csymKind; csymWord += o.csymWord; csymGuess += o.csymGuess;
    cevent += o.cevent; ceventNamed += o.ceventNamed;
    coptEngine += o.coptEngine; coptEngineNamed += o.coptEngineNamed; coptEngineGuess += o.coptEngineGuess;
    coptScript += o.coptScript; coptScriptNamed += o.coptScriptNamed; coptScriptGuess += o.coptScriptGuess;
    coid += o.coid; coidNamed += o.coidNamed; coidGuess += o.coidGuess;
    cbif += o.cbif; cbifNamed += o.cbifNamed;
    cmsg += o.cmsg; cmsgNamed += o.cmsgNamed; cmsgGuess += o.cmsgGuess;
    ccls += o.ccls; cclsNamed += o.cclsNamed;
    cassert += o.cassert;
}

const char* SzSpliceEventName(int symid)
{
    const SPLICEEVENTNAME* p = PeventOf(symid);
    return p ? p->name : nullptr;
}
const char* SzSpliceEventWhat(const char* name)
{
    for (const SPLICEEVENTNAME& e : c_aevent)
        if (!std::strcmp(e.name, name)) return e.what;
    return nullptr;
}
const char* SzSpliceEventArg(int symid, int iarg)
{
    const SPLICEEVENTNAME* p = PeventOf(symid);
    return p && iarg >= 0 && iarg < 4 ? p->args[iarg] : nullptr;
}
const char* SzSpliceOptionName(int optid)
{
    const auto it = MpOptName().find(optid);
    return it == MpOptName().end() ? nullptr : it->second;
}
const char* SzSpliceBifAlias(int bifk)
{
    for (const SPLICEBIFALIAS& b : c_abif)
        if (b.bifk == bifk) return b.alias;
    return nullptr;
}

const char* SzSpliceClassName(int cid)
{
    for (const SPLICECLASSNAME& c : c_acls)
        if (c.cid == cid) return c.name;
    return nullptr;
}
int SpliceClassByName(const std::string& name)
{
    for (const SPLICECLASSNAME& c : c_acls)
        if (name == c.name) return c.cid;
    return -1;
}
std::string SpliceKindOfOptions(const std::set<int>& aopt)
{
    // the classes having every one of the options
    std::set<int> setcid;
    bool fFirst = true;
    for (int o : aopt)
    {
        const std::set<int>* pc = SpliceOptionClasses(o);
        if (!pc) return std::string();
        if (fFirst) { setcid = *pc; fFirst = false; continue; }
        std::set<int> r;
        for (int c : setcid)
            if (pc->count(c)) r.insert(c);
        setcid = std::move(r);
    }
    if (setcid.empty()) return std::string();
    // their common base (the class table's bases)
    auto chain = [](int cid) {
        std::vector<int> a;
        for (int c = cid, k = 0; c >= 0 && c < CID_Max && k < 32; c = g_acls[c].cidBase, ++k)
        {
            if (g_acls[c].cid != c) break;   // (the classes not set up: no executable)
            a.push_back(c);
        }
        return a;
    };
    std::vector<int> acommon = chain(*setcid.begin());
    for (int c : setcid)
    {
        const std::vector<int> a = chain(c);
        std::vector<int> r;
        for (int x : acommon)
            if (std::find(a.begin(), a.end(), x) != a.end()) r.push_back(x);
        acommon = std::move(r);
    }
    static const std::map<int, const char*> s_mp = {
        {0x01, "world"}, {0x05, "volume"}, {0x06, "proxy"}, {0x0A, "path"}, {0x0F, "zone"}, {0x11, "mesh-zone"}, {0x16, "shape"},
        {0x1F, "grouper"}, {0x20, "explosion"}, {0x2E, "recon-subject"}, {0x34, "player"}, {0x35, "sly"}, {0x36, "murray"},
        {0x37, "bentley"}, {0x45, "turret"}, {0x86, "animation-runner"}, {0x87, "camera"}, {0x88, "state-machine-runner"},
        {0x89, "controller"}, {0x93, "noise"}, {0x99, "attachment"}};
    for (int c : acommon)   // (the most derived first)
    {
        if (const auto it = s_mp.find(c); it != s_mp.end()) return it->second;
        if (const char* sz = SzSpliceClassName(c)) return sz;
    }
    return std::string();
}

bool FSpliceTextReadable(const std::string& text)
{
    if (text.find(c_szHeader) != std::string::npos || text.find(c_szHeaderOld) != std::string::npos || text.rfind(";=", 0) == 0) return true;
    return text.find("\n;=") != std::string::npos;
}

int SpliceNewSymbolId(const std::string& name)
{
    uint32_t h = 2166136261u;
    for (const char c : name) { h ^= uint8_t(c); h *= 16777619u; }
    return 200000 + int(h % 800000u);
}

int SpliceOptionByName(const std::string& name)
{
    const auto it = MpOptByName().find(name);
    return it == MpOptByName().end() ? -1 : it->second;
}
int SpliceBifByName(const std::string& name)
{
    for (const SPLICEBIFALIAS& b : c_abif)
        if (name.size() > 2 && name.compare(1, name.size() - 2, b.alias) == 0 && name.front() == '<' && name.back() == '>') return b.bifk;
    if (name.rfind("#bif:", 0) == 0) return std::atoi(name.c_str() + 5);
    return BifOfRaw(name);
}
const char* SzSpliceJobEvent(int n) { return SzJobEvent(n); }
int SpliceJobEventByName(const std::string& name)
{
    for (const SPLICEJOBEVENT& e : c_ajobevent)
        if (name == e.name) return e.n;
    return -1;
}
int SpliceSymbolByName(const std::string& name)
{
    for (const SPLICEEVENTNAME& e : c_aevent)
        if (name == e.name) return e.symid;
    for (const SPLICEFIXEDSYM& f : c_afixed)
        if (name == f.name) return f.symid;
    return -1;
}
bool FSpliceReservedWord(const std::string& s) { return FReserved(s); }
void SpliceLegend(const std::string& readable, std::map<std::string, std::string>& mp)
{
    std::size_t i = 0;
    while (i < readable.size())
    {
        const std::size_t e = std::min(readable.find('\n', i), readable.size());
        if (readable.compare(i, 2, ";=") == 0)
        {
            std::size_t k = i + 2;
            while (k < e)
            {
                while (k < e && readable[k] == ' ') ++k;
                const std::size_t k0 = k;
                while (k < e && readable[k] != ' ') ++k;
                const std::string ent = readable.substr(k0, k - k0);
                const std::size_t eq = ent.rfind('=');
                if (eq != std::string::npos && eq > 0) mp[ent.substr(0, eq)] = ent.substr(eq + 1);
            }
        }
        i = e + 1;
    }
}
void SpliceTableNames(std::vector<std::string>& a)
{
    for (const SPLICEOPTNAME& o : c_aopt) a.push_back(std::string(".") + o.name);
    for (const SPLICEEVENTNAME& e : c_aevent) a.push_back(e.name);
    for (const SPLICEFIXEDSYM& f : c_afixed) a.push_back(f.name);
    for (const SPLICEBIFALIAS& b : c_abif) a.push_back(std::string("<") + b.alias + ">");
    for (int i = 0; i < 256; ++i)
        if (const char* sz = SzBifNameRaw(i)) a.push_back(std::string("<") + sz + ">");
    for (const SPLICEJOBEVENT& e : c_ajobevent) a.push_back(std::string("%") + e.name);
}

namespace
{
// the text parsed and named (throws on a parse error; false with debug-info 1)
bool FParseAndName(const std::string& raw, std::vector<NODE>& atop, std::vector<const NODE*>& amodule, NAMER& nm)
{
    PARSER p{raw};
    while (!p.FEnd()) atop.push_back(p.Node());
    int fDebug = 0;
    for (const NODE& t : atop)
    {
        if (FIsHead(t, "debug-info") && t.a.size() == 2) fDebug = std::atoi(t.a[1].atom.c_str());
        else if (FIsHead(t, "module") && t.a.size() >= 3) amodule.push_back(&t);
        else throw std::runtime_error("line " + std::to_string(t.line) + ": not (debug-info N) or (module N ...)");
    }
    if (fDebug) return false;
    // passes: each one's names built from others read the previous pass's (its last is the one kept)
    for (int pass = 0; pass < 3; ++pass)
    {
        if (pass)
        {
            nm.mpsymcandPrev = std::move(nm.mpsymcand);
            nm.mpsymcand.clear();
            nm.mpoptcandPrev = std::move(nm.mpoptcand);
            nm.mpoptcand.clear();
            nm.mpwordcand.clear();
        }
        for (const NODE* pm : amodule)
            for (std::size_t k = 2; k < pm->a.size(); ++k)
            {
                const NODE& code = pm->a[k];
                // the module's list ((begin ...)): its first cell is evaluated
                if (code.fList && !code.a.empty()) nm.Scan(code.a[0], -1, true);
                for (std::size_t j = 1; code.fList && j < code.a.size(); ++j) nm.Scan(code.a[j], -1, false);
            }
    }
    nm.Choose();
    return true;
}
}  // namespace

bool FSpliceReadable(const std::string& raw, const SPLICENAMECTX& ctx, std::string& readable, std::string& error, SPLICENAMESTATS* pstats,
                     std::vector<SPLICENAMEROW>* prows)
{
    try
    {
        std::vector<NODE> atop;
        std::vector<const NODE*> amodule;
        NAMER nm(ctx);
        if (!FParseAndName(raw, atop, amodule, nm)) { error = "scripts with debug-info 1 are shown as they are"; return false; }
        t_fAssertNotes = ctx.fAssertNotes;
        t_cassert = 0;
        struct RESET { ~RESET() { t_fAssertNotes = false; } } reset;
        // the text: the title, the header, the legend, then the modules
        std::string out;
        {
            // (the converter's title line)
            const std::size_t e = raw.find('\n');
            const std::string first = raw.substr(0, e);
            out += first.rfind(";", 0) == 0 ? first + "\n" : "; " + ctx.title + "\n";
        }
        out += std::string(c_szHeader) + "\n";
        out += "; self: the object the handler runs for; on-...: the engine's events; .name: an option; $name: an object;\n"
               "; <name>: a built-in; %name: a job event; ~name: an animation / cutscene event word. The names' numbers: the end\n"
               "; of the text. A new name becomes a new symbol (its id stays the same wherever the name is used).\n";
        out += "(debug-info 0)\n";
        for (const NODE* pm : amodule)
        {
            out += "\n(module " + pm->a[1].atom + "\n  ";
            for (std::size_t k = 2; k < pm->a.size(); ++k)
            {
                if (k > 2) out += "\n  ";
                NODE r = nm.Rename(pm->a[k]);
                // the module's (begin ...): each top form on its own lines, a blank line between them
                if (r.fList && r.atom.empty() && r.a.size() == 1 && FIsHead(r.a[0], "begin") && 2 + CchFlat(r) > c_cchLine)
                {
                    out += "((begin";
                    for (std::size_t j = 1; j < r.a[0].a.size(); ++j)
                    {
                        out += j > 1 ? "\n\n    " : "\n    ";
                        AssertNote(r.a[0].a[j], 4, out);
                        PrintPretty(r.a[0].a[j], 4, out);
                    }
                    out += "))";
                }
                else PrintPretty(r, 2, out);
            }
            out += ")\n";
        }
        if (amodule.empty()) out += "; (no modules)\n";
        out += "\n; ---- the names' numbers (\";= name=number\")\n";
        // the legend: by kind
        auto legend = [&](const char* title, auto pred) {
            std::string line;
            bool fAny = false;
            for (const auto& [n, r] : nm.mpLegend)
            {
                if (!pred(n)) continue;
                if (!fAny) { out += std::string(";  ") + title + "\n"; fAny = true; }
                const std::string e = n + "=" + r;
                if (!line.empty() && line.size() + 1 + e.size() > c_cchLine) { out += ";= " + line + "\n"; line.clear(); }
                line += (line.empty() ? "" : " ") + e;
            }
            if (!line.empty()) out += ";= " + line + "\n";
        };
        legend("events and names", [](const std::string& n) { return n[0] != '.' && n[0] != '$' && n[0] != '%' && n[0] != '~' && n[0] != '&' && n[0] != '?'; });
        legend("guessed names (?name: what it is and where it lives, from how the scripts use it)", [](const std::string& n) { return n[0] == '?'; });
        legend("job events", [](const std::string& n) { return n[0] == '%'; });
        legend("event words", [](const std::string& n) { return n[0] == '~'; });
        legend("options (.?name: guessed)", [](const std::string& n) { return n[0] == '.'; });
        legend("objects", [](const std::string& n) { return n[0] == '$'; });
        legend("classes", [](const std::string& n) { return n[0] == '&'; });
        readable = std::move(out);
        if (prows)
        {
            auto confWord = [](int c) { return c >= CONF_Fixed ? "fixed" : c >= CONF_Use + 0 ? "traced" : c > CONF_Guess ? "use" : "guess"; };
            for (const auto& [id, n] : nm.mpsym)
            {
                const auto it = nm.mpsymConf.find(id);
                int c = it == nm.mpsymConf.end() ? CONF_Fixed : it->second;
                prows->push_back({"symbol", "sym" + std::to_string(id), n, FGuess(n) ? "guess" : c >= CONF_Fixed ? "fixed" : c >= CONF_Role ? "traced" : "use"});
            }
            for (const auto& [id, n] : nm.mpopt)
            {
                const bool fEngine = id <= 1232;
                prows->push_back({fEngine ? "option" : "script-option", std::to_string(id), n,
                                  n.size() > 1 && n[1] == '?' ? "guess" : fEngine ? "traced" : nm.mpoptConf[id] >= CONF_Value ? "traced" : "use"});
            }
            for (const auto& [id, n] : nm.mpoid) prows->push_back({"object", std::to_string(id), n, n.size() > 1 && n[1] == '?' ? "guess" : "fixed"});
            for (const auto& [id, n] : nm.mpword) prows->push_back({"event-word", std::to_string(id), n, n.size() > 1 && n[1] == '?' ? "guess" : "use"});
            for (const auto& [id, n] : nm.mpcls) prows->push_back({"class", std::to_string(id), n, n.size() > 1 && n[1] == '?' ? "guess" : "traced"});
            (void)confWord;
        }
        nm.st.cassert = t_cassert;
        if (pstats) *pstats = nm.st;
        return true;
    }
    catch (const std::exception& e)
    {
        error = std::string("scripts: ") + e.what();
        return false;
    }
}

namespace
{
bool FSpliceRawImpl(const std::string& readable, std::string& raw, std::string& error, std::vector<std::string>* panew,
                    std::map<std::string, std::string>* pmpUsed);
}
bool FSpliceRaw(const std::string& readable, std::string& raw, std::string& error, std::vector<std::string>* panew)
{
    return FSpliceRawImpl(readable, raw, error, panew, nullptr);
}
namespace
{
bool FSpliceRawImpl(const std::string& readable, std::string& raw, std::string& error, std::vector<std::string>* panew,
                    std::map<std::string, std::string>* pmpUsed)
{
    // the legend: ";= name=raw ..." lines
    std::unordered_map<std::string, std::string> mp;
    {
        std::size_t i = 0;
        while (i < readable.size())
        {
            const std::size_t e = std::min(readable.find('\n', i), readable.size());
            if (readable.compare(i, 2, ";=") == 0)
            {
                std::size_t k = i + 2;
                while (k < e)
                {
                    while (k < e && readable[k] == ' ') ++k;
                    const std::size_t k0 = k;
                    while (k < e && readable[k] != ' ') ++k;
                    const std::string ent = readable.substr(k0, k - k0);
                    const std::size_t eq = ent.rfind('=');
                    if (eq != std::string::npos && eq > 0) mp[ent.substr(0, eq)] = ent.substr(eq + 1);
                }
            }
            i = e + 1;
        }
    }
    // the symbols in use (the new names get ids none of them has)
    std::set<int> setsym;
    for (const auto& [n, r] : mp)
        if (const int s = SymOf(r); s >= 0) setsym.insert(s);
    std::unordered_map<std::string, std::string> mpnew;
    // the bifs' readable names
    static const std::unordered_map<std::string, std::string> s_mpbif = [] {
        std::unordered_map<std::string, std::string> m;
        for (const SPLICEBIFALIAS& b : c_abif) m[std::string("<") + b.alias + ">"] = RawBif(b.bifk);
        return m;
    }();
    std::string out;
    out.reserve(readable.size());
    std::size_t i = 0;
    int line = 1;
    std::size_t ibLine = 0, ibTok = 0;   // (the column of an error: the token's)
    try
    {
        // first: every symN the text uses (a new name must not take one)
        for (std::size_t k = 0; k + 3 < readable.size(); ++k)
            if (readable.compare(k, 3, "sym") == 0 && (k == 0 || std::strchr(" \t\n(", readable[k - 1])))
            {
                std::size_t j = k + 3;
                while (j < readable.size() && std::isdigit(static_cast<unsigned char>(readable[j]))) ++j;
                if (j > k + 3) setsym.insert(std::atoi(readable.c_str() + k + 3));
            }
        while (i < readable.size())
        {
            const char c = readable[i];
            if (c == '\n') { ++line; out += c; ++i; ibLine = i; continue; }
            ibTok = i;
            if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')') { out += c; ++i; continue; }
            if (c == ';')
            {
                const std::size_t e = std::min(readable.find('\n', i), readable.size());
                // (the readable header and legend are dropped; other comments kept)
                const std::string cm = readable.substr(i, e - i);
                if (cm.rfind(";=", 0) != 0 && cm != c_szHeader && cm != c_szHeaderOld) out += cm;
                i = e;
                continue;
            }
            if (c == '"')
            {
                const std::size_t i0 = i++;
                while (i < readable.size() && readable[i] != '"')
                {
                    if (readable[i] == '\\') ++i;
                    else if (readable[i] == '\n') ++line;
                    ++i;
                }
                if (i >= readable.size()) throw std::runtime_error("a string without its end");
                ++i;
                out += readable.substr(i0, i - i0);
                continue;
            }
            const std::size_t i0 = i;
            while (i < readable.size() && !std::isspace(static_cast<unsigned char>(readable[i])) && readable[i] != '(' && readable[i] != ')' && readable[i] != '"' && readable[i] != ';') ++i;
            const std::string a = readable.substr(i0, i - i0);
            const std::size_t cchOut0 = out.size();
            auto used = [&] {
                if (pmpUsed && out.size() > cchOut0) (*pmpUsed)[a] = out.substr(cchOut0);
            };
            if (const auto it = mp.find(a); it != mp.end()) { out += it->second; used(); continue; }
            if (a[0] == '.' && a.size() > 1)
            {
                if (const auto it = MpOptByName().find(a.substr(1)); it != MpOptByName().end()) { out += std::to_string(it->second); used(); continue; }
                throw std::runtime_error("unknown option name '" + a + "' (an option is .name or its number; a new one needs \";= " + a + "=N\")");
            }
            if (a[0] == '%' && a.size() > 1)
            {
                bool f = false;
                for (const SPLICEJOBEVENT& e : c_ajobevent)
                    if (a.compare(1, std::string::npos, e.name) == 0) { out += std::to_string(e.n); f = true; break; }
                if (f) { used(); continue; }
                throw std::runtime_error("unknown job event '" + a + "'");
            }
            if (a[0] == '$') throw std::runtime_error("unknown object name '" + a + "' (add \";= " + a + "=OID\" or write the oid)");
            if (a[0] == '&' && a.size() > 1)
            {
                if (const int c = SpliceClassByName(a.substr(1)); c >= 0) { out += std::to_string(c); used(); continue; }
                throw std::runtime_error("unknown class name '" + a + "' (add \";= " + a + "=CID\" or write the class id)");
            }
            if (a[0] == '~' && a.size() > 1) throw std::runtime_error("unknown event word '" + a + "' (add \";= " + a + "=N\" or write the number)");
            if (a.front() == '<' && a.back() == '>')
            {
                if (const auto it = s_mpbif.find(a); it != s_mpbif.end()) { out += it->second; used(); continue; }
                out += a;
                continue;
            }
            // a bare word: the special forms, numbers, #..., symN stay; an event's name; else a new symbol
            const bool fWord = std::isalpha(static_cast<unsigned char>(a[0])) || a[0] == '-' || a[0] == '_' ||
                               (a[0] == '?' && a.size() > 1 && std::isalpha(static_cast<unsigned char>(a[1])));
            if (!fWord || FReserved(a) || SymOf(a) >= 0 || FInt(a) || a == "-" || std::isdigit(static_cast<unsigned char>(a.size() > 1 ? a[1] : 'x')))
            {
                out += a;
                continue;
            }
            bool fEvent = false;
            for (const SPLICEEVENTNAME& e : c_aevent)
                if (a == e.name) { out += "sym" + std::to_string(e.symid); fEvent = true; break; }
            if (fEvent) { used(); continue; }
            for (const SPLICEFIXEDSYM& f : c_afixed)
                if (a == f.name) { out += "sym" + std::to_string(f.symid); fEvent = true; break; }
            if (fEvent) { used(); continue; }
            auto it = mpnew.find(a);
            if (it == mpnew.end())
            {
                // (a stable id: the name's hash, the next free one past a symbol the text has)
                int id = SpliceNewSymbolId(a);
                while (setsym.count(id)) id = id + 1 >= 1000000 ? 200000 : id + 1;
                setsym.insert(id);
                it = mpnew.emplace(a, "sym" + std::to_string(id)).first;
                if (panew) panew->push_back(a + " = " + it->second);
            }
            out += it->second;
            used();
        }
    }
    catch (const std::exception& e)
    {
        error = "line " + std::to_string(line) + ":" + std::to_string(int(ibTok - std::min(ibTok, ibLine)) + 1) + ": " + e.what();
        return false;
    }
    raw = std::move(out);
    return true;
}
}  // namespace

bool FSpliceNormalize(const std::string& readable, std::string& out, std::string& error, std::vector<std::string>* panew)
{
    std::string raw;
    std::map<std::string, std::string> mpUsed;
    if (!FSpliceRawImpl(readable, raw, error, panew, &mpUsed)) return false;
    // the text without its table at the end (from its title line: everything after it comments)
    std::string body = readable;
    const std::size_t at = body.rfind("\n; ---- the names' numbers");
    if (at != std::string::npos)
    {
        bool fAllComments = true;
        for (std::size_t i = at + 1; i < body.size();)
        {
            const std::size_t e = std::min(body.find('\n', i), body.size());
            std::size_t k = i;
            while (k < e && (body[k] == ' ' || body[k] == '\t' || body[k] == '\r')) ++k;
            if (k < e && body[k] != ';') { fAllComments = false; break; }
            i = e + 1;
        }
        if (fAllComments) body.resize(at + 1);
    }
    while (body.size() > 1 && body.back() == '\n' && body[body.size() - 2] == '\n') body.pop_back();
    if (!body.empty() && body.back() != '\n') body += '\n';
    out = body + "\n; ---- the names' numbers (\";= name=number\")\n";
    auto legend = [&](const char* title, auto pred) {
        std::string line;
        bool fAny = false;
        for (const auto& [n, r] : mpUsed)
        {
            if (!pred(n)) continue;
            if (!fAny) { out += std::string(";  ") + title + "\n"; fAny = true; }
            const std::string e = n + "=" + r;
            if (!line.empty() && line.size() + 1 + e.size() > c_cchLine) { out += ";= " + line + "\n"; line.clear(); }
            line += (line.empty() ? "" : " ") + e;
        }
        if (!line.empty()) out += ";= " + line + "\n";
    };
    legend("events and names", [](const std::string& n) { return n[0] != '.' && n[0] != '$' && n[0] != '%' && n[0] != '~' && n[0] != '<' && n[0] != '&' && n[0] != '?'; });
    legend("guessed names (?name: what it is and where it lives, from how the scripts use it)", [](const std::string& n) { return n[0] == '?'; });
    legend("job events", [](const std::string& n) { return n[0] == '%'; });
    legend("event words", [](const std::string& n) { return n[0] == '~'; });
    legend("options (.?name: guessed)", [](const std::string& n) { return n[0] == '.'; });
    legend("objects", [](const std::string& n) { return n[0] == '$'; });
    legend("classes", [](const std::string& n) { return n[0] == '&'; });
    return true;
}

bool FSpliceTextToBytes(const std::string& text, std::vector<uint8_t>& ab, std::string& error)
{
    if (FSpliceTextReadable(text))
    {
        std::string raw;
        if (!FSpliceRaw(text, raw, error)) return false;
        return FSpliceFromText(raw, ab, error);
    }
    if (FSpliceFromText(text, ab, error)) return true;
    // (names without a table: a hand-written readable text, taken when it has no errors: a typo in the converter's
    // text must not become a new symbol)
    std::string raw, err2;
    std::vector<uint8_t> ab2;
    if (FSpliceRaw(text, raw, err2) && FSpliceFromText(raw, ab2, err2))
    {
        SPLICEDIAGCTX ctx;
        ctx.fEngineTables = false;
        std::vector<SPLICEISSUE> adiag;
        SpliceDiagnose(text, ctx, adiag);
        for (const SPLICEISSUE& d : adiag)
            if (d.sev == SPLICESEV_Error) { error = StrSpliceDiag(d); return false; }
        ab = std::move(ab2);
        error.clear();
        return true;
    }
    return false;
}

int SpliceNamesSelfTest()
{
    int cfail = 0;
    std::set<std::string> set;
    for (const SPLICEOPTNAME& o : c_aopt)
    {
        if (!set.insert(o.name).second) { std::printf("splice-names: option name %s twice\n", o.name); ++cfail; }
        if (Slug(o.name) != o.name) { std::printf("splice-names: option name %s is not a plain name\n", o.name); ++cfail; }
    }
    set.clear();
    for (const SPLICEEVENTNAME& e : c_aevent)
        if (!set.insert(e.name).second || FReserved(e.name)) { std::printf("splice-names: event name %s twice / reserved\n", e.name); ++cfail; }
    for (const SPLICEFIXEDSYM& f : c_afixed)
        if (!set.insert(f.name).second) { std::printf("splice-names: fixed name %s twice\n", f.name); ++cfail; }
    set.clear();
    for (const SPLICEBIFALIAS& b : c_abif)
        if (!set.insert(b.alias).second || BifOfRaw(std::string("<") + b.alias + ">") >= 0 && BifOfRaw(std::string("<") + b.alias + ">") != b.bifk)
        {
            std::printf("splice-names: built-in alias %s twice / another built-in's name\n", b.alias);
            ++cfail;
        }
    // a small text both ways
    const std::string raw =
        "; group 1 test\n(debug-info 0)\n\n(module 0\n  ((begin\n    (define sym1074 #obj:0)\n"
        "    (define sym0 (lambda (sym1002) (set! sym1074 (<find-object> 4090 sym1002)) ((<geto> sym1074 40))))\n"
        "    (define sym4 (lambda (sym990 sym991) (case sym991 ((5569) (#bif:130 0 1)))))\n"
        "    (define sym5 (lambda (sym1002) (<addo> sym1002 1524 (lambda #none ((<geto> sym1074 39)))) (<seto!> sym1074 34 #vec(1 2 3))))\n"
        "    (define sym2100 (lambda (sym2101) (<for-each> (lambda (sym2102) ((<geto> sym2102 40))) (<find-objects> 56 sym2101)) (sym1533)))\n"
        "    (define sym2103 (quote (4090)))\n"
        "    (define sym2104 (lambda (sym2105) (<car> (<find-objects-of-names> sym2103 sym2105)))))))\n";
    SPLICENAMECTX ctx;
    ctx.pfnObject = [](int oid) { return oid == 4090 ? std::string("Door 1") : std::string(); };
    std::string rd, back, err;
    if (!FSpliceReadable(raw, ctx, rd, err)) { std::printf("splice-names: readable failed: %s\n", err.c_str()); return cfail + 1; }
    if (!FSpliceRaw(rd, back, err)) { std::printf("splice-names: raw failed: %s\n%s\n", err.c_str(), rd.c_str()); return cfail + 1; }
    for (const char* want : {"on-start", "self", "$door-1", ".remove", ".add", ".position", "<rumble>", "on-event", "event", "&guard",
                             "on-aim-setup-failed", "(quote ($door-1))", "?param-1-of-"})
        if (rd.find(want) == std::string::npos) { std::printf("splice-names: the readable text has no %s:\n%s\n", want, rd.c_str()); ++cfail; }
    std::vector<uint8_t> abA, abB;
    if (!FSpliceFromText(raw, abA, err) || !FSpliceFromText(back, abB, err) || abA != abB)
    {
        std::printf("splice-names: the round trip does not compile to the same bytes (%s):\n%s\n", err.c_str(), back.c_str());
        ++cfail;
    }
    // a new name: a new symbol
    std::string rd2 = rd;
    const std::size_t at = rd2.find("(<seto!>");
    if (at != std::string::npos) rd2.insert(at, "(define my-counter 1) ");
    std::vector<std::string> anew;
    if (!FSpliceRaw(rd2, back, err, &anew) || anew.size() != 1 || !FSpliceFromText(back, abB, err))
    {
        std::printf("splice-names: a new name was not made a new symbol (%s)\n", err.c_str());
        ++cfail;
    }
    if (FSpliceRaw(rd + "\n; x\n(module 1 ((<find-objects> &no-such-class self)))\n", back, err))
    {
        std::printf("splice-names: an unknown class name was taken\n");
        ++cfail;
    }
    // a mod's definition of the original's never-defined function compiles to its symbol
    if (!FSpliceRaw("(debug-info 0)\n(module 0 ((begin (define on-aim-setup-failed (lambda #none 1)) (define ?my-guess 2))))\n", back, err) ||
        back.find("(define sym1533 ") == std::string::npos)
    {
        std::printf("splice-names: on-aim-setup-failed did not compile to sym1533 (%s)\n", back.c_str());
        ++cfail;
    }
    if (FSpliceRaw(rd + "\n; x\n(module 1 ((<geto> self .no-such-option)))\n", back, err))
    {
        std::printf("splice-names: an unknown option name was taken\n");
        ++cfail;
    }
    return cfail;
}

namespace
{
struct ACTWALK
{
    const NAMER& nm;
    std::map<int, int> mpsymOid;    // symbols bound to (find-object N) in the module
    std::vector<SPLICEOBJACT>* pa;
    int imodule = 0;
    std::string ev;
    std::vector<std::string> acond;

    std::string Flat(const NODE& n) const
    {
        std::string s;
        PrintFlat(nm.Rename(n), s);
        if (s.size() > 90) s = s.substr(0, 87) + "...";
        return s;
    }
    // what object an expression is: its oid (0: self / this, -1: not known)
    int OidOf(const NODE& x) const
    {
        if (!x.fList)
        {
            const int s = SymOf(x.atom);
            if (s == 1002 || s == 990) return 0;
            if (const auto it = mpsymOid.find(s); it != mpsymOid.end()) return it->second;
            return -1;
        }
        if ((FIsHead(x, "<find-object>") || FIsHead(x, "<find-object-kind-4>")) && x.a.size() >= 2)
        {
            long oid = 0;
            if (FInt(x.a[1].atom, &oid) && oid > 0) return int(oid);
        }
        return -1;
    }
    void Walk(const NODE& n)
    {
        if (!n.fList || !n.atom.empty() || n.a.empty()) return;
        const std::string f = n.a[0].fList ? std::string() : n.a[0].atom;
        if (f == "if" && n.a.size() >= 3)
        {
            Walk(n.a[1]);
            acond.push_back(Flat(n.a[1]));
            Walk(n.a[2]);
            acond.back() = "not " + acond.back();
            for (std::size_t k = 3; k < n.a.size(); ++k) Walk(n.a[k]);
            acond.pop_back();
            return;
        }
        if (f == "case" && n.a.size() >= 3)
        {
            Walk(n.a[1]);
            const std::string key = Flat(n.a[1]);
            for (std::size_t k = 2; k < n.a.size(); ++k)
            {
                const NODE& c = n.a[k];
                if (!c.fList || c.a.empty()) continue;
                acond.push_back(c.a[0].fList ? key + " is " + Flat(c.a[0]) : key + " is something else");
                for (std::size_t j = 1; j < c.a.size(); ++j) Walk(c.a[j]);
                acond.pop_back();
            }
            return;
        }
        if (f == "<schedule-callback>" && n.a.size() >= 3)
        {
            acond.push_back("after " + Flat(n.a[1]) + " s");
            for (std::size_t k = 2; k < n.a.size(); ++k) Walk(n.a[k]);
            acond.pop_back();
            return;
        }
        // ((<geto> X 39 / 40 / 1019 / 1023 / 1024))
        if (n.a[0].fList && FIsHead(n.a[0], "<geto>") && n.a[0].a.size() >= 3)
        {
            long opt = 0;
            if (FInt(n.a[0].a[2].atom, &opt) && (opt == 39 || opt == 40 || opt == 1019 || opt == 1023 || opt == 1024 || opt == 13 || opt == 12))
            {
                SPLICEOBJACT a;
                a.imodule = imodule;
                a.oid = OidOf(n.a[0].a[1]);
                a.optid = int(opt);
                a.event = ev;
                for (const std::string& c : acond) a.cond += (a.cond.empty() ? "" : ", ") + c;
                a.who = Flat(n.a[0].a[1]);
                pa->push_back(std::move(a));
            }
        }
        if (f == "lambda")
        {
            // (a function inside: its body runs when it is called)
            for (std::size_t k = 2; k < n.a.size(); ++k) Walk(n.a[k]);
            return;
        }
        for (const NODE& c : n.a) Walk(c);
    }
};
// symbols bound to one object found by a literal oid (define / set!)
void CollectOidSyms(const NODE& n, std::map<int, int>& mp)
{
    if (!n.fList) return;
    if ((FIsHead(n, "define") || FIsHead(n, "set!")) && n.a.size() >= 3 && !n.a[1].fList)
    {
        const NODE& e = n.a[2];
        long oid = 0;
        if ((FIsHead(e, "<find-object>") || FIsHead(e, "<find-object-kind-4>")) && e.a.size() >= 2 && FInt(e.a[1].atom, &oid) && oid > 0)
            mp[SymOf(n.a[1].atom)] = int(oid);
    }
    for (const NODE& c : n.a) CollectOidSyms(c, mp);
}
}  // namespace

bool FSpliceObjectActions(const std::string& raw, const SPLICENAMECTX& ctx, std::vector<SPLICEOBJACT>& aact, std::string& error)
{
    try
    {
        std::vector<NODE> atop;
        std::vector<const NODE*> amodule;
        NAMER nm(ctx);
        if (!FParseAndName(raw, atop, amodule, nm)) { error = "debug-info 1"; return false; }
        for (const NODE* pm : amodule)
        {
            ACTWALK w{nm, {}, &aact};
            w.imodule = std::atoi(pm->a[1].atom.c_str());
            for (std::size_t k = 2; k < pm->a.size(); ++k) CollectOidSyms(pm->a[k], w.mpsymOid);
            // the module's top forms: each define's name is the event / function the actions run in
            for (std::size_t k = 2; k < pm->a.size(); ++k)
            {
                const NODE& code = pm->a[k];
                if (!code.fList || code.a.empty()) continue;
                const NODE& top = code.a[0];
                std::vector<const NODE*> aform;
                if (FIsHead(top, "begin")) for (std::size_t j = 1; j < top.a.size(); ++j) aform.push_back(&top.a[j]);
                else aform.push_back(&top);
                for (const NODE* pf : aform)
                {
                    w.ev = "the module's load";
                    if (FIsHead(*pf, "define") && pf->a.size() >= 3 && !pf->a[1].fList)
                    {
                        w.ev = nm.Atom(pf->a[1].atom);
                        w.Walk(pf->a[2]);
                    }
                    else w.Walk(*pf);
                }
            }
        }
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

void SpliceCollectNames(const std::string& raw, const SPLICENAMECTX& ctx, SPLICEWORLDNAMES& world)
{
    try
    {
        std::vector<NODE> atop;
        std::vector<const NODE*> amodule;
        SPLICENAMECTX c = ctx;
        c.pworld = nullptr;
        NAMER nm(c);
        if (!FParseAndName(raw, atop, amodule, nm)) return;
        auto merge = [](std::map<int, std::pair<std::string, int>>& mp, int k, const std::pair<std::string, int>& v) {
            if (v.second <= CONF_Kind) return;
            auto it = mp.find(k);
            if (it == mp.end() || v.second > it->second.second) mp[k] = v;
        };
        for (const auto& [k, v] : nm.mpsymBase) merge(world.mpsym, k, v);
        for (const auto& [k, v] : nm.mpoptBase) merge(world.mpopt, k, v);
        // the event words: the name most handlers give
        for (const auto& [w, a] : nm.mpwordcand)
        {
            std::map<std::string, int>& mpv = world.mpwordvote[w];
            for (const CAND& c : a) mpv[c.name] += c.count;
            const auto itBest = std::max_element(mpv.begin(), mpv.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
            if (itBest != mpv.end()) world.mpword[w] = {itBest->first, itBest->second};
        }
    }
    catch (const std::exception&)
    {
    }
}
