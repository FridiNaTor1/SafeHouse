// Diagnostics of the splice scripts (splicediag.h).
#include "splicediag.h"

#include "cls.h"
#include "disc.h"
#include "eval.h"
#include "splicenames.h"
#include "splicesexp.h"
#include "worldparts.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>


namespace
{
using namespace splicesexp;

// ---- the executable's option tables: which classes have an option, the methods' argument counts
struct ENGINEOPTS
{
    bool fOk = false;
    std::map<int, std::set<int>> mpoptcid;      // option -> the classes that have it
    std::map<int, std::set<int>> mpmethodcarg;  // a method option -> its argument counts (over the classes)
};
const ENGINEOPTS& EngineOpts()
{
    static ENGINEOPTS s_eo;
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        if (!g_disc.FIsOpen()) return;
        try
        {
            for (int cid = 0; cid < 0xA0; ++cid)
            {
                const uint32_t c = g_disc.U32Elf(0x279AD0 + uint32_t(cid) * 8);
                const uint32_t a = g_disc.U32Elf(0x279AD0 + uint32_t(cid) * 8 + 4);
                if (c == 0 || c > 3000) continue;
                for (uint32_t i = 0; i < c; ++i)
                {
                    const int opt = int(g_disc.U32Elf(a + i * 8));
                    const uint32_t vaE = g_disc.U32Elf(a + i * 8 + 4);
                    s_eo.mpoptcid[opt].insert(cid);
                    const uint32_t grf = g_disc.U32Elf(vaE + 4);
                    if (grf & 1) s_eo.mpmethodcarg[opt].insert(int(g_disc.U32Elf(vaE + 0x18)));
                }
            }
            s_eo.fOk = !s_eo.mpoptcid.empty();
        }
        catch (const std::exception&)
        {
            s_eo = ENGINEOPTS();
        }
    });
    return s_eo;
}
}  // namespace
// the classes whose tables have the option (null: none / no executable)
const std::set<int>* SpliceOptionClasses(int optid)
{
    const ENGINEOPTS& eo = EngineOpts();
    const auto it = eo.mpoptcid.find(optid);
    return it == eo.mpoptcid.end() ? nullptr : &it->second;
}
namespace
{
// the class has the option (its table at 0x279AD0)
bool FClassHasOption(int cid, int optid)
{
    const ENGINEOPTS& eo = EngineOpts();
    const auto it = eo.mpoptcid.find(optid);
    return it != eo.mpoptcid.end() && it->second.count(cid);
}

int Distance(const std::string& a, const std::string& b)
{
    std::vector<int> d(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) d[j] = int(j);
    for (std::size_t i = 1; i <= a.size(); ++i)
    {
        int prev = d[0];
        d[0] = int(i);
        for (std::size_t j = 1; j <= b.size(); ++j)
        {
            const int cur = d[j];
            d[j] = std::min({d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] == b[j - 1] ? 0 : 1)});
            prev = cur;
        }
    }
    return d[b.size()];
}

enum RESK
{
    RESK_Special,   // a special form's word, else, #none ...
    RESK_Sym,
    RESK_Num,
    RESK_Str,
    RESK_Flag,      // #t / #f
    RESK_Obj0,      // #obj:0
    RESK_Opt,
    RESK_Oid,
    RESK_Word,
    RESK_JobEv,
    RESK_Bif,
    RESK_Bad,
};
struct RES
{
    RESK k = RESK_Bad;
    long v = 0;
    bool fNew = false;   // a new name (a new symbol)
    std::string why;     // RESK_Bad: what it is not
};

struct DEF
{
    const NODE* pname = nullptr;
    const NODE* pval = nullptr;
    int cref = 0;
    int cparam = -1;      // a lambda's fixed parameters (-1: not a lambda)
    bool fRest = false;
    bool fParam = false;
};
struct SCOPE
{
    SCOPE* pparent = nullptr;
    bool fModule = false;
    std::map<long, DEF> mpdef;
};

struct DIAGNOSER
{
    const SPLICEDIAGCTX& ctx;
    std::vector<SPLICEISSUE>& a;
    std::map<std::string, std::string> legend;
    std::vector<std::string> anameTable;
    const ENGINEOPTS* peo = nullptr;
    int imodule = -1;
    std::set<const NODE*> setReported;   // (names already reported: no "unused" hint on them)

    void Add(const NODE& n, int sev, const char* code, const std::string& msg, const std::string& fixLabel = std::string(),
             const std::string& fixText = std::string())
    {
        SPLICEISSUE d;
        d.line = n.line;
        d.col = n.col;
        d.lineEnd = n.lineEnd ? n.lineEnd : n.line;
        d.colEnd = n.colEnd ? n.colEnd : n.col + int(n.atom.size()) - 1;
        if (!n.fList && n.atom.empty()) d.colEnd = n.col;
        d.sev = sev;
        d.code = code;
        d.message = msg;
        d.fixLabel = fixLabel;
        d.fixText = fixText;
        a.push_back(std::move(d));
    }

    std::string Closest(const std::string& s, const std::vector<std::string>& acand) const
    {
        std::string best;
        int dBest = std::max(2, int(s.size()) / 4) + 1;
        for (const std::string& c : acand)
        {
            if (c.empty() || c[0] != s[0] || c == s) continue;
            if (std::abs(int(c.size()) - int(s.size())) >= dBest) continue;
            const int d = Distance(s, c);
            if (d < dBest) { dBest = d; best = c; }
        }
        return best;
    }
    std::vector<std::string> NamesLike(char c0, const SCOPE* psc = nullptr) const
    {
        std::vector<std::string> v;
        for (const std::string& n : anameTable)
            if (!n.empty() && n[0] == c0) v.push_back(n);
        for (const auto& [n, r] : legend)
            if (!n.empty() && n[0] == c0) v.push_back(n);
        (void)psc;
        return v;
    }

