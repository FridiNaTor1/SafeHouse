// Readable splice scripts (splicenames.h): parse the converter's text, name what the port knows, print it back with
// its own name table; and the way back.
#include "splicenames.h"

#include "worldparts.h"

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

// ---------------------------------------------------------------------------------------------------------
// the text: S-expressions as the converter prints them (worldparts.cpp LEXER: '(' ')' "strings" atoms ; comments);
// an atom glued to a following '(' (#vec(...), @(...)) is the list's prefix
struct NODE
{
    std::string atom;          // an atom; a list's prefix
    bool fList = false;
    std::vector<NODE> a;
    int line = 0;
};

struct PARSER
{
    const std::string& t;
    std::size_t i = 0;
    int line = 1;
    [[noreturn]] void Fail(const std::string& what) { throw std::runtime_error("line " + std::to_string(line) + ": " + what); }
    void Skip()
    {
        for (;;)
        {
            while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
                if (t[i++] == '\n') ++line;
            if (i < t.size() && t[i] == ';')
            {
                while (i < t.size() && t[i] != '\n') ++i;
                continue;
            }
            break;
        }
    }
    bool FEnd() { Skip(); return i >= t.size(); }
    NODE Node()
    {
        Skip();
        if (i >= t.size()) Fail("unexpected end");
        NODE n;
        n.line = line;
        const char c = t[i];
        if (c == ')') Fail("unexpected ')'");
        if (c == '"')
        {
            const std::size_t i0 = i++;
            while (i < t.size() && t[i] != '"')
            {
                if (t[i] == '\\') ++i;
                else if (t[i] == '\n') ++line;
                ++i;
            }
            if (i >= t.size()) Fail("a string without its end");
            ++i;
            n.atom = t.substr(i0, i - i0);
            return n;
        }
        if (c != '(')
        {
            const std::size_t i0 = i;
            while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != '(' && t[i] != ')' && t[i] != '"' && t[i] != ';') ++i;
            n.atom = t.substr(i0, i - i0);
            if (i < t.size() && t[i] == '(') { List(n); }
            return n;
        }
        List(n);
        return n;
    }
    void List(NODE& n)
    {
        ++i;   // '('
        n.fList = true;
        for (;;)
        {
            Skip();
            if (i >= t.size()) Fail("a list without its ')'");
            if (t[i] == ')') { ++i; return; }
            n.a.push_back(Node());
        }
    }
};

bool FInt(const std::string& s, long* pv = nullptr)
{
    if (s.empty()) return false;
    char* pe = nullptr;
    const long v = std::strtol(s.c_str(), &pe, 10);
    if (*pe) return false;
    if (pv) *pv = v;
    return true;
}
int SymOf(const std::string& s)
{
    if (s.size() < 4 || s.compare(0, 3, "sym") != 0) return -1;
    for (std::size_t k = 3; k < s.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) return -1;
    return std::atoi(s.c_str() + 3);
}
bool FIsHead(const NODE& n, const char* sz) { return n.fList && n.atom.empty() && !n.a.empty() && !n.a[0].fList && n.a[0].atom == sz; }
const NODE* PArg(const NODE& n, std::size_t i) { return n.fList && i < n.a.size() ? &n.a[i] : nullptr; }

// the words the reader keeps for itself (worldparts.cpp's tag words) and the names we never give
bool FReserved(const std::string& s)
{
    static const std::set<std::string> s_set = {"#none", "set!", "define", "assert", "debug", "if", "or", "and", "cond", "else", "case", "let",
                                                "while", "lambda", "begin", "import", "quote", ".", "!", "module", "debug-info"};
    return s_set.count(s) != 0;
}