    RES Resolve(const std::string& s) const
    {
        RES r;
        if (s.empty()) { r.why = "an empty word"; return r; }
        if (s[0] == '"') { r.k = RESK_Str; return r; }
        long v = 0;
        if (FInt(s, &v)) { r.k = RESK_Num; r.v = v; return r; }
        if ((std::isdigit(static_cast<unsigned char>(s[0])) || ((s[0] == '-' || s[0] == '.') && s.size() > 1 && std::isdigit(static_cast<unsigned char>(s[1])))) &&
            std::strtod(s.c_str(), nullptr) == std::strtod(s.c_str(), nullptr))
        {
            char* pe = nullptr;
            std::strtod(s.c_str(), &pe);
            if (pe && !*pe) { r.k = RESK_Num; return r; }
        }
        if (s == "#t" || s == "#f") { r.k = RESK_Flag; r.v = s == "#t"; return r; }
        if (s == "#obj:0") { r.k = RESK_Obj0; return r; }
        if (s.rfind("#bif:", 0) == 0) { r.k = RESK_Bif; r.v = std::atoi(s.c_str() + 5); return r; }
        if (s[0] == '#') { r.k = RESK_Special; return r; }
        if (const auto it = legend.find(s); it != legend.end())
        {
            RES rr = Resolve(it->second);
            if (s[0] == '.' && rr.k == RESK_Num) rr.k = RESK_Opt;
            else if (s[0] == '$' && rr.k == RESK_Num) rr.k = RESK_Oid;
            else if (s[0] == '~' && rr.k == RESK_Num) rr.k = RESK_Word;
            else if (s[0] == '%' && rr.k == RESK_Num) rr.k = RESK_JobEv;
            return rr;
        }
        if (s[0] == '.' && s.size() > 1)
        {
            const int o = SpliceOptionByName(s.substr(1));
            if (o >= 0) { r.k = RESK_Opt; r.v = o; return r; }
            r.why = "option";
            return r;
        }
        if (s[0] == '$' && s.size() > 1) { r.why = "object"; return r; }
        if (s[0] == '~' && s.size() > 1) { r.why = "event word"; return r; }
        if (s[0] == '%' && s.size() > 1)
        {
            const int n = SpliceJobEventByName(s.substr(1));
            if (n >= 0) { r.k = RESK_JobEv; r.v = n; return r; }
            r.why = "job event";
            return r;
        }
        if (s[0] == '&' && s.size() > 1)
        {
            const int c = SpliceClassByName(s.substr(1));
            if (c >= 0) { r.k = RESK_Num; r.v = c; return r; }
            r.why = "class";
            return r;
        }
        if (s.size() > 2 && s.front() == '<' && s.back() == '>')
        {
            const int b = SpliceBifByName(s);
            if (b >= 0) { r.k = RESK_Bif; r.v = b; return r; }
            r.why = "built-in";
            return r;
        }
        if (FSpliceReservedWord(s)) { r.k = RESK_Special; return r; }
        if (const int sym = SymOf(s); sym >= 0) { r.k = RESK_Sym; r.v = sym; return r; }
        if (const int sym = SpliceSymbolByName(s); sym >= 0) { r.k = RESK_Sym; r.v = sym; return r; }
        const bool fWord = std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '-' || s[0] == '_' ||
                           (s[0] == '?' && s.size() > 1 && std::isalpha(static_cast<unsigned char>(s[1])));
        if (!fWord) { r.k = RESK_Special; return r; }
        r.k = RESK_Sym;
        r.v = SpliceNewSymbolId(s);
        r.fNew = true;
        return r;
    }

    // an atom's meaning, with an error for a name nothing maps
    RES Atom(const NODE& n)
    {
        RES r = Resolve(n.atom);
        if (r.k == RESK_Bad)
        {
            const char c0 = n.atom[0];
            const std::string sug = Closest(n.atom, NamesLike(c0));
            std::string msg = "unknown " + r.why + " '" + n.atom + "'";
            if (r.why == "option") msg += ": no option has this name (a new one needs a \";= " + n.atom + "=N\" line)";
            else if (r.why == "object") msg += ": no \";=\" line gives its oid (write the oid or add \";= " + n.atom + "=OID\")";
            else if (r.why == "event word") msg += ": no \";=\" line gives its number";
            else if (r.why == "built-in") msg += ": the game has no built-in of this name";
            else if (r.why == "class") msg += ": no class has this name (write the class id or add \";= " + n.atom + "=CID\")";
            Add(n, SPLICESEV_Error, ("unknown-" + std::string(r.why == "event word" ? "word" : r.why == "job event" ? "job-event" : r.why == "built-in" ? "builtin" : r.why)).c_str(),
                msg + (sug.empty() ? std::string() : " (did you mean " + sug + "?)"), sug.empty() ? std::string() : "Replace with " + sug, sug);
        }
        return r;
    }