std::string Slug(const std::string& in)
{
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
    if (s.back() == 's') return s;
    if (s.back() == 'y' && s.size() > 1 && !std::strchr("aeiou", s[s.size() - 2])) return s.substr(0, s.size() - 1) + "ies";
    return s + "s";
}
std::string Singular(const std::string& s)
{
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
    std::set<int> setoid;                          // literal oids
    std::set<int> setoptEngine, setoptScript, setbif, setevent;
    std::set<std::string> setcase;
    std::map<int, int> mpsymOid;                   // a symbol that holds one object: its oid (find-object N)
    std::set<int> setsymOidKey;                    // symbols holding an oid: their case values are oids
    std::set<int> setsymJobEvKey;                  // symbols holding a job event number (%name in their case values)
    int iseq = 0;

    // the final maps
    std::map<int, std::string> mpsym, mpopt, mpoid;
    std::map<int, std::pair<std::string, int>> mpsymBase, mpoptBase;   // the names before made unique, their confidence
    std::map<std::string, std::string> mpLegend;   // name -> raw (the ";=" lines)

    explicit NAMER(const SPLICENAMECTX& c) : ctx(c) {}

    void Note(int sym)
    {
        if (!mpsymFirst.count(sym)) mpsymFirst[sym] = iseq++;
    }
    void Cand(int sym, const std::string& name, int conf)
    {
        if (sym < 0 || name.empty()) return;
        Note(sym);
        std::vector<CAND>& a = mpsymcand[sym];
        for (CAND& c : a)
            if (c.name == name) { c.count++; c.conf = std::max(c.conf, conf); return; }
        a.push_back({name, conf, 1});
    }
    void OptCand(int optid, const std::string& name, int conf)
    {
        if (name.empty()) return;
        std::vector<CAND>& a = mpoptcand[optid];
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
            const std::string n = NameOfExpr(e.a[1], depth + 1);
            return n.empty() ? "obj" : n;
        }
        if (f == "<find-objects>" || f == "#bif:113" || f == "<find-object-113 (0x1241E8)>" || f == "<find-nearest-objects>")
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
                if (opt > 1232) return std::string();
            }
            return std::string();
        }
        if (f == "lambda") return "fn";
        if (f == "quote") return e.a.size() >= 2 && e.a[1].fList ? "list" : std::string();
        if (f == "<list>" || f == "<cons>" || f == "<append>" || f == "<shuffle>") return "list";
        if (f == "<filter>" && e.a.size() >= 3) { const std::string n = NameOfExpr(e.a[2], depth + 1); return n.empty() ? "list" : n; }
        if (f == "<map>") return "list";
        if (f == "<car>" || f == "<nth>")
            if (e.a.size() >= 2) { const std::string n = NameFromSymOrExpr(e.a[1], depth); return n.empty() ? std::string() : Singular(n); }
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
                return pbest->conf >= CONF_Arg ? pbest->name : std::string();
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
                if (const std::string n = OptName(int(opt)); !n.empty()) return "set-" + (n.back() == '?' ? n.substr(0, n.size() - 1) : n);
        }
        if (FIsHead(b, "set!") && b.a.size() >= 3 && !b.a[1].fList && b.a[2].fList && !b.a[2].a.empty() && FIsHead(b.a[2].a[0], "<geto>"))
            if (std::string n = NameOfStatement(b.a[2], depth + 1); !n.empty()) return n;
        if (FIsHead(b, "set!") && b.a.size() >= 3 && !b.a[1].fList)
        {
            const std::string v = NameFromSymOrExpr(b.a[1], 0);
            if (!v.empty()) return (b.a[2].atom == "#obj:0" ? "clear-" : "set-") + v;
        }
        // ((<geto> X .opt) args): the method's name and who it is called on
        if (!b.a.empty() && FIsHead(b.a[0], "<geto>") && b.a[0].a.size() >= 3)
        {
            long opt = 0;
            if (FInt(b.a[0].a[2].atom, &opt))
                if (std::string n = OptName(int(opt)); !n.empty() && n.back() != '?')
                {
                    const std::string who = NameFromSymOrExpr(b.a[0].a[1], 0);
                    if (!who.empty() && who != "self" && who != "this") n += "-" + who;
                    return n;
                }
        }
        // (F args): a wrapper of another function
        if (!b.a.empty() && !b.a[0].fList)
            if (const int sf = SymOf(b.a[0].atom); sf >= 0)
                if (const std::string n = NameFromSymOrExpr(b.a[0], 0); !n.empty()) return n;
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
            if (const std::string n = NameFromSymOrExpr(lam.a[2], 0); !n.empty()) return "get-" + n;
        for (std::size_t k = 2; k < lam.a.size(); ++k)
        {
            const NODE& b = lam.a[k];
            if (FIsHead(b, "assert") || FIsHead(b, "define")) continue;
            if (std::string n = NameOfStatement(b, 0); !n.empty()) return n;
        }
        return std::string();
    }

    // ---- the analysis: one pass gathering candidates (run twice: the second sees the first's names)
    struct FN
    {
        const NODE* plam = nullptr;
    };
    std::map<int, const NODE*> mpfn;   // symbols defined as lambdas: their lambda

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
                                if (i == 0) Cand(p, p == 990 ? "this" : "self", CONF_Role);
                                else if (const SPLICEEVENTNAME* pe = PeventOf(s); pe && i - 1 < 4 && pe->args[i - 1]) Cand(p, pe->args[i - 1], CONF_Role);
                                else Cand(p, "arg" + std::to_string(i), CONF_Kind);
                            }
                    for (std::size_t k = 2; k < e.a.size(); ++k) Scan(e.a[k], s, false);
                    if (s == 4)   // the event words of on-event's case
                        CollectCases(e);
                    return;
                }
                if (FIsHead(e, "lambda"))
                {
                    mpfn[s] = &e;
                    if (const std::string nl = NameOfLambda(e); !nl.empty()) Cand(s, nl, CONF_Value);
                    else Cand(s, "fn", CONF_Kind);
                }
                else if ((FIsHead(e, "<+>") || FIsHead(e, "<->")) && e.a.size() == 3 && e.a[1].atom == n.a[1].atom && FInt(e.a[2].atom)) Cand(s, "counter", CONF_Value);
                else if (const std::string ne = NameOfExpr(e); !ne.empty()) Cand(s, ne, ne == "obj" || ne == "list" || ne == "flag" ? CONF_Kind : CONF_Value);
                else if (!e.fList && (FInt(e.atom) || e.atom.find('.') != std::string::npos) && std::isdigit(static_cast<unsigned char>(e.atom.back()))) Cand(s, "n", CONF_Kind);
                // a symbol holding one object found by its oid
                if ((FIsHead(e, "<find-object>") || FIsHead(e, "<find-object-kind-4>")) && e.a.size() >= 2)
                {
                    long oid = 0;
                    if (FInt(e.a[1].atom, &oid)) mpsymOid[s] = int(oid);
                }
            }
            for (std::size_t k = 2; k < n.a.size(); ++k) Scan(n.a[k], ievent, false);
            return;
        }
        if (f == "lambda" && n.a.size() >= 2)
        {
            if (n.a[1].fList)
                for (const NODE& p : n.a[1].a)
                    if (const int s = SymOf(p.atom); s >= 0) { Note(s); setsymNonWord.insert(s); }
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
                    for (const NODE& v : n.a[k].a[0].a) NoteOid(v);
        if (f == "case" && n.a.size() >= 3 && !n.a[1].fList && setsymJobEvKey.count(SymOf(n.a[1].atom)))
            for (std::size_t k = 2; k < n.a.size(); ++k)
                if (n.a[k].fList && !n.a[k].a.empty() && n.a[k].a[0].fList)
                    for (const NODE& v : n.a[k].a[0].a)
                    {
                        long e = 0;
                        if (FInt(v.atom, &e) && SzJobEvent(int(e))) mpLegend[std::string("%") + SzJobEvent(int(e))] = v.atom;
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
                    OptCand(int(opt), nv, nv == "fn" || nv == "obj" || nv == "list" || nv == "flag" ? CONF_Kind : CONF_Value);
                }
                if (f == "<seto!>" && FOidArg(int(opt), -1) && n.a.size() >= 4) NoteOid(n.a[3]);
            }
            // (<geto> X opt) where X is a symbol: an object
            if (const int s = SymOf(n.a[1].atom); s >= 0) Cand(s, "obj", CONF_Kind);
        }
        if ((f == "<find-object>" || f == "<find-object-kind-4>" || f == "<find-objects>" || f == "#bif:113" || f == "<find-object-113 (0x1241E8)>") && n.a.size() >= 2)
        {
            NoteOid(n.a[1]);
            if (const int s = SymOf(n.a[1].atom); s >= 0) Cand(s, "oid", CONF_Use);   // (a symbol holding an oid)
        }
        // (<seto!> X .opt S): S holds what the option takes
        if (f == "<seto!>" && n.a.size() >= 4 && !n.a[3].fList)
            if (const int s = SymOf(n.a[3].atom); s >= 0)
            {
                long opt = 0;
                if (FInt(n.a[2].atom, &opt))
                    if (const std::string on = OptName(int(opt)); !on.empty() && on.back() != '?') Cand(s, "new-" + on, CONF_Use);
            }
        if (f == "<find-objects-of-names>" && n.a.size() >= 2 && FIsHead(n.a[1], "quote") && n.a[1].a.size() >= 2 && n.a[1].a[1].fList)
            for (const NODE& o : n.a[1].a[1].a) NoteOid(o);
        if (!f.empty() && (f[0] == '<' || f.rfind("#bif:", 0) == 0)) setbif.insert(BifOfRaw(f));
        // a method call ((<geto> X opt) args): oid arguments
        if (h.fList && FIsHead(h, "<geto>") && h.a.size() >= 3)
        {
            long opt = 0;
            if (FInt(h.a[2].atom, &opt))
                for (std::size_t i = 1; i < n.a.size(); ++i)
                    if (FOidArg(int(opt), int(i) - 1)) NoteOid(n.a[i]);
        }
        // (for-each / map / filter (lambda (x) ...) L): x is one of L
        if ((f == "<for-each>" || f == "<map>" || f == "<filter>") && n.a.size() >= 3 && FIsHead(n.a[1], "lambda") && n.a[1].a.size() >= 2 &&
            n.a[1].a[1].fList && n.a[1].a[1].a.size() == 1)
        {
            const std::string nl = NameFromSymOrExpr(n.a[2], 0);
            if (const int p = SymOf(n.a[1].a[1].a[0].atom); p >= 0 && !nl.empty()) Cand(p, Singular(nl), CONF_Arg);
        }
        // (F a b): F's parameters from the arguments
        if (const int sf = SymOf(f); sf >= 0)
            if (const auto it = mpfn.find(sf); it != mpfn.end() && it->second->a.size() >= 2 && it->second->a[1].fList)
            {
                const std::vector<NODE>& ap = it->second->a[1].a;
                for (std::size_t i = 0; i < ap.size() && i + 1 < n.a.size(); ++i)
                    if (const int p = SymOf(ap[i].atom); p >= 0)
                    {
                        const std::string na = NameFromSymOrExpr(n.a[i + 1], 0);
                        if (!na.empty() && na != "self" && na != "this") Cand(p, na, CONF_Arg);
                        else if (na == "self" || na == "this") Cand(p, "obj", CONF_Kind);
                    }
            }
        for (const NODE& c : n.a) Scan(c, ievent, false);
    }
    void NoteOid(const NODE& o)
    {
        long oid = 0;
        if (!o.fList && FInt(o.atom, &oid) && oid > 0) setoid.insert(int(oid));
    }
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
            if (!name.empty()) mpsymBase[s] = {name, conf};
            const bool fWord = setsymWordOnly.count(s) && !setsymNonWord.count(s);
            ++st.csym;
            if (fWord) ++st.csymWord;
            if (name.empty()) continue;   // (stays symN)
            if (conf <= CONF_Kind)
            {
                ++st.csymKind;
                name = reserve(name + "-" + std::to_string(s), std::to_string(s));
            }
            else
            {
                ++st.csymNamed;
                name = reserve(name, std::to_string(s));
            }
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
                ++st.coptEngineNamed;
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
            if (ctx.pworld && conf <= CONF_Kind)
                if (const auto it = ctx.pworld->mpopt.find(o); it != ctx.pworld->mpopt.end() && it->second.second > conf && it->second.second > CONF_Kind)
                {
                    n = it->second.first;
                    conf = it->second.second;
                }
            if (n.empty()) continue;
            mpoptBase[o] = {n, conf};
            if (conf <= CONF_Kind) n += "-" + std::to_string(o);
            else ++st.coptScriptNamed;
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
            const std::string n = ObjName(oid);
            if (n.empty()) continue;
            ++st.coidNamed;
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
        for (int b : setbif)
            if (b >= 0)
            {
                ++st.cbif;
                const std::string r = ReadableBif(b);
                if (r.rfind("#bif:", 0) != 0 && r.find(' ') == std::string::npos && r.rfind("<bif-", 0) != 0) ++st.cbifNamed;
            }
        st.cmsg = int(setcase.size());
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
        if ((f == "<find-object>" || f == "<find-object-kind-4>" || f == "<find-objects>" || f == "#bif:113" || f == "<find-object-113 (0x1241E8)>") && n.a.size() >= 2)
            oidAt(r.a[1], n.a[1]);
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
std::size_t CchFlat(const NODE& n)
{
    if (!n.fList) return n.atom.size();
    std::size_t c = n.atom.size() + 2;
    for (std::size_t i = 0; i < n.a.size(); ++i) c += CchFlat(n.a[i]) + (i ? 1 : 0);
    return c;
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
        PrintPretty(n.a[i], indentChild, out);
    }
    out += ')';
}

const char* const c_szHeader = "; (readable names: display only; the \";=\" lines tie each name to the game's number)";
}  // namespace

void SPLICENAMESTATS::Add(const SPLICENAMESTATS& o)
{
    csym += o.csym; csymNamed += o.csymNamed; csymKind += o.csymKind; csymWord += o.csymWord;
    cevent += o.cevent; ceventNamed += o.ceventNamed;
    coptEngine += o.coptEngine; coptEngineNamed += o.coptEngineNamed;
    coptScript += o.coptScript; coptScriptNamed += o.coptScriptNamed;
    coid += o.coid; coidNamed += o.coidNamed;
    cbif += o.cbif; cbifNamed += o.cbifNamed;
    cmsg += o.cmsg;
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

bool FSpliceTextReadable(const std::string& text) { return text.find(c_szHeader) != std::string::npos; }

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
            nm.mpoptcand.clear();
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

bool FSpliceReadable(const std::string& raw, const SPLICENAMECTX& ctx, std::string& readable, std::string& error, SPLICENAMESTATS* pstats)
{
    try
    {
        std::vector<NODE> atop;
        std::vector<const NODE*> amodule;
        NAMER nm(ctx);
        if (!FParseAndName(raw, atop, amodule, nm)) { error = "scripts with debug-info 1 are shown as they are"; return false; }
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
               "; <name>: a built-in; %name: a job event. The names' numbers: the end of the text. A new name becomes a new symbol.\n";
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
                        PrintPretty(r.a[0].a[j], 4, out);
                    }
                    out += "))";
                }
                else PrintPretty(r, 2, out);
            }
            out += ")\n";
        }
        if (amodule.empty()) out += "; (no modules)\n";
        out += "\n; ---- the names' numbers (display only: \";= name=number\")\n";
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
        legend("events and names", [](const std::string& n) { return n[0] != '.' && n[0] != '$' && n[0] != '%'; });
        legend("job events", [](const std::string& n) { return n[0] == '%'; });
        legend("options", [](const std::string& n) { return n[0] == '.'; });
        legend("objects", [](const std::string& n) { return n[0] == '$'; });
        readable = std::move(out);
        if (pstats) *pstats = nm.st;
        return true;
    }
    catch (const std::exception& e)
    {
        error = std::string("scripts: ") + e.what();
        return false;
    }
}