    // ---- scopes
    static DEF* PdefFind(SCOPE* psc, long sym)
    {
        for (SCOPE* p = psc; p; p = p->pparent)
            if (const auto it = p->mpdef.find(sym); it != p->mpdef.end()) return &it->second;
        return nullptr;
    }
    // the defines of a body (hoisted: the frame binds them as they run; not inside functions)
    void Collect(const NODE& n, SCOPE& sc)
    {
        if (!n.fList || !n.atom.empty() || n.a.empty()) return;
        if (n.a[0].fList)
        {
            // (a clause of a case / cond, a method call: their bodies bind into the same frame)
            for (const NODE& c : n.a) Collect(c, sc);
            return;
        }
        const std::string& f = n.a[0].atom;
        if (f == "lambda" || f == "quote") return;
        if (f == "define" && n.a.size() >= 3 && !n.a[1].fList)
        {
            const RES r = Resolve(n.a[1].atom);
            if (r.k == RESK_Sym)
            {
                DEF d;
                d.pname = &n.a[1];
                d.pval = &n.a[2];
                if (FIsHead(n.a[2], "lambda") && n.a[2].a.size() >= 2)
                {
                    const NODE& ps = n.a[2].a[1];
                    if (ps.fList)
                    {
                        d.cparam = 0;
                        for (const NODE& p : ps.a)
                            if (p.atom == ".") d.fRest = true;
                            else if (!d.fRest) ++d.cparam;
                    }
                    else if (ps.atom == "#none") d.cparam = 0;
                    else d.fRest = true, d.cparam = 0;
                }
                if (const auto it = sc.mpdef.find(r.v); it != sc.mpdef.end() && sc.fModule && it->second.pname != &n.a[1])
                    Add(n.a[1], SPLICESEV_Warning, "redefined",
                        "'" + n.a[1].atom + "' is defined again (first at line " + std::to_string(it->second.pname->line) + "): the later define wins");
                sc.mpdef[r.v] = d;
            }
            Collect(n.a[2], sc);
            return;
        }
        for (std::size_t i = 1; i < n.a.size(); ++i) Collect(n.a[i], sc);
    }

    // ---- the walk
    void Literal(const NODE& n)
    {
        // (quoted data: only the names must map)
        if (!n.fList)
        {
            if (!n.atom.empty() && std::strchr(".$~%", n.atom[0]) && n.atom.size() > 1) Atom(n);
            return;
        }
        for (const NODE& c : n.a) Literal(c);
    }

    void Ref(const NODE& n, const RES& r, SCOPE* psc)
    {
        if (r.k != RESK_Sym) return;
        if (DEF* pd = PdefFind(psc, r.v)) { ++pd->cref; return; }
        // (the engine's events and the handlers' fixed parameters are bound by the engine)
        if (r.v < 64 || r.v == 1002 || r.v == 990 || r.v == 991) return;
        std::vector<std::string> acand;
        for (SCOPE* p = psc; p; p = p->pparent)
            for (const auto& [id, d] : p->mpdef)
                if (d.pname) acand.push_back(d.pname->atom);
        const std::string sug = Closest(n.atom, acand);
        Add(n, SPLICESEV_Error, "unknown-symbol",
            "'" + n.atom + "' is not defined in this module (its value would be nothing)" + (sug.empty() ? std::string() : " (did you mean " + sug + "?)"),
            sug.empty() ? std::string() : "Replace with " + sug, sug);
    }

    static bool FLiteralKind(const RES& r) { return r.k == RESK_Num || r.k == RESK_Str || r.k == RESK_Flag; }

    void Expr(const NODE& n, SCOPE* psc)
    {
        if (!n.fList)
        {
            const RES r = Atom(n);
            Ref(n, r, psc);
            return;
        }
        if (!n.atom.empty()) { Literal(n); return; }   // #vec(...), @(...)
        if (n.a.empty()) return;
        const NODE& h = n.a[0];
        if (h.fList)
        {
            // ((<geto> X .opt) args): a method
            Expr(h, psc);
            if (FIsHead(h, "<geto>") && h.a.size() >= 3 && !h.a[2].fList)
            {
                const RES ro = Resolve(h.a[2].atom);
                if ((ro.k == RESK_Opt || ro.k == RESK_Num) && peo && peo->fOk && ro.v <= 1232)
                    if (const auto it = peo->mpmethodcarg.find(ro.v); it != peo->mpmethodcarg.end() && it->second.size() == 1)
                    {
                        const int c = *it->second.begin();
                        const int carg = int(n.a.size()) - 1;
                        if (carg > c)
                            Add(n.a[std::size_t(c) + 1], SPLICESEV_Warning, "arg-count",
                                "the method " + h.a[2].atom + " takes " + std::to_string(c) + " argument" + (c == 1 ? "" : "s") + ": the rest are not even evaluated");
                    }
            }
            for (std::size_t i = 1; i < n.a.size(); ++i) Expr(n.a[i], psc);
            return;
        }
        const RES rh = Atom(h);
        const std::string& f = h.atom;
        if (rh.k == RESK_Special)
        {
            if (f == "quote") { for (std::size_t i = 1; i < n.a.size(); ++i) Literal(n.a[i]); return; }
            if (f == "define")
            {
                if (n.a.size() != 3) Add(n, SPLICESEV_Error, "syntax", "(define name value) takes a name and a value");
                if (n.a.size() >= 2 && (n.a[1].fList || Resolve(n.a[1].atom).k != RESK_Sym))
                    Add(n.a[1], SPLICESEV_Error, "syntax", "define needs a name here");
                for (std::size_t i = 2; i < n.a.size(); ++i) Expr(n.a[i], psc);
                return;
            }
            if (f == "set!")
            {
                if (n.a.size() != 3) Add(n, SPLICESEV_Error, "syntax", "(set! name value) takes a name and a value");
                if (n.a.size() >= 2)
                {
                    if (n.a[1].fList) Add(n.a[1], SPLICESEV_Error, "syntax", "set! needs a name here");
                    else if (const RES r = Atom(n.a[1]); r.k == RESK_Sym && !PdefFind(psc, r.v) && r.v >= 64 && r.v != 1002 && r.v != 990 && r.v != 991)
                        Add(n.a[1], SPLICESEV_Warning, "set-undefined",
                            "set! of '" + n.a[1].atom + "', which nothing defines: it sets nothing (define it first)", "Define it here",
                            "(define " + n.a[1].atom);
                    else if (r.k != RESK_Sym) Add(n.a[1], SPLICESEV_Error, "syntax", "set! needs a name here");
                }
                for (std::size_t i = 2; i < n.a.size(); ++i) Expr(n.a[i], psc);
                return;
            }
            if (f == "lambda") { Lambda(n, psc, -1); return; }
            if (f == "if")
            {
                if (n.a.size() < 3 || n.a.size() > 4) Add(n, SPLICESEV_Error, "syntax", "(if condition then [else]) takes 2 or 3 parts");
                if (n.a.size() >= 3 && !n.a[1].fList && (n.a[1].atom == "#t" || n.a[1].atom == "#f"))
                {
                    const bool fTrue = n.a[1].atom == "#t";
                    const NODE* pdead = fTrue ? (n.a.size() >= 4 ? &n.a[3] : nullptr) : &n.a[2];
                    Add(pdead ? *pdead : n.a[1], SPLICESEV_Warning, "unreachable",
                        std::string("unreachable: the condition is always ") + (fTrue ? "true" : "false"));
                }
                for (std::size_t i = 1; i < n.a.size(); ++i) Expr(n.a[i], psc);
                return;
            }
            if (f == "cond")
            {
                bool fElse = false;
                for (std::size_t i = 1; i < n.a.size(); ++i)
                {
                    const NODE& c = n.a[i];
                    if (fElse) Add(c, SPLICESEV_Warning, "unreachable", "unreachable: an earlier clause always applies");
                    if (!c.fList || c.a.empty()) { Add(c, SPLICESEV_Error, "syntax", "a cond clause is (condition body...)"); continue; }
                    if (!c.a[0].fList && (c.a[0].atom == "else" || c.a[0].atom == "#t")) fElse = true;
                    for (std::size_t j = c.a[0].atom == "else" ? 1 : 0; j < c.a.size(); ++j) Expr(c.a[j], psc);
                }
                return;
            }
            if (f == "case")
            {
                if (n.a.size() < 2) { Add(n, SPLICESEV_Error, "syntax", "(case key clauses...) needs a key"); return; }
                Expr(n.a[1], psc);
                bool fElse = false;
                std::map<std::string, int> mpseen;
                for (std::size_t i = 2; i < n.a.size(); ++i)
                {
                    const NODE& c = n.a[i];
                    if (fElse) Add(c, SPLICESEV_Warning, "unreachable", "unreachable: a clause after else");
                    if (!c.fList || c.a.empty()) { Add(c, SPLICESEV_Error, "syntax", "a case clause is ((values...) body...) or (else body...)"); continue; }
                    if (!c.a[0].fList && c.a[0].atom == "else") fElse = true;
                    else if (c.a[0].fList)
                        for (const NODE& v : c.a[0].a)
                        {
                            const RES rv = Atom(v);
                            const std::string key = rv.k == RESK_Bad ? v.atom : std::to_string(int(rv.k)) + ":" + std::to_string(rv.v);
                            if (rv.k == RESK_Bad) continue;
                            if (const auto it = mpseen.find(key); it != mpseen.end())
                                Add(v, SPLICESEV_Warning, "unreachable",
                                    "the value " + v.atom + " is taken by the clause at line " + std::to_string(it->second) + ": this one never gets it");
                            else mpseen[key] = v.line;
                        }
                    else Add(c.a[0], SPLICESEV_Error, "syntax", "a case clause starts with its values in a list, or else");
                    for (std::size_t j = 1; j < c.a.size(); ++j) Expr(c.a[j], psc);
                }
                return;
            }
            if (f == "while")
            {
                if (n.a.size() >= 3 && !n.a[1].fList && n.a[1].atom == "#f") Add(n.a[2], SPLICESEV_Warning, "unreachable", "unreachable: the loop's condition is always false");
                for (std::size_t i = 1; i < n.a.size(); ++i) Expr(n.a[i], psc);
                return;
            }
            if (f == "assert")
            {
                if (n.a.size() >= 2) Expr(n.a[1], psc);
                for (std::size_t i = 2; i < n.a.size(); ++i)
                    if (FIsHead(n.a[i], "quote")) Literal(n.a[i]);
                    else Expr(n.a[i], psc);
                return;
            }
            if (f == "else") { Add(h, SPLICESEV_Error, "syntax", "else outside a cond / case"); return; }
            for (std::size_t i = 1; i < n.a.size(); ++i) Expr(n.a[i], psc);
            return;
        }
        if (FLiteralKind(rh) || rh.k == RESK_Obj0 || rh.k == RESK_Opt || rh.k == RESK_Oid || rh.k == RESK_Word || rh.k == RESK_JobEv)
        {
            Add(h, SPLICESEV_Error, "not-callable", "'" + f + "' is not a function: it cannot be called");
            for (std::size_t i = 1; i < n.a.size(); ++i) Expr(n.a[i], psc);
            return;
        }
        const int carg = int(n.a.size()) - 1;
        if (rh.k == RESK_Bif)
        {
            Builtin(n, int(rh.v), carg, psc);
            for (std::size_t i = 1; i < n.a.size(); ++i) Expr(n.a[i], psc);
            return;
        }
        if (rh.k == RESK_Sym)
        {
            DEF* pd = PdefFind(psc, rh.v);
            if (pd) ++pd->cref;
            else if (rh.v == 1533 || rh.v == 1407)
                // (the original's two calls of a function no script of the game defines: looked up, nothing, applied:
                // nothing happens; notes/SCRIPTNAMES3_2026-11-18.md)
                Add(h, SPLICESEV_Warning, "never-defined",
                    "'" + f + "' is called but no script of the game defines it (the original's own; the call does nothing): define it in this "
                    "module to give it a meaning");
            else if (rh.v >= 64 && rh.v != 1002 && rh.v != 990 && rh.v != 991)
            {
                std::vector<std::string> acand;
                for (SCOPE* p = psc; p; p = p->pparent)
                    for (const auto& [id, d] : p->mpdef)
                        if (d.pname && d.cparam >= 0) acand.push_back(d.pname->atom);
                const std::string sug = Closest(f, acand);
                Add(h, SPLICESEV_Error, "unknown-function",
                    "unknown function '" + f + "': this module defines none of this name" + (sug.empty() ? std::string() : " (did you mean " + sug + "?)"),
                    sug.empty() ? std::string() : "Replace with " + sug, sug);
            }
            if (pd && pd->cparam >= 0 && !pd->fParam)
            {
                if (carg < pd->cparam)
                    Add(n, SPLICESEV_Error, "arg-count",
                        "'" + f + "' needs " + std::to_string(pd->cparam) + " argument" + (pd->cparam == 1 ? "" : "s") + ", this call gives " + std::to_string(carg) +
                            " (the missing ones are nothing)");
                else if (carg > pd->cparam && !pd->fRest)
                    Add(n.a[std::size_t(pd->cparam) + 1], SPLICESEV_Warning, "arg-count",
                        "'" + f + "' takes " + std::to_string(pd->cparam) + " argument" + (pd->cparam == 1 ? "" : "s") + ": the rest are not even evaluated");
            }
            for (std::size_t i = 1; i < n.a.size(); ++i) Expr(n.a[i], psc);
            return;
        }
        for (std::size_t i = 1; i < n.a.size(); ++i) Expr(n.a[i], psc);
    }