bool FSpliceRaw(const std::string& readable, std::string& raw, std::string& error, std::vector<std::string>* panew)
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
    int symNext = 9000;
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
            if (c == '\n') { ++line; out += c; ++i; continue; }
            if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')') { out += c; ++i; continue; }
            if (c == ';')
            {
                const std::size_t e = std::min(readable.find('\n', i), readable.size());
                // (the readable header and legend are dropped; other comments kept)
                const std::string cm = readable.substr(i, e - i);
                if (cm.rfind(";=", 0) != 0 && cm != c_szHeader) out += cm;
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
            if (const auto it = mp.find(a); it != mp.end()) { out += it->second; continue; }
            if (a[0] == '.' && a.size() > 1)
            {
                if (const auto it = MpOptByName().find(a.substr(1)); it != MpOptByName().end()) { out += std::to_string(it->second); continue; }
                throw std::runtime_error("unknown option name '" + a + "' (an option is .name or its number)");
            }
            if (a[0] == '%' && a.size() > 1)
            {
                bool f = false;
                for (const SPLICEJOBEVENT& e : c_ajobevent)
                    if (a.compare(1, std::string::npos, e.name) == 0) { out += std::to_string(e.n); f = true; break; }
                if (f) continue;
                throw std::runtime_error("unknown job event '" + a + "'");
            }
            if (a[0] == '$') throw std::runtime_error("unknown object name '" + a + "' (add \";= " + a + "=OID\" or write the oid)");
            if (a.front() == '<' && a.back() == '>')
            {
                if (const auto it = s_mpbif.find(a); it != s_mpbif.end()) { out += it->second; continue; }
                out += a;
                continue;
            }
            // a bare word: the special forms, numbers, #..., symN stay; an event's name; else a new symbol
            const bool fWord = std::isalpha(static_cast<unsigned char>(a[0])) || a[0] == '-' || a[0] == '_';
            if (!fWord || FReserved(a) || SymOf(a) >= 0 || FInt(a) || a == "-" || std::isdigit(static_cast<unsigned char>(a.size() > 1 ? a[1] : 'x')))
            {
                out += a;
                continue;
            }
            bool fEvent = false;
            for (const SPLICEEVENTNAME& e : c_aevent)
                if (a == e.name) { out += "sym" + std::to_string(e.symid); fEvent = true; break; }
            if (fEvent) continue;
            for (const SPLICEFIXEDSYM& f : c_afixed)
                if (a == f.name) { out += "sym" + std::to_string(f.symid); fEvent = true; break; }
            if (fEvent) continue;
            auto it = mpnew.find(a);
            if (it == mpnew.end())
            {
                while (setsym.count(symNext)) ++symNext;
                setsym.insert(symNext);
                it = mpnew.emplace(a, "sym" + std::to_string(symNext)).first;
                if (panew) panew->push_back(a + " = " + it->second);
            }
            out += it->second;
        }
    }
    catch (const std::exception& e)
    {
        error = "line " + std::to_string(line) + ": " + e.what();
        return false;
    }
    raw = std::move(out);
    return true;
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
        "    (define sym5 (lambda (sym1002) (<addo> sym1002 1524 (lambda #none ((<geto> sym1074 39)))) (<seto!> sym1074 34 #vec(1 2 3)))))))\n";
    SPLICENAMECTX ctx;
    ctx.pfnObject = [](int oid) { return oid == 4090 ? std::string("Door 1") : std::string(); };
    std::string rd, back, err;
    if (!FSpliceReadable(raw, ctx, rd, err)) { std::printf("splice-names: readable failed: %s\n", err.c_str()); return cfail + 1; }
    if (!FSpliceRaw(rd, back, err)) { std::printf("splice-names: raw failed: %s\n%s\n", err.c_str(), rd.c_str()); return cfail + 1; }
    for (const char* want : {"on-start", "self", "$door-1", ".remove", ".add", ".position", "<rumble>", "on-event", "event"})
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
    }
    catch (const std::exception&)
    {
    }
}