    int OidOf(const NODE& x) const
    {
        if (x.fList) return -1;
        const RES r = Resolve(x.atom);
        return (r.k == RESK_Oid || r.k == RESK_Num) && r.v > 0 ? int(r.v) : -1;
    }

    void Builtin(const NODE& n, int bifk, int carg, SCOPE* psc)
    {
        (void)psc;
        const std::string& f = n.a[0].atom;
        if (bifk >= 0 && bifk < BIFK_Max)
        {
            const BIF& b = g_mpbifkbif[bifk];
            if (carg < b.crefReq)
                Add(n, SPLICESEV_Error, "arg-count",
                    f + " needs " + std::string(b.fVarArg ? "at least " : "") + std::to_string(b.crefReq) + " argument" + (b.crefReq == 1 ? "" : "s") + ", this call gives " +
                        std::to_string(carg));
            else if (carg > b.crefReq && !b.fVarArg)
                Add(n.a[std::size_t(b.crefReq) + 1], SPLICESEV_Warning, "arg-count",
                    f + " takes " + std::to_string(b.crefReq) + " argument" + (b.crefReq == 1 ? "" : "s") + ": the rest are not even evaluated");
        }
        // arithmetic / comparisons of a string, flag or object literal
        static const std::set<int> s_setArith = {0, 1, 2, 3, 8, 9, 10, 11};
        if (s_setArith.count(bifk))
            for (std::size_t i = 1; i < n.a.size(); ++i)
                if (!n.a[i].fList)
                {
                    const RES r = Resolve(n.a[i].atom);
                    if (r.k == RESK_Str || r.k == RESK_Flag || r.k == RESK_Obj0 || r.k == RESK_Oid)
                        Add(n.a[i], SPLICESEV_Warning, "type", f + " works on numbers (and vectors): " + n.a[i].atom + " is " +
                                                                     (r.k == RESK_Str ? "a string" : r.k == RESK_Flag ? "a flag" : "an object"));
                }
        // options: (<geto> X .opt) (<seto!> X .opt v) (<addo> X opt v) (<ensureo> X opt v)
        if ((f == "<geto>" || f == "<seto!>" || f == "<addo>" || f == "<ensureo>") && n.a.size() >= 3)
        {
            const NODE& x = n.a[1];
            if (!x.fList)
            {
                const RES rx = Resolve(x.atom);
                if (rx.k == RESK_Num || rx.k == RESK_Str || rx.k == RESK_Flag)
                    Add(x, SPLICESEV_Error, "type", f + " takes an object first: " + x.atom + " is " + (rx.k == RESK_Num ? "a number" : rx.k == RESK_Str ? "a string" : "a flag"));
            }
            const NODE& o = n.a[2];
            RES ro;
            if (!o.fList) ro = Resolve(o.atom);
            if (!o.fList && (ro.k == RESK_Str || ro.k == RESK_Flag))
                Add(o, SPLICESEV_Error, "type", f + "'s second argument is an option (.name or its number), not " + o.atom);
            if (!o.fList && (ro.k == RESK_Opt || ro.k == RESK_Num) && peo && peo->fOk && ro.v >= 0 && ro.v <= 1232 && (f == "<geto>" || f == "<seto!>"))
            {
                const auto it = peo->mpoptcid.find(int(ro.v));
                if (it == peo->mpoptcid.end())
                    Add(o, SPLICESEV_Warning, "unknown-option", "no class has the option " + o.atom + " (ids up to 1232 are the engine's)");
                else
                {
                    // the object's class (a literal object, a find-object of one)
                    int oid = -1;
                    if (!x.fList) oid = OidOf(x);
                    else if ((FIsHead(x, "<find-object>") || FIsHead(x, "<find-object-kind-4>")) && x.a.size() >= 2) oid = OidOf(x.a[1]);
                    const int cid = oid > 0 && ctx.pfnObjectCid ? ctx.pfnObjectCid(oid) : -1;
                    if (cid >= 0 && !FClassHasOption(cid, int(ro.v)))
                        Add(o, SPLICESEV_Warning, "class-option", "the object " + (x.fList ? x.a[1].atom : x.atom) + "'s class has no option " + o.atom + ": it reads nothing / sets nothing");
                    // self: the classes running this module
                    if (cid < 0 && !x.fList && (x.atom == "self" || x.atom == "sym1002") && ctx.pfnModuleClasses && imodule >= 0)
                    {
                        const std::vector<int> acid = ctx.pfnModuleClasses(imodule);
                        bool fAny = acid.empty();
                        for (int c : acid) fAny = fAny || FClassHasOption(c, int(ro.v));
                        if (!fAny)
                            Add(o, SPLICESEV_Warning, "class-option",
                                "the objects running this module have no option " + o.atom + ": it reads nothing / sets nothing");
                    }
                }
            }
        }
        // objects looked up by their oid
        if ((f == "<find-object>" || f == "<find-object-kind-4>") && n.a.size() >= 2)
        {
            const int oid = OidOf(n.a[1]);
            if (oid > 0 && ctx.pfnObjectExists && !ctx.pfnObjectExists(oid) && !ctx.setoidKnown.count(oid))
                Add(n.a[1], SPLICESEV_Warning, "missing-object", "no object " + n.a[1].atom + (n.a[1].atom[0] == '$' ? " (" + std::to_string(oid) + ")" : std::string()) +
                                                                     " in this world: the lookup finds nothing");
        }
        if (f == "<find-objects-of-names>" && n.a.size() >= 2 && FIsHead(n.a[1], "quote") && n.a[1].a.size() >= 2 && n.a[1].a[1].fList)
            for (const NODE& v : n.a[1].a[1].a)
            {
                const int oid = OidOf(v);
                if (oid > 0 && ctx.pfnObjectExists && !ctx.pfnObjectExists(oid) && !ctx.setoidKnown.count(oid))
                    Add(v, SPLICESEV_Warning, "missing-object", "no object " + v.atom + " in this world: the lookup finds nothing");
            }
    }

    // (lambda params body...); symEvent: the engine event it handles (-1: none)
    void Lambda(const NODE& n, SCOPE* psc, long symEvent)
    {
        SCOPE sc;
        sc.pparent = psc;
        if (n.a.size() < 2) { Add(n, SPLICESEV_Error, "syntax", "(lambda (parameters...) body...) needs its parameters"); return; }
        const NODE& ps = n.a[1];
        int cparam = 0;
        if (ps.fList)
            for (const NODE& p : ps.a)
            {
                if (p.fList) { Add(p, SPLICESEV_Error, "syntax", "a parameter is a name"); continue; }
                if (p.atom == ".") continue;
                const RES r = Resolve(p.atom);
                if (r.k != RESK_Sym) { Add(p, SPLICESEV_Error, "syntax", "a parameter is a name, not " + p.atom); continue; }
                DEF d;
                d.pname = &p;
                d.fParam = true;
                d.cref = 1;   // (parameters are not reported unused)
                sc.mpdef[r.v] = d;
                ++cparam;
            }
        else if (ps.atom != "#none" && Resolve(ps.atom).k == RESK_Sym)
        {
            DEF d;
            d.pname = &ps;
            d.fParam = true;
            d.cref = 1;
            sc.mpdef[Resolve(ps.atom).v] = d;
        }
        // an engine event's handler: the arguments the engine passes (the object and the event's own)
        if (symEvent >= 0 && symEvent < 64)
            if (const char* sz = SzSpliceEventName(int(symEvent)); sz && ps.fList)
            {
                int cpass = 1;
                while (cpass < 5 && SzSpliceEventArg(int(symEvent), cpass - 1)) ++cpass;
                if (std::strcmp(sz, "on-event") == 0 || std::strcmp(sz, "on-event-16") == 0) cpass = std::max(cpass, 2);
                if (cparam > cpass)
                    Add(ps.a[std::size_t(cpass)], SPLICESEV_Warning, "event-args",
                        std::string(sz) + " gets " + std::to_string(cpass) + " argument" + (cpass == 1 ? "" : "s") + " from the engine: '" + ps.a[std::size_t(cpass)].atom +
                            "' is always nothing");
            }
        for (std::size_t i = 2; i < n.a.size(); ++i) Collect(n.a[i], sc);
        for (std::size_t i = 2; i < n.a.size(); ++i) Expr(n.a[i], &sc);
        for (const auto& [id, d] : sc.mpdef)
            if (!d.fParam && d.cref == 0 && d.pname)
            {
                // (the define's own line, removable when it is alone on its lines)
                Add(*d.pname, SPLICESEV_Warning, "unused", "'" + d.pname->atom + "' is defined but never used in this function");
            }
    }

    void Module(const NODE& m)
    {
        imodule = m.a.size() >= 2 ? std::atoi(m.a[1].atom.c_str()) : -1;
        SCOPE sc;
        sc.fModule = true;
        for (std::size_t k = 2; k < m.a.size(); ++k)
        {
            const NODE& code = m.a[k];
            if (!code.fList) { Add(code, SPLICESEV_Error, "syntax", "a module's code is a list"); continue; }
            for (const NODE& c : code.a) Collect(c, sc);
        }
        for (std::size_t k = 2; k < m.a.size(); ++k)
        {
            const NODE& code = m.a[k];
            if (!code.fList) continue;
            for (const NODE& c : code.a) TopForm(c, &sc);
        }
        for (const auto& [id, d] : sc.mpdef)
            if (d.cref == 0 && d.pname && id >= 64 && id != 1002 && id != 990 && id != 991 && !setReported.count(d.pname))
                Add(*d.pname, SPLICESEV_Hint, "unused", "'" + d.pname->atom + "' is defined but nothing in this module uses it");
    }
    // a module's top: (begin defines...), a define of an engine event
    void TopForm(const NODE& n, SCOPE* psc)
    {
        if (FIsHead(n, "begin"))
        {
            for (std::size_t i = 1; i < n.a.size(); ++i) TopForm(n.a[i], psc);
            return;
        }
        if (FIsHead(n, "define") && n.a.size() == 3 && !n.a[1].fList)
        {
            const RES r = Resolve(n.a[1].atom);
            if (r.k == RESK_Sym && FIsHead(n.a[2], "lambda"))
            {
                // a look-alike of an engine event the engine never calls
                const std::string& nm = n.a[1].atom;
                if (r.fNew && (nm.rfind("on-", 0) == 0 || (nm.rfind("can-", 0) == 0 && nm.back() == '?')))
                {
                    std::vector<std::string> aev;
                    for (int s = 0; s < 64; ++s)
                        if (const char* sz = SzSpliceEventName(s)) aev.push_back(sz);
                    const std::string sug = Closest(nm, aev);
                    setReported.insert(&n.a[1]);
                    Add(n.a[1], SPLICESEV_Warning, "unknown-event",
                        "'" + nm + "' is not an engine event: the engine never calls it" + (sug.empty() ? std::string() : " (did you mean " + sug + "?)"),
                        sug.empty() ? std::string() : "Replace with " + sug, sug);
                }
                Atom(n.a[1]);
                Lambda(n.a[2], psc, r.v);
                return;
            }
        }
        Expr(n, psc);
    }

    void Run(const std::string& text)
    {
        SpliceLegend(text, legend);
        SpliceTableNames(anameTable);
        if (ctx.fEngineTables) peo = &EngineOpts();
        std::vector<NODE> atop;
        try
        {
            PARSER p{text};
            while (!p.FEnd()) atop.push_back(p.Node());
        }
        catch (const std::exception& e)
        {
            // "line L:C: what"
            SPLICEISSUE d;
            const char* sz = e.what();
            if (std::sscanf(sz, "line %d:%d:", &d.line, &d.col) < 2) d.line = 1, d.col = 1;
            d.lineEnd = d.line;
            d.colEnd = d.col;
            d.sev = SPLICESEV_Error;
            d.code = "syntax";
            const char* pmsg = std::strchr(sz, ' ');
            pmsg = pmsg ? std::strchr(pmsg + 1, ' ') : nullptr;
            d.message = pmsg ? pmsg + 1 : sz;
            if (d.message.find("without its ')'") != std::string::npos)
            {
                d.fixLabel = "Add the missing )";
                d.fixText = ")";
                d.fFixAppend = true;
            }
            a.push_back(d);
            return;
        }
        std::set<int> setmod;
        for (const NODE& t : atop)
        {
            if (FIsHead(t, "debug-info")) continue;
            if (FIsHead(t, "module") && t.a.size() >= 3)
            {
                if (!setmod.insert(std::atoi(t.a[1].atom.c_str())).second) Add(t.a[1], SPLICESEV_Error, "syntax", "module " + t.a[1].atom + " twice");
                Module(t);
                continue;
            }
            Add(t, SPLICESEV_Error, "syntax", "the text holds (debug-info N) and (module N code...) only");
        }
    }
};
}  // namespace

void SpliceDiagnose(const std::string& text, const SPLICEDIAGCTX& ctx, std::vector<SPLICEISSUE>& adiag)
{
    DIAGNOSER d{ctx, adiag};
    d.Run(text);
    std::stable_sort(adiag.begin(), adiag.end(), [](const SPLICEISSUE& x, const SPLICEISSUE& y) {
        return x.line != y.line ? x.line < y.line : x.col < y.col;
    });
}

std::string StrSpliceDiag(const SPLICEISSUE& d)
{
    static const char* const s_asz[3] = {"error", "warning", "hint"};
    std::string s = "line " + std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + s_asz[std::clamp(d.sev, 0, 2)] + ": " + d.message + " [" + d.code + "]";
    if (!d.fixLabel.empty()) s += " (quick fix: " + d.fixLabel + ")";
    return s;
}

void CountSpliceDiags(const std::vector<SPLICEISSUE>& a, int& cerr, int& cwarn, int& chint)
{
    cerr = cwarn = chint = 0;
    for (const SPLICEISSUE& d : a) (d.sev == SPLICESEV_Error ? cerr : d.sev == SPLICESEV_Warning ? cwarn : chint)++;
}

bool FApplySpliceFix(const SPLICEISSUE& d, std::string& text)
{
    if (d.fixLabel.empty()) return false;
    if (d.fFixAppend)
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        text += d.fixText + "\n";
        return true;
    }
    // the byte offsets of (line, col)
    auto offset = [&](int line, int col) -> std::size_t {
        std::size_t i = 0;
        for (int l = 1; l < line; ++l)
        {
            i = text.find('\n', i);
            if (i == std::string::npos) return std::string::npos;
            ++i;
        }
        return i + std::size_t(std::max(col, 1) - 1);
    };
    if (d.fFixLines)
    {
        const std::size_t i0 = offset(d.line, 1);
        std::size_t i1 = offset(d.lineEnd + 1, 1);
        if (i0 == std::string::npos) return false;
        if (i1 == std::string::npos) i1 = text.size();
        text.erase(i0, i1 - i0);
        return true;
    }
    const std::size_t i0 = offset(d.line, d.col), i1 = offset(d.lineEnd, d.colEnd);
    if (i0 == std::string::npos || i1 == std::string::npos || i1 < i0 || i1 >= text.size()) return false;
    text.replace(i0, i1 - i0 + 1, d.fixText);
    return true;
}

int SpliceDiagSelfTest()
{
    // a small readable text with an error / warning seeded on known lines
    const std::string text =
        "; group 1 test\n"                                                  // 1
        "(debug-info 0)\n"                                                  // 2
        "(module 0\n"                                                       // 3
        "  ((begin\n"                                                       // 4
        "    (define door #obj:0)\n"                                        // 5
        "    (define open-door (lambda (who) ((<geto> door .add))))\n"      // 6
        "    (define on-start\n"                                            // 7
        "      (lambda (self)\n"                                            // 8
        "        (set! door (<find-object> 4090 self))\n"                   // 9
        "        (<seto!> door .positon (<vector> 0 0 0))\n"                // 10: unknown option (did you mean .position)
        "        (open-door)\n"                                             // 11: too few arguments
        "        (opne-door self)\n"                                        // 12: unknown function (did you mean open-door)
        "        (<rumble> 3)\n"                                            // 13: <rumble> needs 2
        "        (define unused-thing 5)\n"                                 // 14: unused
        "        (if #t (<rumble> 3 0.5) (<rumble> 1 0.1))\n"               // 15: unreachable else
        "        (<+> 1 \"two\")\n"                                         // 16: type
        "        (5 6)\n"                                                   // 17: not callable
        "        (<seto!> self .add-action $nobody)))\n"                    // 18: unknown object
        "    (define on-strat (lambda (self) (set! nothing-here 1))))))\n"  // 19: not an event; set! undefined
        "\n; ---- the names' numbers\n;= door=sym3000 open-door=sym3001\n";
    struct WANT
    {
        int line;
        const char* code;
        int sev;
    };
    const WANT awant[] = {
        {10, "unknown-option", SPLICESEV_Error}, {11, "arg-count", SPLICESEV_Error},  {12, "unknown-function", SPLICESEV_Error},
        {13, "arg-count", SPLICESEV_Error},      {14, "unused", SPLICESEV_Warning},   {15, "unreachable", SPLICESEV_Warning},
        {16, "type", SPLICESEV_Warning},         {17, "not-callable", SPLICESEV_Error}, {18, "unknown-object", SPLICESEV_Error},
        {19, "unknown-event", SPLICESEV_Warning}, {19, "set-undefined", SPLICESEV_Warning},
    };
    SPLICEDIAGCTX ctx;
    ctx.fEngineTables = false;
    ctx.pfnObjectExists = [](int oid) { return oid == 4090; };
    std::vector<SPLICEISSUE> a;
    SpliceDiagnose(text, ctx, a);
    int cfail = 0;
    for (const WANT& w : awant)
    {
        bool f = false;
        for (const SPLICEISSUE& d : a) f = f || (d.line == w.line && d.code == w.code && d.sev == w.sev);
        if (!f) { std::printf("splice-diag: line %d: no %s\n", w.line, w.code); ++cfail; }
    }
    // nothing else (each seeded problem once)
    for (const SPLICEISSUE& d : a)
    {
        bool f = false;
        for (const WANT& w : awant) f = f || (d.line == w.line && d.code == w.code);
        if (!f) { std::printf("splice-diag: unexpected %s\n", StrSpliceDiag(d).c_str()); ++cfail; }
    }
    // the quick fix of the misspelt option
    for (const SPLICEISSUE& d : a)
        if (d.code == "unknown-option")
        {
            std::string t = text;
            if (!FApplySpliceFix(d, t) || t.find("(<seto!> door .position (<vector>") == std::string::npos) { std::printf("splice-diag: the quick fix did not apply\n"); ++cfail; }
        }
    // a missing ')': at its line, with the fix
    std::vector<SPLICEISSUE> a2;
    SpliceDiagnose("(debug-info 0)\n(module 0\n  ((begin (define x 1)))\n", ctx, a2);
    if (a2.size() != 1 || a2[0].code != "syntax" || !a2[0].fFixAppend) { std::printf("splice-diag: a missing ')' not found\n"); ++cfail; }
    // (round 3) the original's never-defined function: a warning, not an error; defined in the module: nothing; a class
    // name; a guessed name
    {
        std::vector<SPLICEISSUE> a3;
        SpliceDiagnose("(debug-info 0)\n(module 0\n  ((begin (define on-start (lambda (self) (on-aim-setup-failed) (<find-objects> &guard self)"
                       " (<find-objects> &no-such-class self) (define ?guess-thing 1) ?guess-thing)))))\n", ctx, a3);
        int cnever = 0, cclass = 0, cother = 0;
        for (const SPLICEISSUE& d : a3)
            if (d.code == "never-defined" && d.sev == SPLICESEV_Warning) ++cnever;
            else if (d.code == "unknown-class") ++cclass;
            else ++cother;
        if (cnever != 1 || cclass != 1 || cother) { std::printf("splice-diag: round 3's names (never-defined %d, unknown class %d, other %d)\n", cnever, cclass, cother); ++cfail; }
        std::vector<SPLICEISSUE> a4;
        SpliceDiagnose("(debug-info 0)\n(module 0\n  ((begin (define on-aim-setup-failed (lambda #none (<rumble> 3 0.5)))"
                       " (define on-start (lambda (self) (on-aim-setup-failed))))))\n", ctx, a4);
        if (!a4.empty()) { std::printf("splice-diag: a defined on-aim-setup-failed still reported: %s\n", StrSpliceDiag(a4[0]).c_str()); ++cfail; }
    }
    if (cfail)
        for (const SPLICEISSUE& d : a) std::printf("  %s\n", StrSpliceDiag(d).c_str());
    return cfail;
}
