// The map editor (editor_impl.h; Sly 2: Sly2-Release/editor_cutscene.cpp): the cutscene editor and its Timeline dock.
// notes/CUTSCENES_2026-11-18.md, docs/MODDING.md "Cutscenes", notes/EDITOR_PLAN.md "Cutscenes".
//
// A cutscene is a script object (SCPT): its record's method options are its commands (Sly2-Release/cutscene.h). The
// timeline shows them as clips on tracks (the camera, the dialogue, each character, effects, sound, events, the flow),
// at the times a hidden run of the game measured (the trace: cutscene.h --cutscene-trace), or estimated from their
// arguments until it has. Scrubbing puts the traced camera and the moved objects where they were at that time; the
// ghosts show where the characters go. Edits are the record's bytes (the project's undo steps): an argument changed,
// a clip moved (the wait before it) or resized (its own seconds), a command added / deleted, a camera shot set from the
// view, a new cutscene made; the game plays them as it plays its own.
#include "editor_impl.h"

#if CANE_EDITOR_3D && EDGAME_SLY2

#include "cutscene.h"
#include "aseg.h"
#include "asega.h"
#include "bone.h"
#include "so.h"
#include "vecmat.h"
#include "core/spawn.h"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cfloat>

namespace edimpl
{

std::set<int> OidsLookedUp(const std::string& text);   // editor_script.inc

namespace
{
const char* SzTaskState(int n)
{
    static const char* s_a[] = {"locked", "available", "done", "done and closed", "activate", "deactivate"};
    return n >= 0 && n <= 5 ? s_a[n] : "?";
}
// a value's bytes after its kind word (brx.cpp RefReadOption)
int CbOfValueKind(int k)
{
    switch (k)
    {
    case 0: return 1;
    case 1: case 5: case 6: return 4;
    case 2: case 9: return 12;
    case 7: case 14: case 15: case 16: case 17: case 19: case 20: case 21: case 23: return 2;
    case 10: return 8;
    default: return ((k & ~0x8000) >> 12) == 1 ? 4 : 0;
    }
}
bool FFloatKind(int k) { return k == 1 || k == 2 || k == 9 || k == 10; }
// at most cb bytes of a UTF-8 text, not cutting a letter (the translated names)
std::string Utf8Cut(const std::string& s, std::size_t cb)
{
    if (s.size() <= cb) return s;
    while (cb > 0 && (static_cast<unsigned char>(s[cb]) & 0xC0) == 0x80) --cb;
    return s.substr(0, cb);
}
constexpr float c_dtFrame = 1.0f / 60.0f;
}  // namespace

// A value of a command's option in its part
struct CSVAL
{
    int w = -1;                 // its kind word (0x8000: a list)
    std::size_t ib = 0;         // its first value's bytes in the part
    int cb = 0;                 // each value's size
    int c = 1;                  // values (a list: its count)
};

struct CSCMD
{
    int ic = 0;                 // the command's index in the script (0: the opening, made by the loader)
    int kind = 0, b = 0;
    int32_t an[18]{};
    int ifield = -1;            // its option (EDITFK_OptionArray; -1: the implicit opening command)
    const cut::BUILDER* pbuilder = nullptr;
    std::string rel;            // its part, the S16 eopid's offset there and the option's bytes after it
    std::size_t ibOpt = 0, cbOpt = 0;
    std::vector<CSVAL> aval;
    int icBlock = -1;           // the block it is in (its opening command)
    int depth = 0;
    // its place on the timeline
    float t0 = 0.0f, t1 = 0.0f;         // shown from / to
    float tHold = 0.0f;                 // how long it holds its track (0: the next starts at once)
    float tExec = 0.0f;                 // when its track runs it (a shot's end is shown at its arrival: t0 moved)
    bool fMeasured = false, fRan = false, fEstimate = false;
    int itrack = -1, lane = 0;          // its track and its row there (clips that overlap stack)
    int oidActor = -1;                  // the object it acts on (-1: none / the track's)
    int icPair = -1;                    // a letterbox's "Pause game" off (its clip's end)
    float G(uint32_t ib) const { float g; std::memcpy(&g, &an[(ib - 0x10) / 4], 4); return g; }
    int N(uint32_t ib) const { return an[(ib - 0x10) / 4]; }
};

struct CSTRACK
{
    std::string name;
    cut::TRACKK tk = cut::TK_Flow;
    int oid = -1;
    std::vector<int> aicmd;
    int clane = 1;
};

struct CSSCENE
{
    int oid = 0;
    std::string name;
    int cCmd = 0, cLine = 0, cShot = 0, cPause = 0;
};

// An object in the trace: oid, class, rank among the world's objects of that oid and class in aploAll's order (oid -1:
// none)
using TRACEID = std::array<int, 3>;
inline TRACEID TraceIdOf(const nlohmann::json& j)
{
    if (j.is_array() && j.size() >= 3) return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
    return {-1, 0, 0};
}

// The traced scene (cutscene.h's trace JSON)
struct CSTRACE
{
    bool fOk = false, fStopped = false, fLeft = false, fTimedOut = false;
    float tEnd = 0.0f;
    struct CAMKEY { float t; glm::vec3 pos; glm::mat3 mat; float fov; };
    std::vector<CAMKEY> acam;
    struct OBJKEY { float t; glm::vec3 pos; glm::quat q; bool fIn; };
    // (depth > 0: a model under another object, its keys relative to its parent's record; fStill: it stood still in
    // the scene, kept only where the editor's world has it elsewhere)
    struct OBJ { int oid = 0, cid = 0, rank = 0, depth = 0; bool fStill = false; std::vector<OBJKEY> akey; TRACEID parent{{-1, 0, 0}}; };
    std::vector<OBJ> aobj;
    // the models the game does not draw at some time (out of the world, inactive, faded out ...): when it does
    struct DRAWN
    {
        int oid = 0, cid = 0, rank = 0;
        std::vector<std::pair<float, bool>> akey;   // (t, drawn from t on)
        bool FAt(float t) const
        {
            bool f = akey.empty() || akey[0].second;
            for (const auto& [tk, fk] : akey)
                if (tk <= t + 1e-4f) f = fk;
            return f;
        }
    };
    std::vector<DRAWN> adrawn;
    // the models' parts the game shows / hides at run time (ALO aglobShown) from a time on
    struct PARTS
    {
        int oid = 0, cid = 0, rank = 0;
        std::vector<std::pair<float, std::vector<std::pair<int32_t, bool>>>> akey;
        const std::vector<std::pair<int32_t, bool>>* PaAt(float t) const
        {
            const std::vector<std::pair<int32_t, bool>>* pa = akey.empty() ? nullptr : &akey[0].second;
            for (const auto& k : akey)
                if (k.first <= t + 1e-4f) pa = &k.second;
            return pa;
        }
    };
    std::vector<PARTS> aparts;
    // where the game drew each model in the camera's view (every few seconds; cutsurvey's check)
    struct SEEN { int oid = 0, cid = 0, rank = 0; std::vector<std::pair<float, glm::vec3>> at; };
    std::vector<SEEN> aseen;
    struct SPAN
    {
        int id = 0, on = -1, oncid = -1, onrank = 0;   // (the object it plays on: oid, class, rank among those)
        float t0 = 0, t1 = 0, length = 0;
        float at = 0;                                // the clip's time at t0
        bool fAmbient = false;                       // playing before the scene started (an idle)
        struct KEY { float t, at, rate; int id; };
        std::vector<KEY> akey;                       // the clip's time (and, for a blend, the clip) from t on
        // the clip and its time at t
        float TClip(float t, int* pid = nullptr) const
        {
            float tc = at + (t - t0);
            if (pid) *pid = id;
            for (const KEY& k : akey)
                if (k.t <= t + 1e-4f)
                {
                    tc = k.at + k.rate * (t - k.t);
                    if (pid) *pid = k.id;
                }
            return std::max(0.0f, tc);
        }
    };
    std::vector<SPAN> aline, aanim;
    std::vector<std::array<float, 3>> acmd;   // (run, ic, t)
    struct RUN { int block = 0, parent = -1; float t0 = 0, t1 = -1; };
    std::vector<RUN> arun;
    int oidPlayer = -1;
};

struct AKDATA;   // (editor_cutclip.inc: an animation's data as the level stores it)

class CCutsceneEditor
{
public:
    explicit CCutsceneEditor(CEditor& ed) : e(ed) {}
    CEditor& e;
    bool fOpen = false;

    void Draw();
    void Frame();
    bool FLooking() const { return m_fLookActive; }
    bool FVerb(const std::string& verb, const std::string& arg);
    void Close();

private:
    // the world's cutscenes
    void BuildList();
    std::vector<CSSCENE> m_ascene;
    const SW* m_pswList = nullptr;
    char m_filter[64] = "";
    bool m_fScenesOnly = true;
    // the open one
    void Open(int oid);
    void Decode();
    void Layout();
    void BuildTracks();
    void BuildTrackList();
    std::string Label(const CSCMD& c) const;
    std::string NameOf(int oid) const;
    LO* PloOfOid(int oid) const;
    // the trace's object: the rank-th of the world's objects of that oid and class (aploAll's order, as the game
    // ranks them: the proxies' spare instances share their oid); RankOf an object's rank
    LO* PloOf(int oid, int cid, int rank) const;
    LO* PloOf(const TRACEID& id) const { return PloOf(id[0], id[1], id[2]); }
    int RankOf(const LO* plo) const;
    mutable std::map<std::pair<int, int>, std::vector<LO*>> m_mpoidcid;   // the world's objects by oid and class
    mutable std::unordered_map<const LO*, int> m_mprank;
    mutable const SW* m_pswRank = nullptr;
    float LineLength(int id) const;
    float AnimLength(int oid) const;
    void WhoStarts();
    int m_oid = 0;
    LO* m_pscpt = nullptr;
    const SW* m_pswSeen = nullptr;
    std::vector<CSCMD> m_acmd;
    std::vector<CSTRACK> m_atrack;
    std::vector<std::string> m_astarts, m_aeffects;
    std::string m_decodeNote;
    float m_tEnd = 1.0f;
    mutable std::unordered_map<int, LO*> m_mpoid;
    mutable const SW* m_pswOid = nullptr;
    std::map<int, float> m_mplineLength;
    bool m_fLinesRead = false;
    // the trace (a hidden game run)
    void StartTrace();
    void PollTrace();
    bool FReadTrace(const std::string& path);
    std::string TraceDir() const;
    CSTRACE m_trace;
    bool m_fTracePending = false, m_fTraceWanted = true;
    int m_measureMode = 0, m_taskMeasure = 0;
    int RootStarter() const;
    std::string Signature() const { std::string s; for (const CSCMD& c : m_acmd) s += char(c.kind + 1); return s; }
    std::string m_sigPending, m_sigTrace;
    bool m_fRemeasure = false;
    double m_tEdit = 0.0;
    std::size_t m_cHistory = 0, m_cRedo = 0;
    int m_pollTrace = 0, m_cReadFailed = 0;
    double m_tTraceStart = 0.0;
    std::string m_tracePath, m_traceNote;
    // the playhead, scrubbing, the scene at the playhead
    void ApplyScene();
    void RestoreScene();
    bool FCameraAt(float t, CAMERA& cam) const;
    bool FObjectAt(const CSTRACE::OBJ& o, float t, glm::vec3& pos, glm::quat& q, bool& fIn) const;
    bool FObjectWorld(ALO* palo, const TRACEID* pparent, glm::mat4& W, glm::mat4* pWparent = nullptr) const;
    void DrawGhosts();
    // characters posed at the playhead (step 3: the clip playing on each, as the game poses it)
    using POSE = std::vector<std::pair<XF*, glm::mat4>>;   // each record's world relative to its object record
    bool FAnimOn(int oid, float t, LO*& plAseg, float& tClip) const;
    // every clip playing at t on the object or an object under it (its idle from before the scene too), in the order
    // they started: (the object it plays on, the clip, its time); the game layers them (a body clip, a face's, the
    // mouth's, a part's own ...)
    bool FAnimsOn(const LO* plo, float t, std::vector<std::tuple<LO*, LO*, float>>& aclip) const;
    std::vector<LO*> m_aploSpan;                           // (each animation's object, ForgetPoses)
    std::vector<ALO*> m_apaloPoseRoot;                     // (the trees clips play in: posed whole)
    const SW* m_pswPoseRoot = nullptr;
    LO* PlAsegOf(LO* plo, int id) const;
    const POSE* PposeAt(ALO* palo, float t);
    void PoseCharacters();
    void ForgetPoses();
    std::map<std::pair<const ALO*, std::string>, POSE> m_mppose;   // (by the clips and their frames)
    std::set<ALO*> m_setPosed;
    std::vector<std::array<int, 3>> m_aoidAnimated;        // the trace's objects animations play on (oid, class, rank)
    // the objects the game has out of the world at the playhead (shown / hidden by the scene, the proxies' spare
    // instances ...): their scene entries are not drawn (CEditor::m_aiSceneHidden)
    void HideOutOfWorld(const std::set<const LO*>& setTree, const std::set<const LO*>& setSelf);
    // cutsurvey: every model the game drew in the camera's view against the preview at the same times (in the
    // editor's world, a model in its scene, not left out, posed when a clip plays on it, at the game's place)
    void Survey();
    std::set<const LO*> m_setOutLast, m_setNotDrawnLast;   // (ApplyScene's last: what it left out and why)
    bool m_fPoses = true;
    float m_t = 0.0f, m_tApplied = -1.0f;
    bool m_fPlaying = false, m_fLoop = false, m_fLookThrough = true, m_fPreview = true, m_fGhosts = true;
    float m_tLoop0 = 0.0f, m_tLoop1 = 0.0f;
    bool m_fLookActive = false;
    CAMERA m_camSaved;
    // the picture while looking through: the game's 16:9 frame fitted into the 3D view's free part (between the tool
    // strip, the right column, the menu and the Timeline dock), as fractions of the window (fFraction) or pixels
    glm::vec4 RcPicture(bool fFraction) const;
    glm::vec4 m_rcLook{0.0f};
    std::unordered_map<XF*, std::pair<ALO*, glm::mat4>> m_mpOrig;   // the records the preview moved: their own local matrices
    std::unordered_map<ALO*, std::vector<std::pair<int32_t, bool>>> m_mpPartsOrig;   // the models whose parts it set as the game had them: theirs
    // the view
    void DrawToolbar();
    void DrawTracks(float w, float h);
    void DrawInspector();
    void DrawAddMenu(int icBefore);
    float m_pps = 60.0f, m_tView = 0.0f;   // pixels per second, the view's first second
    int m_isel = -1;
    int m_itab = 0, m_itabWanted = -1;     // the scene's inspector tab (Scene / Start it / Add / Characters)
    int m_dragKind = 0;                    // 1 the clip moved, 2 its end, 3 the playhead, 4 the loop range
    float m_dragT0 = 0.0f, m_dragDt = 0.0f;
    bool m_fByRun = false;                 // the tracks by the script's own tracks (runners), not by kind
    bool m_fFitPending = true;
    // edits
    bool FPatchValue(CSCMD& c, int iarg, int icomp, double v, const std::string& label);
    bool FSetSlot(CSCMD& c, int ib, double v, const std::string& label);
    int IargOfSlot(const CSCMD& c, int ib, int* picomp = nullptr) const;
    bool FDeleteCommand(int i);
    bool FInsertCommand(int iBefore, int kind, int variant, const std::vector<std::pair<int, double>>& aset, const std::string& label);
    bool FMoveClip(int i, float dt);
    bool FResizeClip(int i, float dur);
    int IbDuration(const CSCMD& c) const;
    bool FShotFromView(int i);
    void PlayInGame(float t);
    int CreateCutscene(std::string& error);
    bool FValueBytes(int kind, int otyp, double v, std::vector<uint8_t>& ab) const;
    // character paths (step 2)
    bool FSetPathPoints(LO* ppath, const std::vector<glm::vec3>& aw, const std::string& label, int curved = -1);   // (curved -1: as it is)
    bool FPathPoints(LO* ppath, std::vector<glm::vec3>& aw) const;
    int NewPath(const std::vector<glm::vec3>& aw, std::string& error, bool fCurved = false);
    int IobjMarker() const;                         // a marker to copy (a "Go to" target of the level's scripts)
    static std::vector<glm::vec3> CurvePoints(const std::vector<glm::vec3>& aw, int cPerSeg);   // the curve through them, sampled
    int InsertPoint(float t) const;                 // the main track's command a new track starting at t goes before
    float TFreeAt(int iBefore) const;               // the time the main track reaches that command
    bool FNewCharacterPath(int oidActor, float t, const std::vector<glm::vec3>& aw, bool fWalk, float speed, int oidAnim, std::string& error,
                           bool fCurved = false, float faceDeg = NAN);
    glm::vec3 ActorPosAt(int oidActor, float t, float* pheading) const;
    void DrawPathPanel();
    bool FCharacterCombo(const char* label, int& oid);
    int m_oidPathActor = -1, m_oidArriveAnim = -1;
    bool m_fPathWalk = true, m_fPathCurved = false, m_fPathFace = false;
    float m_pathFaceDeg = 0.0f;
    float m_pathSpeed = 1.0f, m_pathLength = 400.0f;
    // camera keys (step 4): a key = a camera move's end view at a time; the move to it eased
    bool FNewCameraKey(float t, float blend, int ease, std::string& error);
    int IcShotOf(int icEnd) const;                  // a "Camera move end"'s move (its 0x38)
    bool FEaseCombo(const char* id, int& ease);
    float m_keyBlend = 1.0f;
    int m_keyEase = 3;
    // cinematic animations' channels and keys as sub-tracks (step 4)
    struct SUBROW { std::string name; std::vector<float> at; bool fEvents = false; int oid = INT_MIN; };
    std::vector<SUBROW> SubrowsOf(const CSCMD& c) const;
    ASEG* PasegOfClip(const CSCMD& c) const;
    // round 3: the clips' keys edited (editor_cutclip.inc: the ASEG's bytes in the level), new clips from poses
    bool FClipData(ASEG* paseg, AKDATA& d, std::string& rel, std::size_t& ib, std::size_t& cb, std::string& error) const;
    bool FWriteClip(ASEG* paseg, const AKDATA& d, const std::string& label);
    bool FEditClipKeys(const CSCMD& c, const SUBROW& row, float t, float tTo, bool fAdd, float tStretch = 0.0f);
    void DrawClipPanel(const CSCMD& c);
    void DrawNewClipPanel();
    // round 3: the presentation track (editor_cutpresent.inc): letterbox, title cards, captions, fades
    struct FADEINFO { float tIn = 0, tHold = 0, tOut = 0; uint8_t rgba[4] = {0, 0, 0, 0x80}; };
    bool FFadeClip(const CSCMD& c) const;
    bool FFadeOf(int oidFade, FADEINFO& fi) const;
    bool FSetFadeOption(int oidFade, int eopid, const void* pv, const std::string& label);
    void LayoutPresentation();
    bool FAddSpanAtTime(float t, float len, int kindOn, const std::vector<std::pair<int, double>>& aOn, int kindOff, const std::vector<std::pair<int, double>>& aOff,
                        const std::string& label);
    bool FAddPresentation(int kind, float t, float len, const std::string& text, const float rgb[3], float tIn, float tOut, std::string& error);
    void DrawPresentPanel();
    void DrawPresentInspector(CSCMD& c);
    bool FResizePresentation(int i, float dt);
    bool FPresentEdge(const CSCMD& c) const;
    bool FFadeAt(float t, float rgba[4]) const;
    // (SafeHouse addition: the custom presentation, the rule's custom_presentation: captions in the scene's pause,
    // title cards of their own text, Sly2-Release/cutpresent.cpp)
    bool FCustomPresent() const { return RuleOf().customPresentation; }
    void SetCustomPresent(bool f);
    static bool FTitleCardClip(const CSCMD& c) { return c.kind == 0x53 && ((c.b & ~1) == cut::SCPTB_TITLECARD || c.N(0x18) == cut::SCPTB_TITLECARD); }
    int m_presentKind = 0;
    float m_presentLen = 2.0f, m_presentIn = 0.5f, m_presentOut = 0.5f, m_presentRgb[3] = {0, 0, 0};
    char m_presentText[256] = "";
    int m_fadePendingOid = -1;
    float m_fadeRgbPending[3] = {0, 0, 0};
    struct CLIPPOSE { int oidActor; int oidClip; float tClip; float at; };
    std::vector<CLIPPOSE> m_apose;
    int NewClipFromPoses(std::string& error);
    int m_keyRow = 0;                          // the selected key: its sub-row and clip time (-1: none)
    float m_keyT = -1.0f;
    int m_keyDragRow = -1;                     // a key dragged on its row: from m_keyDragT0 by m_dragDt
    float m_keyDragT0 = 0.0f;
    // step 5: when it plays and what a skip does (the package's rule, mod.json "cutscenes"), events at a time, the
    // previews of the subtitles / letterbox / title card at the playhead
    cane::mods::CutsceneRule RuleOf() const;
    void SetRule(const cane::mods::CutsceneRule& r);
    void DrawRulePanel();
    void DrawEventPanel();
    bool FAddAtTime(float t, int kind, int variant, const std::vector<std::pair<int, double>>& aset, const std::string& label);
    void DrawPreviewOverlay();
    int m_eventKind = 0, m_eventValue = 1, m_eventObject = -1;
    // step 6: the splice that starts it (the task's own script: editor_scriptdiag.inc FWriteSceneStarter)
    void DrawStarterPanel();
    int m_oidStarterTask = -1, m_starterState = 1, m_starterKind = 0, m_starterEvent = 1;
    bool FWriteStarter(int kind, int oid, int value, const glm::vec3& pos, std::vector<std::string>& adiag, std::string& error);
    std::vector<std::string> m_astarterDiag;
    std::string m_starterNote;
    // recorded play sessions (step 2)
    void RecordSession();
    bool FImportSession(const std::string& path, int oidActor, float t, std::string& error);
    std::string SessionPath() const { return TraceDir() + "/session.json"; }
    bool m_fRecording = false;
    int m_pollRecord = 0;
};

// ---- the world's cutscenes

void CCutsceneEditor::BuildList()
{
    m_ascene.clear();
    m_pswList = e.m_psw.get();
    for (const auto& up : e.m_psw->aploAll)
    {
        LO* plo = up.get();
        if (!FIsLoDerivedFrom(plo, EDCID(0x72))) continue;
        CSSCENE s;
        s.oid = plo->oid;
        int kind = 0, b = 0;
        int32_t an[18];
        for (int i = 0; FScptCommand(plo, i, kind, b, an); ++i)
        {
            ++s.cCmd;
            s.cLine += kind == 0x45 || kind == 0x3B;
            s.cShot += kind == 0x38;
            s.cPause += kind == 0x12 && an[0];
        }
        // its name: the first line's subtitle, else what the outliner calls it
        std::string what;
        for (int i = 0; FScptCommand(plo, i, kind, b, an); ++i)
            if (kind == 0x45)
            {
                what = e.StrText(an[0]);
                break;
            }
        if (what.size() > 48) what = what.substr(0, 45) + "...";
        s.name = std::to_string(s.oid) + (what.empty() ? "" : "  \"" + what + "\"");
        m_ascene.push_back(std::move(s));
    }
    std::sort(m_ascene.begin(), m_ascene.end(), [](const CSSCENE& a, const CSSCENE& b) {
        const int ka = a.cPause * 4 + a.cLine * 2 + a.cShot, kb = b.cPause * 4 + b.cLine * 2 + b.cShot;
        return ka != kb ? ka > kb : a.oid < b.oid;
    });
}

LO* CCutsceneEditor::PloOfOid(int oid) const
{
    if (oid < 0) return nullptr;
    if (m_pswOid != e.m_psw.get())
    {
        m_mpoid.clear();
        m_pswOid = e.m_psw.get();
        for (const auto& up : e.m_psw->aploAll)
        {
            auto [it, f] = m_mpoid.try_emplace(up->oid, up.get());
            if (!f && e.FHasMatrix(up.get()) && !e.FHasMatrix(it->second)) it->second = up.get();
        }
    }
    const auto it = m_mpoid.find(oid);
    return it == m_mpoid.end() ? nullptr : it->second;
}

LO* CCutsceneEditor::PloOf(int oid, int cid, int rank) const
{
    if (oid < 0) return nullptr;
    RankOf(nullptr);   // (the index for this load)
    if (cid >= 0)
    {
        const auto it = m_mpoidcid.find({oid, cid});
        if (it != m_mpoidcid.end() && rank >= 0 && std::size_t(rank) < it->second.size()) return it->second[std::size_t(rank)];
        if (it != m_mpoidcid.end() || rank > 0) return nullptr;   // (a clone the game made at run time: not in the editor's world)
    }
    // (an older trace without classes: the oid's object)
    return rank == 0 ? PloOfOid(oid) : nullptr;
}

int CCutsceneEditor::RankOf(const LO* plo) const
{
    if (m_pswRank != e.m_psw.get())
    {
        m_mpoidcid.clear();
        m_mprank.clear();
        m_pswRank = e.m_psw.get();
        for (const auto& up : e.m_psw->aploAll)
        {
            auto& a = m_mpoidcid[{up->oid, int(up->Cid())}];
            m_mprank[up.get()] = int(a.size());
            a.push_back(up.get());
        }
    }
    if (!plo) return 0;
    const auto it = m_mprank.find(plo);
    return it == m_mprank.end() ? 0 : it->second;
}

std::string CCutsceneEditor::NameOf(int oid) const
{
    if (oid == -1) return T("(none)");
    LO* plo = PloOfOid(oid);
    if (!plo) return std::to_string(oid);
    std::string s = e.NameOfObject(plo);
    if (s.find(std::to_string(oid)) == std::string::npos) s += " " + std::to_string(oid);
    return s;
}

float CCutsceneEditor::LineLength(int id) const
{
    const auto it = m_mplineLength.find(id);
    return it == m_mplineLength.end() ? 2.0f : it->second;
}

float CCutsceneEditor::AnimLength(int oid) const
{
    LO* plo = PloOfOid(oid);
    if (plo && FIsLoDerivedFrom(plo, EDCID(0x6D)))
        if (const ASEGD* pd = PasegdOf(static_cast<ASEG*>(plo))) return pd->tMax;
    return 1.0f;
}

void CCutsceneEditor::Open(int oid)
{
    RestoreScene();
    m_oid = oid;
    m_isel = -1;
    m_t = 0.0f;
    m_tApplied = -1.0f;
    m_fPlaying = false;
    m_trace = CSTRACE();
    ForgetPoses();
    m_traceNote.clear();
    m_fFitPending = true;
    if (!m_fLinesRead)
    {
        m_fLinesRead = true;
        std::vector<DLGLINE> al;
        std::vector<std::string> alang;
        std::string error;
        if (e.FParseDialogue(al, alang, error))
            for (const DLGLINE& l : al)
                if (!l.atLength.empty()) m_mplineLength[l.id] = l.atLength[0];
    }
    Decode();
    WhoStarts();
    // its trace from before (the same project state), else a new one
    m_tracePath = TraceDir() + "/" + std::to_string(oid) + ".json";
    m_cHistory = e.m_proj.history().size();
    m_cRedo = e.m_proj.redoHistory().size();
    m_fRemeasure = false;
    if (m_fTraceWanted) StartTrace();
}

// The script's commands: the live ones (the loader's constants) with each argument read from the record's bytes
void CCutsceneEditor::Decode()
{
    m_acmd.clear();
    m_decodeNote.clear();
    m_pscpt = PloOfOid(m_oid);
    m_pswSeen = e.m_psw.get();
    if (!m_pscpt || !FIsLoDerivedFrom(m_pscpt, EDCID(0x72))) { m_pscpt = nullptr; m_decodeNote = T("no such script in this world"); return; }
    const int c = CScptCommands(m_pscpt);
    for (int i = 0; i < c; ++i)
    {
        CSCMD cmd;
        cmd.ic = i;
        FScptCommand(m_pscpt, i, cmd.kind, cmd.b, cmd.an);
        m_acmd.push_back(cmd);
    }
    // the record's method options that build commands, in order: command 1, 2, ...
    std::vector<int> aifield;
    if (const auto it = e.m_mpobj.find(m_pscpt); it != e.m_mpobj.end())
        for (int i : it->second.aiarr)
            if (cut::PbuilderOfEopid(e.m_rec.afield[std::size_t(i)].eopid)) aifield.push_back(i);
    if (int(aifield.size()) + 1 != c) m_decodeNote = T("its commands do not match its record (read only)");
    for (std::size_t k = 0; k < aifield.size() && k + 1 < m_acmd.size(); ++k)
    {
        CSCMD& cmd = m_acmd[k + 1];
        const EDITFIELD& f = e.m_rec.afield[std::size_t(aifield[k])];
        cmd.ifield = aifield[k];
        cmd.pbuilder = cut::PbuilderOfEopid(f.eopid);
        if (!cmd.pbuilder || cmd.pbuilder->kind != cmd.kind) { m_decodeNote = T("its commands do not match its record (read only)"); cmd.ifield = -1; continue; }
        std::size_t ib = 0;
        if (f.ib < 2 || !e.FLocate(f.ib - 2, (f.ibEnd - f.ib) + 2, cmd.rel, ib)) { cmd.ifield = -1; continue; }
        cmd.ibOpt = ib;
        cmd.cbOpt = (f.ibEnd - f.ib) + 2;
        const std::vector<uint8_t>* pab = e.m_proj.partRef(cmd.rel);
        if (!pab || ib + cmd.cbOpt > pab->size()) { cmd.ifield = -1; continue; }
        const uint8_t* pb = pab->data();
        std::size_t i = ib + 2;
        const std::size_t iEnd = ib + cmd.cbOpt;
        for (int j = 0; j < cmd.pbuilder->cArgs && i + 2 <= iEnd; ++j)
        {
            CSVAL v;
            uint16_t w;
            std::memcpy(&w, pb + i, 2);
            i += 2;
            v.w = w;
            int k = w;
            if (w & 0x8000)
            {
                k = w & 0x7FFF;
                v.c = i < iEnd ? pb[i++] : 0;
            }
            v.cb = CbOfValueKind(k);
            v.ib = i;
            i += std::size_t(v.cb) * std::size_t(v.c);
            cmd.aval.push_back(v);
        }
        // the arguments from the record (an edit shows before the world is loaded again)
        for (std::size_t j = 0; j < cmd.aval.size() && j < cmd.pbuilder->alay.size(); ++j)
        {
            const SCPTARGSLOT& as = cmd.pbuilder->alay[j];
            const CSVAL& v = cmd.aval[j];
            const int k = v.w & 0x7FFF;
            if (as.ib == -1 || v.cb == 0 || v.c < 1 || (v.w & 0x8000)) continue;
            auto rd = [&](std::size_t ibv) -> double {
                if (FFloatKind(k)) { float g; std::memcpy(&g, pb + ibv, 4); return g; }
                if (v.cb == 1) return pb[ibv];
                if (v.cb == 2) { int16_t n; std::memcpy(&n, pb + ibv, 2); return n; }
                int32_t n;
                std::memcpy(&n, pb + ibv, 4);
                return n;
            };
            if (as.ib == -2) { cmd.b = int(rd(v.ib)); continue; }
            if (as.otyp == 2 && v.cb == 12)
            {
                for (int q = 0; q < 3; ++q)
                {
                    float g;
                    std::memcpy(&g, pb + v.ib + 4 * std::size_t(q), 4);
                    if (as.fRad) g *= 0.017453294f;
                    std::memcpy(&cmd.an[(as.ib - 0x10) / 4 + q], &g, 4);
                }
                continue;
            }
            const double d = rd(v.ib);
            if (as.otyp == 1 || FFloatKind(k))
            {
                const float g = float(d);
                std::memcpy(&cmd.an[(as.ib - 0x10) / 4], &g, 4);
            }
            else cmd.an[(as.ib - 0x10) / 4] = int32_t(d);
        }
    }
    // the blocks
    std::vector<int> astack;
    for (CSCMD& cmd : m_acmd)
    {
        cmd.icBlock = astack.empty() ? -1 : astack.back();
        cmd.depth = int(astack.size());
        if (cmd.kind == 0) astack.push_back(cmd.ic);
        else if (cmd.kind == 1 && !astack.empty()) { astack.pop_back(); cmd.depth = int(astack.size()); }
    }
    for (CSCMD& cmd : m_acmd)
    {
        const cut::CMDDESC& d = cut::DescOfKind(cmd.kind);
        cmd.oidActor = d.ibActor >= 0 ? cmd.N(uint32_t(d.ibActor)) : -1;
        if (cmd.oidActor == -1 && d.tk == cut::TK_Actor && cmd.icBlock >= 0) cmd.oidActor = m_acmd[std::size_t(cmd.icBlock)].an[0];
    }
    Layout();
    BuildTracks();
}

// What starts it and what it does to the world (words)
void CCutsceneEditor::WhoStarts()
{
    m_astarts.clear();
    m_aeffects.clear();
    if (!m_pscpt) return;
    // objects whose options name it (a use's "starts a script", ...)
    for (const EDITFIELD& f : e.m_rec.afield)
    {
        if (f.fk != EDITFK_Option || !f.plo || f.plo == m_pscpt) continue;
        const OPTTYPE t = OptType(f.k);
        if (!(std::string(t.name) == "object id" || FOidOption(f.eopid) || t.cb == 4)) continue;
        int n = 0;
        if (e.FOptionInt(f.plo, f.eopid, n) && n == m_oid && (std::string(t.name) == "object id" || FOidOption(f.eopid) || f.eopid == EDEOPID(1070)))
            m_astarts.push_back(e.StrLo(f.plo) + ": " + (SzOptionName(f.eopid) ? SzOptionName(f.eopid) : StrOption(f.eopid)));
    }
    // other scripts' "Start script" commands
    for (const auto& up : e.m_psw->aploAll)
    {
        if (!FIsLoDerivedFrom(up.get(), EDCID(0x72)) || up.get() == m_pscpt) continue;
        int kind = 0, b = 0;
        int32_t an[18];
        for (int i = 0; FScptCommand(up.get(), i, kind, b, an); ++i)
            if (kind == 0x0B && an[0] == m_oid)
            {
                m_astarts.push_back(std::string(T("script")) + " " + std::to_string(up->oid) + ": " + T("Start script") + " (" + T("command") + " " + std::to_string(i) + ")");
                break;
            }
    }
    // the splice scripts that look it up (they start it with its on-start method)
    for (const ce::GroupInfo& g : e.m_index.groups)
    {
        if (g.scriptsPart.empty()) continue;
        std::vector<uint8_t> ab;
        if (!e.m_proj.readPart(g.scriptsPart, ab)) continue;
        if (OidsLookedUp(std::string(ab.begin(), ab.end())).count(m_oid)) m_astarts.push_back(std::string(T("the splice scripts of")) + " " + g.name);
    }
    for (const CSCMD& c : m_acmd)
        switch (c.kind)
        {
        case 0x11: m_aeffects.push_back(std::string(T("task")) + " " + NameOf(c.an[0]) + " -> " + SzTaskState(c.an[1])); break;
        case 0x0B: m_aeffects.push_back(std::string(T("starts script")) + " " + std::to_string(c.an[0])); break;
        case 0x2C: m_aeffects.push_back(std::string(T("shows")) + " " + NameOf(c.an[0])); break;
        case 0x2D: m_aeffects.push_back(std::string(T("hides")) + " " + NameOf(c.an[0])); break;
        case 0x0D: m_aeffects.push_back(std::string(T("state machine of")) + " " + NameOf(c.an[0]) + " -> " + std::to_string(c.an[2])); break;
        case 0x16: if (c.an[0] != -1 && PloOfOid(c.an[0]) && FIsLoDerivedFrom(PloOfOid(c.an[0]), EDCID(0x29))) m_aeffects.push_back(T("warps the player")); break;
        case 0x4C: m_aeffects.push_back(std::string(T("music")) + " " + std::to_string(c.an[0])); break;
        default: break;
        }
}

// The commands' times: the trace's where it ran them, else estimated from their arguments
void CCutsceneEditor::Layout()
{
    const int n = int(m_acmd.size());
    for (CSCMD& c : m_acmd) c.t0 = c.t1 = c.tHold = 0.0f, c.fMeasured = c.fRan = false, c.fEstimate = false;
    auto matching = [&](int ic) {
        int d = 0;
        for (int i = ic + 1; i < n; ++i)
        {
            if (m_acmd[std::size_t(i)].kind == 0) ++d;
            else if (m_acmd[std::size_t(i)].kind == 1 && d-- == 0) return i;
        }
        return n;
    };
    // own lengths: shown (visual) and held (the track waits)
    auto own = [&](CSCMD& c, float& vis, float& hold) {
        vis = hold = 0.0f;
        const bool fWait = cut::DescOfKind(c.kind).wait == 2 || (cut::DescOfKind(c.kind).wait == 1 && c.b == 0);
        switch (c.kind)
        {
        case 0x23: case 0x33: vis = std::max(0.0f, c.G(0x10)); break;
        case 0x32: vis = 0.5f; break;
        case 0x53: case 0x2C: case 0x2D: vis = std::max(0.0f, c.G(0x14)); break;
        case 0x45: vis = LineLength(c.an[0]); break;
        case 0x0A: vis = AnimLength(c.an[0]); break;
        case 0x22: vis = AnimLength(c.an[0]); break;
        case 0x38: vis = c.G(0x40); break;
        case 0x44: vis = 3.0f; c.fEstimate = true; break;
        case 0x0D: if (fWait) { vis = 0.5f; c.fEstimate = true; } break;
        case 0x24: case 0x27: case 0x28: case 0x29: case 0x1A: case 0x2A:
        {
            // a walk: the distance from the actor to its target at its speed (an estimate until traced)
            LO* pa = PloOfOid(c.oidActor);
            LO* pt = PloOfOid(c.an[0]);
            float d = 300.0f;
            if (pa && pt) d = glm::length(e.PosWorld(pt) - e.PosWorld(pa));
            float v = c.kind == 0x27 || c.kind == 0x28 ? (c.G(0x1C) > 1.0f ? c.G(0x1C) : (c.N(0x18) ? 300.0f : 650.0f)) : 500.0f;
            vis = std::clamp(d / v, 0.3f, 20.0f);
            c.fEstimate = true;
            break;
        }
        case 0x1D: case 0x1E: case 0x1F: case 0x21: vis = 1.0f; c.fEstimate = true; break;
        case 0x3B: case 0x3C: vis = 0.5f; c.fEstimate = true; break;
        case 0x42: vis = 1.0f; c.fEstimate = true; break;
        case 0x58: case 0x59: case 0x5A: case 0x4F: case 0x50: case 0x51: case 0x43: vis = 1.0f; c.fEstimate = true; break;
        case 0x0B:
        {
            // a script it starts: its commands' own lengths, summed (one level)
            LO* ps = PloOfOid(c.an[0]);
            int kind = 0, b = 0;
            int32_t an[18];
            float s = 0.0f;
            for (int i = 0; ps && FScptCommand(ps, i, kind, b, an); ++i)
            {
                float g;
                std::memcpy(&g, &an[0], 4);
                if (kind == 0x45 && !b) s += LineLength(an[0]);
                else if ((kind == 0x23 || kind == 0x33) && std::isfinite(g)) s += std::max(0.0f, g);
            }
            vis = std::max(0.2f, s);
            c.fEstimate = true;
            break;
        }
        default: break;
        }
        hold = fWait ? vis : 0.0f;
        if (c.kind == 0x23 || c.kind == 0x32 || c.kind == 0x33) hold = fWait ? vis : 0.0f;
    };
    // estimate: each block from its start, its sub-blocks beside it
    std::map<int, float> mpblockEnd, mpalarm;
    std::function<float(int, float)> layBlock = [&](int icOpen, float t0) -> float {
        float t = t0, tCam0 = 0.0f, tCamDur = 0.0f;
        const int icEnd = icOpen < 0 ? n : matching(icOpen);
        for (int i = icOpen + 1; i < icEnd; ++i)
        {
            CSCMD& c = m_acmd[std::size_t(i)];
            if (c.kind == 0)
            {
                c.t0 = t;
                const float tEndSub = layBlock(i, t);
                c.t1 = tEndSub;
                mpblockEnd[c.an[0]] = tEndSub;
                i = matching(i);
                if (i < n) m_acmd[std::size_t(i)].t0 = m_acmd[std::size_t(i)].t1 = tEndSub;
                continue;
            }
            float vis = 0.0f, hold = 0.0f;
            own(c, vis, hold);
            c.t0 = t;
            c.t1 = t + vis;
            c.tHold = hold;
            if (c.kind == 0x38)
            {
                tCam0 = t;
                tCamDur = c.G(0x40);
                if (tCamDur == 0.0f)
                {
                    // the commands up to its end (scpt.cpp 0x38: their own durations)
                    int d = 0;
                    for (int k = i + 1; k < icEnd; ++k)
                    {
                        const CSCMD& q = m_acmd[std::size_t(k)];
                        if (q.kind == 0) ++d;
                        else if (q.kind == 1) --d;
                        else if (q.kind == 0x39 && d == 0) break;
                        else if (d == 0 && !q.b)
                        {
                            float v2 = 0.0f, h2 = 0.0f;
                            CSCMD qq = q;
                            own(qq, v2, h2);
                            if (q.kind == 0x23 || q.kind == 0x33 || q.kind == 0x2C || q.kind == 0x2D || q.kind == 0x53 || q.kind == 0x32) tCamDur += v2;
                        }
                    }
                }
                c.t1 = t + std::max(tCamDur, 0.05f);
            }
            if (c.kind == 0x39)
            {
                const float tEndCam = tCam0 + tCamDur;
                c.tHold = std::max(0.0f, tEndCam - t);
                c.t1 = t + c.tHold;
                // (a shot without its own seconds lasts until its end command)
                for (int k = i - 1; k > icOpen; --k)
                    if (m_acmd[std::size_t(k)].kind == 0x38) { CSCMD& s0 = m_acmd[std::size_t(k)]; s0.t1 = std::max(s0.t1, c.t1); break; }
            }
            if (c.kind == 0x52)
            {
                // the track's last line's rest (or its first seconds)
                float tLineEnd = t;
                for (int k = i - 1; k > icOpen; --k)
                    if (m_acmd[std::size_t(k)].kind == 0x45) { tLineEnd = m_acmd[std::size_t(k)].t0 + LineLength(m_acmd[std::size_t(k)].an[0]); break; }
                const float g = c.G(0x10);
                c.tHold = std::max(0.0f, g < 0.0f ? tLineEnd - t : std::min(tLineEnd - t, g));
                c.t1 = t + c.tHold;
            }
            if (c.kind == 0x03)
            {
                float tw = t;
                for (int k = 0; k < c.an[0] && k < 4; ++k)
                    if (const auto it = mpblockEnd.find(c.an[1 + k]); it != mpblockEnd.end()) tw = std::max(tw, it->second);
                c.tHold = tw - t;
                c.t1 = tw;
            }
            if (c.kind == 0x05) mpalarm[c.an[0]] = t;
            if (c.kind == 0x04)
                if (const auto it = mpalarm.find(c.an[0]); it != mpalarm.end() && it->second > t) { c.tHold = it->second - t; c.t1 = it->second; }
            t += c.tHold;
        }
        return t;
    };
    float tEnd = layBlock(0, 0.0f);
    // measured: the trace's executions (each command's first; a trace of these same commands)
    if (m_trace.fOk && m_sigTrace == Signature())
    {
        std::map<int, std::vector<std::pair<int, float>>> mprun;   // run -> (ic, t) in order
        for (const auto& a : m_trace.acmd) mprun[int(a[0])].push_back({int(a[1]), a[2]});
        std::set<int> setSeen;
        for (auto& [run, a] : mprun)
            for (std::size_t k = 0; k < a.size(); ++k)
            {
                const int ic = a[k].first;
                if (ic < 0 || ic >= n || setSeen.count(ic)) continue;
                setSeen.insert(ic);
                CSCMD& c = m_acmd[std::size_t(ic)];
                float vis = 0.0f, hold = 0.0f;
                own(c, vis, hold);
                const float tNext = k + 1 < a.size() ? a[k + 1].second
                                    : (run < int(m_trace.arun.size()) && m_trace.arun[std::size_t(run)].t1 >= 0 ? m_trace.arun[std::size_t(run)].t1 : m_trace.tEnd);
                c.t0 = a[k].second;
                c.tHold = std::max(0.0f, tNext - c.t0);
                c.t1 = c.t0 + std::max(c.tHold, c.b || c.kind == 0x38 ? vis : 0.0f);
                c.fMeasured = c.fRan = true;
                c.fEstimate = false;
                if (c.kind == 0 && run + 1 < 1000)
                {
                    // a block: from its start to its runner's end
                    for (const CSTRACE::RUN& r : m_trace.arun)
                        if (r.block == ic) { c.t1 = r.t1 >= 0 ? r.t1 : m_trace.tEnd; break; }
                }
            }
        // the lines and animations as they played; a shot until its end's time
        for (CSCMD& c : m_acmd)
        {
            if (!c.fRan) continue;
            if (c.kind == 0x38)
                for (std::size_t k = std::size_t(c.ic) + 1; k < m_acmd.size(); ++k)
                    if (m_acmd[k].kind == 0x39 && m_acmd[k].icBlock == c.icBlock) { if (m_acmd[k].fRan) c.t1 = std::max(c.t1, m_acmd[k].t1); break; }
            if (c.kind == 0x45)
                for (const auto& l : m_trace.aline)
                    if (l.id == c.an[0] && std::fabs(l.t0 - c.t0) < 0.1f) { c.t1 = std::max(c.t1, l.t1); break; }
            if (c.kind == 0x0A)
                for (const auto& a : m_trace.aanim)
                    if (!a.fAmbient && a.id == c.an[0] && std::fabs(a.t0 - c.t0) < 0.1f) { c.t1 = std::max(c.t1, a.t1); break; }
        }
        // the commands it never ran (another branch, after a stop): after the end, dimmed
        float tAfter = m_trace.tEnd;
        for (CSCMD& c : m_acmd)
            if (!c.fRan && c.ic > 0)
            {
                const float d = c.t1 - c.t0;
                c.t0 = tAfter;
                c.t1 = tAfter + d;
            }
        tEnd = m_trace.tEnd;
    }
    // (a shot's end is the moment the camera arrives: shown there, its shot the clip before it)
    for (CSCMD& c : m_acmd)
    {
        c.tExec = c.t0;
        if (c.kind == 0x39) c.t0 = c.t1;
    }
    m_tEnd = std::max(0.5f, tEnd);
    for (const CSCMD& c : m_acmd) m_tEnd = std::max(m_tEnd, c.t1);
    LayoutPresentation();
}

void CCutsceneEditor::BuildTracks()
{
    BuildTrackList();
    // (overlapping clips of a track stack in rows, as a video editor shows them)
    for (CSTRACK& tr : m_atrack)
    {
        std::vector<int> a = tr.aicmd;
        std::stable_sort(a.begin(), a.end(), [&](int x, int y) { return m_acmd[std::size_t(x)].t0 < m_acmd[std::size_t(y)].t0; });
        std::vector<float> aend;
        for (int ic : a)
        {
            CSCMD& c = m_acmd[std::size_t(ic)];
            const float t1 = std::max(c.t1, c.t0 + 0.12f);
            std::size_t l = 0;
            while (l < aend.size() && aend[l] > c.t0 + 1e-4f) ++l;
            if (l == aend.size()) aend.push_back(t1); else aend[l] = t1;
            c.lane = int(l);
        }
        tr.clane = std::max<int>(1, int(aend.size()));
    }
}

void CCutsceneEditor::BuildTrackList()
{
    m_atrack.clear();
    auto track = [&](cut::TRACKK tk, int oid, const std::string& name) -> int {
        for (std::size_t i = 0; i < m_atrack.size(); ++i)
            if (m_atrack[i].tk == tk && m_atrack[i].oid == oid && (oid != -2 || m_atrack[i].name == name)) return int(i);
        CSTRACK t;
        t.tk = tk, t.oid = oid, t.name = name;
        m_atrack.push_back(std::move(t));
        return int(m_atrack.size()) - 1;
    };
    if (m_fByRun)
    {
        // the script's own tracks: each block a track
        for (CSCMD& c : m_acmd)
        {
            if (c.kind == 1 || (c.kind == 0 && c.ic == 0)) { c.itrack = -1; continue; }
            const int icb = c.kind == 0 ? c.ic : c.icBlock;
            const std::string name = icb <= 0 ? std::string(T("Main track")) : std::string(T("Track")) + " " + std::to_string(icb) + (m_acmd[std::size_t(icb)].an[0] != -1 ? " (" + NameOf(m_acmd[std::size_t(icb)].an[0]) + ")" : "");
            c.itrack = track(cut::TK_Flow, -2, name);
            m_atrack[std::size_t(c.itrack)].aicmd.push_back(c.ic);
        }
        return;
    }
    // by kind: the camera, the dialogue, each character, effects, sound, events, the flow
    static const cut::TRACKK s_aorder[] = {cut::TK_Camera, cut::TK_Present, cut::TK_Dialog, cut::TK_Actor, cut::TK_Effects, cut::TK_Sound, cut::TK_Events, cut::TK_Flow};
    for (cut::TRACKK tk : s_aorder)
        for (CSCMD& c : m_acmd)
        {
            if (c.ic == 0 || c.kind == 1) continue;
            const cut::CMDDESC& d = cut::DescOfKind(c.kind);
            cut::TRACKK tkc = d.tk;
            if (c.kind == 0x23 && c.an[1] == -1 && c.an[2] == -1) tkc = cut::TK_Flow;   // (a plain wait)
            if (FFadeClip(c)) tkc = cut::TK_Present;                                      // (a fade the script's event starts)
            if (tkc != tk) continue;
            if (tk == cut::TK_Actor)
            {
                const int oid = c.oidActor;
                c.itrack = track(tk, oid, oid == -1 ? std::string(T("The track's object")) : NameOf(oid));
            }
            else c.itrack = track(tk, -1, T(cut::SzTrack(tk)));
            m_atrack[std::size_t(c.itrack)].aicmd.push_back(c.ic);
        }
}

std::string CCutsceneEditor::Label(const CSCMD& c) const
{
    char sz[256];
    const cut::CMDDESC& d = cut::DescOfKind(c.kind);
    switch (c.kind)
    {
    case 0x00: return std::string(T("Track")) + (c.an[0] != -1 ? " " + NameOf(c.an[0]) : "");
    case 0x23:
        if (c.an[1] == -1 && c.an[2] == -1) { std::snprintf(sz, sizeof sz, "%s %.2f s", T("Wait"), c.G(0x10)); return sz; }
        std::snprintf(sz, sizeof sz, "%s %s %.2f s", T("Face"), NameOf(c.an[1]).c_str(), c.G(0x10));
        return sz;
    case 0x45:
    {
        std::string s = e.StrText(c.an[0]);
        return std::string(T("Line")) + " " + std::to_string(c.an[0]) + (s.empty() ? "" : ": " + s);
    }
    case 0x0A:
    {
        // (an animation's own name already says it: "Animation 124160")
        const std::string n = NameOf(c.an[0]);
        return n.rfind(T("Animation"), 0) == 0 || n.rfind("Animation", 0) == 0 ? n : std::string(T("Animation")) + " " + n;
    }
    case 0x22: return std::string(T("Prize animation")) + " " + NameOf(c.an[0]);
    case 0x27: return std::string(c.N(0x18) ? T("Walk along") : T("Run along")) + " " + NameOf(c.an[0]);
    case 0x28: return std::string(c.N(0x18) ? T("Walk to") : T("Run to")) + " " + NameOf(c.an[0]);
    case 0x24: return std::string(T("Go to")) + " " + NameOf(c.an[0]);
    case 0x16: return std::string(T("Place at")) + " " + NameOf(c.an[0]);
    case 0x38: std::snprintf(sz, sizeof sz, "%s %.2f s", T("Camera shot"), c.t1 - c.t0); return sz;
    case 0x39: return T("Shot end");
    case 0x11: return std::string(T("Task")) + " " + NameOf(c.an[0]) + " -> " + SzTaskState(c.an[1]);
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x34: case 0x07:
        return std::string(T(d.name)) + " " + NameOf(c.an[0]);
    case 0x0B: return std::string(T("Start script")) + " " + std::to_string(c.an[0]);
    case 0x0D: return std::string(T("State machine")) + " " + NameOf(c.an[0]) + " -> " + std::to_string(c.an[2]);
    case 0x12:
        if (c.an[0] && (c.an[1] & 1)) return T("Letterbox (the game paused)");
        return c.an[0] ? T("Pause the game (scene starts)") : T("Unpause (scene ends)");
    case 0x09:
        if (FFadeClip(c)) return std::string(T("Fade")) + " " + std::to_string(c.an[0]);
        return T(d.name);
    case 0x36: return std::string(T("Sound")) + " " + std::to_string(c.an[0]);
    case 0x4C: case 0x4D: return std::string(T(d.name)) + " " + std::to_string(c.an[0]);
    case 0x53: return std::string((FTitleCardClip(c) ? T("Title card") : T("Caption"))) + ": " + e.StrText(c.an[0]);
    case 0x13: case 0x14: case 0x15: case 0x04: case 0x05: return std::string(T(d.name)) + " " + std::to_string(c.an[0]);
    default: return T(d.name);
    }
}

// ---- the trace

std::string CCutsceneEditor::TraceDir() const
{
    return ce::utf8(cane::userDirectory() / "Editor" / "Cutscenes" / ".trace");
}

void CCutsceneEditor::StartTrace()
{
    if (!m_pscpt) return;
    // (the project's edits as they are now, unsaved too: a copy of the package for the run)
    std::error_code ec;
    const cane::fs::path dirPkg = ce::pathOf(TraceDir()) / "pkg";
    std::string error;
    bool fPkg = !e.m_proj.changedParts().empty();
    if (fPkg)
    {
        e.m_proj.manifest.worlds = ce::worldsOfParts(e.m_inst, e.m_proj.changedParts());
        if (!e.m_proj.exportFolder(dirPkg, error)) { m_traceNote = std::string(T("not measured:")) + " " + error; return; }
    }
    m_sigPending = Signature();
#ifdef _WIN32
    const auto exe = cane::executableDirectory() / EDGAME_EXE ".exe";
#else
    const auto exe = cane::executableDirectory() / EDGAME_EXE;
#endif
    cane::fs::create_directories(ce::pathOf(TraceDir()) / "user", ec);
    cane::fs::remove(ce::pathOf(m_tracePath), ec);
    // (absolute paths: the child starts in the executable's folder)
    std::vector<std::string> args{"--data-dir", ce::utf8(cane::fs::absolute(e.m_inst.dir, ec)), "--user-dir", ce::utf8(cane::fs::absolute(ce::pathOf(TraceDir()) / "user", ec)),
                                  "--mod-package", ce::utf8(cane::fs::absolute(dirPkg / ce::pathOf(e.m_proj.manifest.id), ec)),
                                  "--world", e.WorldArg(e.m_eo.record), "--headless", "--fixed-dt", "--frames", "40000",
                                  "--cutscene-trace", std::to_string(m_oid), "--cutscene-out", ce::utf8(cane::fs::absolute(ce::pathOf(m_tracePath), ec)), "--cutscene-max", "150"};
    if (m_taskMeasure > 0) args[7] += ":" + std::to_string(m_taskMeasure);
    if (m_measureMode == 1)
        if (const int oidRoot = RootStarter(); oidRoot && oidRoot != m_oid)
        {
            args[13] = std::to_string(oidRoot);
            args.insert(args.end(), {"--cutscene-focus", std::to_string(m_oid)});
        }
    if (!fPkg) args.erase(args.begin() + 4, args.begin() + 6);   // (no edits: the installed world)
    if (e.m_fPlayDry)
    {
        std::string line;
        for (std::size_t i = 4; i < args.size(); ++i) line += (i > 4 ? " " : "") + args[i];
        std::printf("editor: cutscene trace: %s\n", line.c_str());
        return;
    }
    if (!ce::spawnDetached(exe, args)) { m_traceNote = "could not start " + ce::utf8(exe); return; }
    m_fTracePending = true;
    m_tTraceStart = glfwGetTime();
    m_pollTrace = 0;
    m_traceNote = T("measuring in the game...");
}

void CCutsceneEditor::PollTrace()
{
    if (!m_fTracePending || ++m_pollTrace % 15 != 0) return;
    std::error_code ec;
    if (!cane::fs::exists(ce::pathOf(m_tracePath), ec))
    {
        // (the game ended without a measurement: it crashed (its log says so) or it ran past any scene's length)
        bool fCrashed = false;
        if (std::ifstream f(ce::pathOf(m_tracePath + ".log")); f)
            for (std::string line; !fCrashed && std::getline(f, line);) fCrashed = line.find("crashed, stack") != std::string::npos;
        if (fCrashed || glfwGetTime() - m_tTraceStart > 600.0)
        {
            m_fTracePending = false;
            m_traceNote = fCrashed ? T("not measured: the game crashed playing it") : T("not measured: the game did not finish");
            e.Status(std::string("cutscene ") + std::to_string(m_oid) + ": " + m_traceNote);
        }
        return;
    }
    if (!FReadTrace(m_tracePath))
    {
        // (a moment more: written by a game of before the rename)
        if (++m_cReadFailed < 8) return;
        m_fTracePending = false;
        m_traceNote = T("the measurement could not be read");
        return;
    }
    m_cReadFailed = 0;
    m_fTracePending = false;
    ForgetPoses();
    m_sigTrace = m_sigPending;
    if (!m_trace.fOk) { Layout(); BuildTracks(); e.Status(std::string("cutscene ") + std::to_string(m_oid) + ": " + m_traceNote); return; }
    Layout();
    BuildTracks();
    m_fFitPending = true;
    m_tApplied = -1.0f;
    char sz[160];
    std::snprintf(sz, sizeof sz, "%s %.2f s%s", T("measured in the game:"), m_trace.tEnd,
                  m_trace.fLeft ? T(" (it leaves the world)") : m_trace.fTimedOut ? T(" (still running: it waits for something)") : "");
    m_traceNote = sz;
    e.Status(std::string("cutscene ") + std::to_string(m_oid) + ": " + sz);
}

bool CCutsceneEditor::FReadTrace(const std::string& path)
{
    std::vector<uint8_t> ab;
    if (!ce::readFile(ce::pathOf(path), ab)) return false;
    const nlohmann::json j = nlohmann::json::parse(ab.begin(), ab.end(), nullptr, false);
    if (!j.is_object() || j.value("format", std::string()) != "sly2-cutscene-trace") return false;
    CSTRACE t;
    t.fOk = j.value("started", false) && j.value("focus_started", true);
    if (!t.fOk)
    {
        m_trace = t;
        m_traceNote = j.value("started", false) ? T("its starter played, but never reached it (it waits for gameplay: try another task)") : T("the script is not in the world at that task");
        return true;
    }
    t.fStopped = j.value("stopped", false);
    t.fLeft = j.value("left_world", false);
    t.fTimedOut = j.value("timed_out", false);
    t.tEnd = j.value("end", 0.0f);
    t.oidPlayer = j.value("player", -1);
    for (const auto& c : j["camera"])
    {
        if (!c.is_array() || c.size() < 14) continue;
        CSTRACE::CAMKEY k;
        k.t = c[0].get<float>();
        k.pos = {c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
        for (int i = 0; i < 3; ++i)
            for (int jj = 0; jj < 3; ++jj) k.mat[i][jj] = c[std::size_t(4 + 3 * i + jj)].get<float>();
        k.fov = c[13].get<float>();
        t.acam.push_back(k);
    }
    for (const auto& o : j["objects"])
    {
        CSTRACE::OBJ ob;
        ob.oid = o.value("oid", 0);
        ob.cid = o.value("cid", 0);
        ob.rank = o.value("rank", 0);
        ob.depth = o.value("depth", 0);
        ob.fStill = o.value("still", 0) != 0;
        if (o.contains("parent")) ob.parent = TraceIdOf(o["parent"]);
        for (const auto& k : o["keys"])
            if (k.is_array() && k.size() >= 9)
                ob.akey.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>()},
                                   glm::quat(k[7].get<float>(), k[4].get<float>(), k[5].get<float>(), k[6].get<float>()), k[8].get<int>() != 0});
        if (!ob.akey.empty()) t.aobj.push_back(std::move(ob));
    }
    if (j.contains("drawn"))
        for (const auto& o : j["drawn"])
            if (o.is_array() && o.size() >= 4 && o[3].is_array())
            {
                CSTRACE::DRAWN d{o[0].get<int>(), o[1].get<int>(), o[2].get<int>(), {}};
                for (std::size_t k = 0; k + 1 < o[3].size(); k += 2) d.akey.push_back({o[3][k].get<float>(), o[3][k + 1].get<int>() != 0});
                t.adrawn.push_back(std::move(d));
            }
    if (j.contains("parts"))
        for (const auto& o : j["parts"])
            if (o.is_array() && o.size() >= 4 && o[3].is_array())
            {
                CSTRACE::PARTS d{o[0].get<int>(), o[1].get<int>(), o[2].get<int>(), {}};
                for (std::size_t k = 0; k + 1 < o[3].size(); k += 2)
                {
                    std::vector<std::pair<int32_t, bool>> a;
                    for (const auto& p : o[3][k + 1])
                        if (p.is_array() && p.size() >= 2) a.push_back({p[0].get<int32_t>(), p[1].get<int>() != 0});
                    d.akey.push_back({o[3][k].get<float>(), std::move(a)});
                }
                t.aparts.push_back(std::move(d));
            }
    if (j.contains("seen"))
        for (const auto& o : j["seen"])
            if (o.is_array() && o.size() >= 4 && o[3].is_array())
            {
                CSTRACE::SEEN d{o[0].get<int>(), o[1].get<int>(), o[2].get<int>(), {}};
                for (const auto& k : o[3])
                    if (k.is_array() && k.size() >= 4) d.at.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>()}});
                t.aseen.push_back(std::move(d));
            }
    for (const auto& l : j["lines"])
    {
        CSTRACE::SPAN sp;
        sp.id = l.value("line", 0), sp.t0 = l.value("t0", 0.0f), sp.t1 = l.value("t1", 0.0f);
        t.aline.push_back(std::move(sp));
    }
    for (const auto& a : j["anims"])
    {
        CSTRACE::SPAN sp;
        sp.id = a.value("aseg", 0), sp.on = a.value("on", -1), sp.t0 = a.value("t0", 0.0f), sp.t1 = a.value("t1", 0.0f), sp.length = a.value("length", 0.0f);
        sp.oncid = a.value("oncid", -1), sp.onrank = a.value("onrank", 0);
        sp.at = a.value("at", 0.0f);
        sp.fAmbient = a.value("ambient", 0) != 0;
        if (a.contains("keys"))
            for (const auto& k : a["keys"])
                if (k.is_array() && k.size() >= 2)
                    sp.akey.push_back({k[0].get<float>(), k[1].get<float>(), k.size() >= 3 ? k[2].get<float>() : 1.0f, k.size() >= 4 ? k[3].get<int>() : sp.id});
        t.aanim.push_back(std::move(sp));
    }
    for (const auto& c : j["cmds"])
        if (c.is_array() && c.size() >= 3) t.acmd.push_back({c[0].get<float>(), c[1].get<float>(), c[2].get<float>()});
    for (const auto& r : j["runs"]) t.arun.push_back({r.value("block", 0), r.value("parent", -1), r.value("t0", 0.0f), r.value("t1", -1.0f)});
    // (the parents placed before their parts; the still ones kept only where the editor's world has them elsewhere:
    // compared with the scene as loaded)
    RestoreScene();
    std::stable_sort(t.aobj.begin(), t.aobj.end(), [](const CSTRACE::OBJ& a, const CSTRACE::OBJ& b) { return a.depth < b.depth; });
    t.aobj.erase(std::remove_if(t.aobj.begin(), t.aobj.end(), [&](const CSTRACE::OBJ& o) {
        if (!o.fStill) return false;
        LO* plo = PloOf(o.oid, o.cid, o.rank);
        if (!plo || !FIsLoDerivedFrom(plo, EDCID(0x30)) || o.akey.empty()) return true;
        // (a part under another object in the editor's world than in the game's: kept, it follows the game's one)
        if (o.depth > 0)
            if (LO* proot = PloOf(o.parent); proot && !FLoInParentChain(plo, proot)) return false;
        glm::mat4 W(1.0f);
        if (!FObjectWorld(static_cast<ALO*>(plo), o.depth > 0 ? &o.parent : nullptr, W)) return true;
        glm::mat3 r(W);
        for (int i = 0; i < 3; ++i)
            if (const float l = glm::length(r[i]); l > 1e-6f) r[i] /= l;
        const glm::quat q = glm::normalize(glm::quat_cast(r));
        return glm::length(glm::vec3(W[3]) - o.akey[0].pos) <= 0.5f && std::fabs(glm::dot(q, o.akey[0].q)) >= 0.9999f;
    }), t.aobj.end());
    m_trace = std::move(t);
    return true;
}

// An object's place as the trace keys it: its record's world, or (a part) relative to the record of its root object in
// the game (pparent; its world in pWparent)
bool CCutsceneEditor::FObjectWorld(ALO* palo, const TRACEID* pparent, glm::mat4& W, glm::mat4* pWparent) const
{
    const XF* pxf = PxfObj(palo) ? PxfObj(palo) : palo->axf.empty() ? nullptr : palo->axf[0].get();
    if (!pxf) return false;
    W = pxf->matWorld;
    if (!pparent) return true;
    LO* plo = PloOf(*pparent);
    ALO* par = plo && FIsLoDerivedFrom(plo, EDCID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
    const XF* pp = !par ? nullptr : PxfObj(par) ? PxfObj(par) : par->axf.empty() ? nullptr : par->axf[0].get();
    if (!pp) return false;
    W = glm::inverse(pp->matWorld) * W;
    if (pWparent) *pWparent = pp->matWorld;
    return true;
}

// the script that starts this one (by a "Start script" command), and the one that starts that ...
int CCutsceneEditor::RootStarter() const
{
    int oid = m_oid;
    std::set<int> setSeen{oid};
    for (bool fFound = true; fFound;)
    {
        fFound = false;
        for (const auto& up : e.m_psw->aploAll)
        {
            if (!FIsLoDerivedFrom(up.get(), EDCID(0x72))) continue;
            int kind = 0, b = 0;
            int32_t an[18];
            for (int i = 0; FScptCommand(up.get(), i, kind, b, an); ++i)
                if (kind == 0x0B && an[0] == oid && !setSeen.count(up->oid)) { oid = up->oid; setSeen.insert(oid); fFound = true; break; }
            if (fFound) break;
        }
    }
    return oid;
}

// ---- the scene at the playhead

bool CCutsceneEditor::FCameraAt(float t, CAMERA& cam) const
{
    const auto& a = m_trace.acam;
    if (a.empty()) return false;
    std::size_t i = 0;
    while (i + 1 < a.size() && a[i + 1].t <= t) ++i;
    const CSTRACE::CAMKEY& k0 = a[i];
    const CSTRACE::CAMKEY& k1 = i + 1 < a.size() ? a[i + 1] : a[i];
    const float u = k1.t > k0.t ? std::clamp((t - k0.t) / (k1.t - k0.t), 0.0f, 1.0f) : 0.0f;
    cam.pos = glm::mix(k0.pos, k1.pos, u);
    cam.fMat = true;
    const glm::quat q = glm::slerp(glm::quat_cast(k0.mat), glm::quat_cast(k1.mat), u);
    cam.mat = glm::mat3_cast(q);
    const glm::vec3 d = cam.mat[0];
    cam.yaw = glm::degrees(std::atan2(d.y, d.x));
    cam.pitch = glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f)));
    cam.fovY = glm::degrees(glm::mix(k0.fov, k1.fov, u));
    return true;
}

bool CCutsceneEditor::FObjectAt(const CSTRACE::OBJ& o, float t, glm::vec3& pos, glm::quat& q, bool& fIn) const
{
    if (o.akey.empty()) return false;
    std::size_t i = 0;
    while (i + 1 < o.akey.size() && o.akey[i + 1].t <= t) ++i;
    const auto& k0 = o.akey[i];
    // (keys are kept where the object changed: between two it moved smoothly when they are a step apart or the change
    // is small (a slow turn: a key each few tenths of a second, far parts swing wide), else it stood until the later one)
    const auto fSmooth = [&](const CSTRACE::OBJKEY& a, const CSTRACE::OBJKEY& b) {
        return b.t - a.t < 0.05f || (b.t - a.t < 2.0f && glm::length(b.pos - a.pos) < 50.0f && std::fabs(glm::dot(a.q, b.q)) > 0.999f && a.fIn == b.fIn);
    };
    if (i + 1 < o.akey.size() && fSmooth(k0, o.akey[i + 1]))
    {
        const auto& k1 = o.akey[i + 1];
        const float u = std::clamp((t - k0.t) / std::max(1e-4f, k1.t - k0.t), 0.0f, 1.0f);
        pos = glm::mix(k0.pos, k1.pos, u);
        q = glm::slerp(k0.q, k1.q, u);
    }
    else pos = k0.pos, q = k0.q;
    fIn = k0.fIn;
    return true;
}

// The clip playing on an object at t (the newest started; an idle from before the scene when none) and its time
bool CCutsceneEditor::FAnimOn(int oid, float t, LO*& plAseg, float& tClip) const
{
    // (the clips on it at t, newest first, an idle from before the scene last; the first that is found)
    std::vector<const CSTRACE::SPAN*> ap;
    for (const CSTRACE::SPAN& a : m_trace.aanim)
        if (a.on == oid && t >= a.t0 - 1e-4f && t <= a.t1 + c_dtFrame + 1e-4f) ap.push_back(&a);
    std::stable_sort(ap.begin(), ap.end(), [](const CSTRACE::SPAN* a, const CSTRACE::SPAN* b) {
        return a->fAmbient != b->fAmbient ? !a->fAmbient : a->t0 > b->t0;
    });
    LO* plo = PloOfOid(oid);
    for (const CSTRACE::SPAN* pa : ap)
    {
        int id = -1;
        const float tc = pa->TClip(t, &id);
        if (id < 0) continue;
        // (its clip: the ASEG under the character of that id, as the game finds it; else the world's)
        LO* pfound = nullptr;
        std::function<void(LO*)> find = [&](LO* p) {
            if (pfound) return;
            if (p != plo && FIsLoDerivedFrom(p, EDCID(0x6D)) && p->oid == id) { pfound = p; return; }
            if (const auto it = e.m_mpchildren.find(p); it != e.m_mpchildren.end())
                for (LO* c : it->second) find(c);
        };
        if (plo) find(plo);
        if (!pfound) pfound = PloOfOid(id);
        if (!pfound || !FIsLoDerivedFrom(pfound, EDCID(0x6D))) continue;
        plAseg = pfound;
        const ASEGD* pd = PasegdOf(static_cast<ASEG*>(pfound));
        tClip = pd && pd->tMax > 0 ? std::clamp(tc, 0.0f, pd->tMax) : tc;
        return true;
    }
    return false;
}

// A clip by its id for an object: the ASEG under the character of that id, as the game finds it; else the world's
LO* CCutsceneEditor::PlAsegOf(LO* plo, int id) const
{
    LO* pfound = nullptr;
    std::function<void(LO*)> find = [&](LO* p) {
        if (pfound) return;
        if (p != plo && FIsLoDerivedFrom(p, EDCID(0x6D)) && p->oid == id) { pfound = p; return; }
        if (const auto it = e.m_mpchildren.find(p); it != e.m_mpchildren.end())
            for (LO* c : it->second) find(c);
    };
    if (plo) find(plo);
    if (!pfound) pfound = PloOfOid(id);
    return pfound && FIsLoDerivedFrom(pfound, EDCID(0x6D)) ? pfound : nullptr;
}

bool CCutsceneEditor::FAnimsOn(const LO* plo, float t, std::vector<std::tuple<LO*, LO*, float>>& aclip) const
{
    aclip.clear();
    if (!plo || m_aploSpan.size() != m_trace.aanim.size()) return false;
    std::vector<std::size_t> ai;
    for (std::size_t i = 0; i < m_trace.aanim.size(); ++i)
    {
        const CSTRACE::SPAN& a = m_trace.aanim[i];
        LO* pon = m_aploSpan[i];
        if (pon && t >= a.t0 - 1e-4f && t <= a.t1 + c_dtFrame + 1e-4f && (pon == plo || FLoInParentChain(pon, plo))) ai.push_back(i);
    }
    // (the idles from before the scene first, then as they started)
    std::stable_sort(ai.begin(), ai.end(), [&](std::size_t i, std::size_t j) {
        const CSTRACE::SPAN &a = m_trace.aanim[i], &b = m_trace.aanim[j];
        return a.fAmbient != b.fAmbient ? a.fAmbient : a.t0 < b.t0;
    });
    for (std::size_t i : ai)
    {
        const CSTRACE::SPAN& a = m_trace.aanim[i];
        int id = -1;
        const float tc = a.TClip(t, &id);
        if (id < 0) continue;
        LO* pl = PlAsegOf(m_aploSpan[i], id);
        if (!pl) continue;
        const ASEGD* pd = PasegdOf(static_cast<ASEG*>(pl));
        aclip.push_back({m_aploSpan[i], pl, pd && pd->tMax > 0 ? std::clamp(tc, 0.0f, pd->tMax) : tc});
    }
    return !aclip.empty();
}

// An object's pose at t, relative to its object record (cached by its clips and their frames)
const CCutsceneEditor::POSE* CCutsceneEditor::PposeAt(ALO* palo, float t)
{
    std::vector<std::tuple<LO*, LO*, float>> aclip;
    if (!palo || !FAnimsOn(palo, t, aclip)) return nullptr;
    std::string sig;
    for (auto& [pon, pl, tc] : aclip)
    {
        tc = float(std::lround(tc * 60.0f)) / 60.0f;
        sig += std::to_string(reinterpret_cast<std::uintptr_t>(pon)) + ":" + std::to_string(reinterpret_cast<std::uintptr_t>(pl)) + "@" + std::to_string(std::lround(tc * 60.0f)) + ";";
    }
    const auto key = std::make_pair(static_cast<const ALO*>(palo), sig);
    auto it = m_mppose.find(key);
    if (it == m_mppose.end())
    {
        POSE a;
        if (m_mppose.size() > 4000) m_mppose.clear();
        e.FPoseCharacter(palo, aclip, a);
        it = m_mppose.emplace(key, std::move(a)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

void CCutsceneEditor::ForgetPoses()
{
    m_mppose.clear();
    m_aoidAnimated.clear();
    m_aploSpan.clear();
    m_pswPoseRoot = nullptr;
    std::set<std::array<int, 3>> set;
    for (const CSTRACE::SPAN& a : m_trace.aanim)
        if (a.on >= 0 && set.insert({a.on, a.oncid, a.onrank}).second) m_aoidAnimated.push_back({a.on, a.oncid, a.onrank});
}

// The animated objects posed at the playhead: each tree a clip plays in posed whole (its root's clips and its parts'),
// each record's world = its root's object record's (where the trace placed it) x its pose (the renderer skins with them)
void CCutsceneEditor::PoseCharacters()
{
    if (m_pswPoseRoot != e.m_psw.get() || m_aploSpan.size() != m_trace.aanim.size())
    {
        m_pswPoseRoot = e.m_psw.get();
        m_aploSpan.clear();
        for (const CSTRACE::SPAN& a : m_trace.aanim) m_aploSpan.push_back(a.on >= 0 ? PloOf(a.on, a.oncid, a.onrank) : nullptr);
        m_apaloPoseRoot.clear();
        std::set<LO*> set;
        for (LO* p : m_aploSpan)
        {
            if (!p) continue;
            while (p->paloParent) p = p->paloParent;
            if (FIsLoDerivedFrom(p, EDCID(0x30)) && set.insert(p).second) m_apaloPoseRoot.push_back(static_cast<ALO*>(p));
        }
    }
    for (ALO* palo : m_apaloPoseRoot)
    {
        const POSE* pp = PposeAt(palo, m_t);
        if (!pp)
        {
            // (no clip at this time: its own pose again)
            if (m_setPosed.erase(palo)) UpdateAloHierarchyWorlds(palo);
            continue;
        }
        XF* pxfObj = PxfObj(palo) ? PxfObj(palo) : palo->axf.empty() ? nullptr : palo->axf[0].get();
        if (!pxfObj) continue;
        if (!m_setPosed.count(palo)) UpdateAloHierarchyWorlds(palo);
        const glm::mat4 W = pxfObj->matWorld;
        for (const auto& [pxf, rel] : *pp) pxf->matWorld = W * rel;
        m_setPosed.insert(palo);
    }
}

// The scene entries the game does not draw at the playhead left out: the objects out of the world with the objects
// under them (setTree), the models it does not draw (setSelf)
void CCutsceneEditor::HideOutOfWorld(const std::set<const LO*>& setTree, const std::set<const LO*>& setSelf)
{
    std::vector<int> ai;
    if (!setTree.empty() || !setSelf.empty())
        for (std::size_t k = 0; k < e.m_apalo.size() && k < e.m_scene.aglobset.size(); ++k)
        {
            if (setSelf.count(e.m_apalo[k])) { ai.push_back(int(k)); continue; }
            for (const LO* p = e.m_apalo[k]; p && !setTree.empty(); p = p->paloParent)
                if (setTree.count(p)) { ai.push_back(int(k)); break; }
        }
    if (ai == e.m_aiSceneHidden) return;
    // (what shows again placed by the scene's own placement first)
    const bool fShown = !std::includes(ai.begin(), ai.end(), e.m_aiSceneHidden.begin(), e.m_aiSceneHidden.end());
    e.m_aiSceneHidden = std::move(ai);
    if (fShown) UpdateSceneFromSw(e.m_scene, e.m_apalo);
}

void CCutsceneEditor::ApplyScene()
{
    if (!m_trace.fOk || !m_fPreview) { RestoreScene(); return; }
    // (the picture's part of the window: the 3D view's free part, the game's 16:9 in it)
    const glm::vec4 rc = RcPicture(true);
    if (m_tApplied == m_t && (!m_fLookActive || rc == m_rcLook)) return;
    m_tApplied = m_t;
    std::set<const LO*> setOut, setNotDrawn;
    for (const CSTRACE::DRAWN& d : m_trace.adrawn)
        if (!d.FAt(m_t))
            if (LO* plo = PloOf(d.oid, d.cid, d.rank)) setNotDrawn.insert(plo);
    // the objects at the trace's places: the root objects and characters, then (after the poses) the parts, relative to
    // their root objects as the game had them
    auto place = [&](const CSTRACE::OBJ& o) {
        LO* plo = PloOf(o.oid, o.cid, o.rank);
        if (!plo || !FIsLoDerivedFrom(plo, EDCID(0x30))) return;
        ALO* palo = static_cast<ALO*>(plo);
        // (the object's own record: an actor's moves under its root's, as the game moves it)
        XF* pxf = PxfObj(palo);
        if (!pxf) pxf = palo->axf.empty() ? nullptr : palo->axf[0].get();
        if (!pxf) return;
        glm::vec3 pos;
        glm::quat q;
        bool fIn = true;
        if (!FObjectAt(o, m_t, pos, q, fIn)) return;
        if (!fIn) setOut.insert(plo);
        glm::mat4 Wk(1.0f), Wroot(1.0f);
        if (!FObjectWorld(palo, o.depth > 0 ? &o.parent : nullptr, Wk, &Wroot)) return;
        if (!m_mpOrig.count(pxf)) m_mpOrig[pxf] = {palo, pxf->mat};
        // the world matrix the trace saw (its scale the record's own), as the record's local one (its records' worlds
        // from their own matrices first: a pose set them directly; a part's parent may be posed)
        UpdateXfWorlds(palo);
        const glm::mat4 W0 = pxf->matWorld;
        glm::vec3 scale(glm::length(glm::vec3(Wk[0])), glm::length(glm::vec3(Wk[1])), glm::length(glm::vec3(Wk[2])));
        for (int i = 0; i < 3; ++i) if (scale[i] < 1e-4f) scale[i] = 1.0f;
        glm::mat4 W = glm::mat4_cast(q);
        for (int i = 0; i < 3; ++i) W[i] *= scale[i];
        W[3] = glm::vec4(pos, 1.0f);
        if (o.depth > 0) W = Wroot * W;   // (the root object's record's world x the part's place)
        const glm::mat4 parent = W0 * glm::inverse(pxf->mat);
        pxf->mat = glm::inverse(parent) * W;
        UpdateAloHierarchyWorlds(palo);
    };
    for (const CSTRACE::OBJ& o : m_trace.aobj)
        if (o.depth == 0) place(o);
    if (m_fPoses) PoseCharacters();
    for (const CSTRACE::OBJ& o : m_trace.aobj)
        if (o.depth > 0) place(o);
    // the parts shown / hidden as the game had them (the scene's placement reads them)
    for (const CSTRACE::PARTS& d : m_trace.aparts)
    {
        LO* plo = PloOf(d.oid, d.cid, d.rank);
        const auto* pa = d.PaAt(m_t);
        if (!plo || !pa || !FIsLoDerivedFrom(plo, EDCID(0x30))) continue;
        ALO* palo = static_cast<ALO*>(plo);
        if (!m_mpPartsOrig.count(palo)) m_mpPartsOrig[palo] = palo->aglobShown;
        palo->aglobShown = *pa;
    }
    if (!m_mpOrig.empty() || !m_setPosed.empty() || !m_mpPartsOrig.empty()) UpdateSceneFromSw(e.m_scene, e.m_apalo);
    HideOutOfWorld(setOut, setNotDrawn);
    m_setOutLast = std::move(setOut);
    m_setNotDrawnLast = std::move(setNotDrawn);
    CAMERA cam = e.m_cam;
    if (m_fLookThrough && FCameraAt(m_t, cam))
    {
        if (!m_fLookActive) { m_camSaved = e.m_cam; m_fLookActive = true; }
        cam.sNear = e.m_cam.sNear;
        cam.sFar = e.m_cam.sFar;
        cam.rcPicture = rc;
        m_rcLook = rc;
        e.m_cam = cam;
    }
    else if (m_fLookActive) { e.m_cam = m_camSaved; m_fLookActive = false; }
}

void CCutsceneEditor::RestoreScene()
{
    if ((!m_mpOrig.empty() || !m_setPosed.empty() || !e.m_aiSceneHidden.empty() || !m_mpPartsOrig.empty()) && m_pswSeen == e.m_psw.get())
    {
        for (auto& [pxf, o] : m_mpOrig)
        {
            pxf->mat = o.second;
            UpdateAloHierarchyWorlds(o.first);
        }
        for (auto& [palo, a] : m_mpPartsOrig) palo->aglobShown = a;
        for (ALO* palo : m_setPosed)
        {
            UpdateAloHierarchyWorlds(palo);
            e.UnbindPosed(palo);
        }
        e.m_aiSceneHidden.clear();
        UpdateSceneFromSw(e.m_scene, e.m_apalo);
    }
    e.m_aiSceneHidden.clear();
    m_setPosed.clear();
    m_mpOrig.clear();
    m_mpPartsOrig.clear();
    if (m_fLookActive) { e.m_cam = m_camSaved; m_fLookActive = false; }
    m_tApplied = -1.0f;
}

static ALO* PaloRootOf(ALO* palo)
{
    while (palo && palo->paloParent) palo = palo->paloParent;
    return palo;
}

void CCutsceneEditor::Survey()
{
    if (!m_trace.fOk) { std::printf("editor: cutsurvey %d: not measured (%s)\n", m_oid, m_traceNote.c_str()); return; }
    std::unordered_map<const ALO*, int> mpentry;
    for (std::size_t k = 0; k < e.m_apalo.size() && k < e.m_scene.aglobset.size(); ++k) mpentry[e.m_apalo[k]] = int(k);
    std::set<const LO*> setAnimated;
    for (const auto& k : m_aoidAnimated)
        if (LO* p = PloOf(k[0], k[1], k[2])) setAnimated.insert(p);
    // (the samples by time: the scene applied once per time)
    std::map<float, std::vector<std::pair<const CSTRACE::SEEN*, glm::vec3>>> mpt;
    for (const CSTRACE::SEEN& d : m_trace.aseen)
        for (const auto& [t, pos] : d.at)
            if (t >= 0.0f && t <= m_tEnd + 1e-3f) mpt[t].push_back({&d, pos});
    const float tSaved = m_t;
    const bool fPreviewSaved = m_fPreview;
    m_fPreview = true;
    int cSample = 0, cMiss = 0;
    std::set<std::array<int, 3>> setModel, setMissed;
    std::map<std::string, int> mpreason;
    double dMax = 0.0;
    for (const auto& [t, a] : mpt)
    {
        m_t = t;
        m_tApplied = -1.0f;
        ApplyScene();
        const std::set<int> setHidden(e.m_aiSceneHidden.begin(), e.m_aiSceneHidden.end());
        for (const auto& [pd, pos] : a)
        {
            ++cSample;
            setModel.insert({pd->oid, pd->cid, pd->rank});
            LO* plo = PloOf(pd->oid, pd->cid, pd->rank);
            const char* szWhy = nullptr;
            double dist = 0.0;
            if (!plo || !FIsLoDerivedFrom(plo, EDCID(0x30))) szWhy = "not in the editor's world";
            else
            {
                ALO* palo = static_cast<ALO*>(plo);
                const auto it = mpentry.find(palo);
                XF* pxf = PxfObj(palo) ? PxfObj(palo) : palo->axf.empty() ? nullptr : palo->axf[0].get();
                std::vector<std::tuple<LO*, LO*, float>> aclip;
                if (it == mpentry.end()) szWhy = "no model in the editor's scene";
                else if (setHidden.count(it->second))
                {
                    szWhy = "left out by the preview";
                    if (m_setNotDrawnLast.count(plo)) szWhy = "left out by the preview (the game's draw state)";
                    for (const LO* p = plo; p; p = p->paloParent)
                        if (m_setOutLast.count(p)) { szWhy = "left out by the preview (an object out of the world)"; break; }
                }
                else if (e.FHidden(plo)) szWhy = "hidden in the editor";
                else if (setAnimated.count(plo) && m_fPoses && FAnimsOn(plo, t, aclip) && !PposeAt(PaloRootOf(palo), t)) szWhy = "not posed";
                else if (!pxf) szWhy = "no record";
                else if ((dist = glm::length(glm::vec3(pxf->matWorld[3]) - pos)) > 60.0) szWhy = "placed elsewhere";
                dMax = std::max(dMax, dist <= 60.0 ? dist : 0.0);
            }
            if (!szWhy) continue;
            ++cMiss;
            ++mpreason[szWhy];
            if (setMissed.insert({pd->oid, pd->cid, pd->rank}).second)
                std::printf("editor: cutsurvey %d miss: %d (%s, rank %d) at %.2f: %s%s\n", m_oid, pd->oid, SzClass(pd->cid), pd->rank, t, szWhy,
                            dist > 60.0 ? (" (" + std::to_string(int(dist)) + " away)").c_str() : "");
        }
    }
    m_t = tSaved;
    m_fPreview = fPreviewSaved;
    m_tApplied = -1.0f;
    ApplyScene();
    std::string reasons;
    for (const auto& [r, n] : mpreason) reasons += " " + r + ": " + std::to_string(n) + ";";
    std::printf("editor: cutsurvey %d: %zu models drawn in view, %d samples, %d missed (%zu models)%s; within %.1f of the game's place\n", m_oid, setModel.size(), cSample, cMiss,
                setMissed.size(), reasons.c_str(), dMax);
}

void CCutsceneEditor::Close()
{
    RestoreScene();
    fOpen = false;
    m_fPlaying = false;
}

// per frame (before the 3D view is drawn)
void CCutsceneEditor::Frame()
{
    if (!fOpen) return;
    if (m_pswList != e.m_psw.get()) BuildList();
    if (m_oid && m_pswSeen != e.m_psw.get())
    {
        // (the world was loaded again after an edit: the objects are new)
        m_mpOrig.clear();
        m_mpPartsOrig.clear();
        m_setPosed.clear();
        m_mppose.clear();
        m_fLookActive = false;
        Decode();
        WhoStarts();
    }
    else if (m_oid && m_pscpt) Decode();   // (the record's bytes: undo / redo / edits show at once)
    PollTrace();
    if (m_fRecording && ++m_pollRecord % 30 == 0)
    {
        std::error_code ec;
        if (cane::fs::exists(ce::pathOf(SessionPath()), ec)) { m_fRecording = false; e.Status(T("the play session is recorded: Make a track from it")); }
    }
    // (an edit, an undo or a redo: measured again once the edits pause)
    if (m_oid && (e.m_proj.history().size() != m_cHistory || e.m_proj.redoHistory().size() != m_cRedo))
    {
        m_cHistory = e.m_proj.history().size();
        m_cRedo = e.m_proj.redoHistory().size();
        m_fRemeasure = m_fTraceWanted;
        m_tEdit = glfwGetTime();
    }
    if (m_fRemeasure && !m_fTracePending && glfwGetTime() - m_tEdit > 0.6) { m_fRemeasure = false; StartTrace(); }
    if (m_fPlaying)
    {
        m_t += ImGui::GetIO().DeltaTime;
        const float tA = m_fLoop && m_tLoop1 > m_tLoop0 ? m_tLoop0 : 0.0f, tB = m_fLoop && m_tLoop1 > m_tLoop0 ? m_tLoop1 : m_tEnd;
        if (m_t > tB) { if (m_fLoop) m_t = tA; else { m_t = tB; m_fPlaying = false; } }
    }
    // (moving an object with the gizmo: the preview lets go first, so the edit is made on its own place)
    if (e.m_axisDrag >= 0 && !m_mpOrig.empty()) { RestoreScene(); m_fPreview = false; }
    ApplyScene();
}

void CCutsceneEditor::DrawGhosts()
{
    if (!m_fGhosts || !m_trace.fOk) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const CSTRACE::OBJ& o : m_trace.aobj)
    {
        if (!FCidDerivedFrom(o.cid, EDCID(0x34)) || o.akey.size() < 2) continue;
        // the path (onion skin: a dot each half second), the place at the playhead
        ImVec2 sPrev;
        bool fPrev = false;
        for (float t = o.akey.front().t; t <= o.akey.back().t + 1e-3f; t += 0.1f)
        {
            glm::vec3 p;
            glm::quat q;
            bool fIn;
            if (!FObjectAt(o, t, p, q, fIn)) break;
            ImVec2 s;
            const bool f = e.FProject(p, s);
            if (f && fPrev) dl->AddLine(sPrev, s, st::u32(st::cta, 0.55f), 2.0f);
            if (f && std::fmod(t + 1e-3f, 0.5f) < 0.1f) dl->AddCircleFilled(s, 3.0f, st::u32(st::cta, 0.8f));
            sPrev = s;
            fPrev = f;
        }
        // onion skin: the character posed as it is half a second, a second ... before (cool) and after (warm) the
        // playhead, as a skeleton (its skin's bones, each to the nearest bone above it)
        LO* plo = PloOf(o.oid, o.cid, o.rank);
        ALO* palo = plo && FIsLoDerivedFrom(plo, EDCID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
        XF* pxfObj = palo ? (PxfObj(palo) ? PxfObj(palo) : palo->axf.empty() ? nullptr : palo->axf[0].get()) : nullptr;
        if (m_fPoses && pxfObj)
        {
            const glm::mat4 W0 = pxfObj->matWorld;
            const glm::vec3 scale(glm::length(glm::vec3(W0[0])), glm::length(glm::vec3(W0[1])), glm::length(glm::vec3(W0[2])));
            for (float tk = std::ceil((m_t - 1.0f) / 0.5f) * 0.5f; tk <= m_t + 1.0f + 1e-3f; tk += 0.5f)
            {
                if (std::fabs(tk - m_t) < 0.1f || tk < o.akey.front().t - 1e-3f || tk > m_tEnd + 1e-3f) continue;
                const POSE* pp = PposeAt(palo, tk);
                glm::vec3 p;
                glm::quat q;
                bool fIn;
                if (!pp || !FObjectAt(o, tk, p, q, fIn)) continue;
                glm::mat4 W = glm::mat4_cast(q);
                for (int i = 0; i < 3; ++i) W[i] *= scale[i] > 1e-4f ? scale[i] : 1.0f;
                W[3] = glm::vec4(p, 1.0f);
                std::unordered_map<const XF*, glm::vec3> mppos;
                for (const auto& [pxf, rel] : *pp) mppos[pxf] = glm::vec3(W * rel[3]);
                // (the bones: the records the skins use; none: every record)
                std::set<const XF*> setBone;
                for (const auto& [pxf, rel] : *pp)
                    if (const auto* paab = pxf->palo ? PaabindOfAlo(pxf->palo) : nullptr)
                        for (const auto& ab : *paab)
                            for (const GLOBBONEBIND& b : ab)
                                if (b.pxfBone) setBone.insert(b.pxfBone);
                const float u = 1.0f - std::fabs(tk - m_t) / 1.25f;
                const uint32_t col = tk < m_t ? st::u32(st::accent, 0.25f + 0.5f * u) : st::u32(0xF08A4B, 0.25f + 0.5f * u);
                for (const auto& [pxf, rel] : *pp)
                {
                    if (!setBone.empty() && !setBone.count(pxf)) continue;
                    const XF* pu = pxf->pxfRef;
                    while (pu && mppos.count(pu) && !setBone.empty() && !setBone.count(pu)) pu = pu->pxfRef;
                    if (!pu || !mppos.count(pu) || glm::length(mppos[pu] - mppos[pxf]) > 120.0f) continue;   // (a bone, not a helper far off)
                    ImVec2 a, b;
                    if (e.FProject(mppos[pu], a) && e.FProject(mppos[pxf], b)) dl->AddLine(a, b, col, 2.0f);
                }
            }
        }
        glm::vec3 p;
        glm::quat q;
        bool fIn;
        ImVec2 s;
        if (FObjectAt(o, m_t, p, q, fIn) && e.FProject(p + glm::vec3(0, 0, 60), s))
        {
            dl->AddCircle(s, 9.0f, st::u32(st::cta), 0, 2.0f);
            const std::string n = NameOf(o.oid);
            dl->AddText(ImVec2(s.x + 12, s.y - 8), st::u32(st::text), n.c_str());
        }
    }
}

// ---- edits

int CCutsceneEditor::IargOfSlot(const CSCMD& c, int ib, int* picomp) const
{
    if (!c.pbuilder) return -1;
    for (std::size_t j = 0; j < c.pbuilder->alay.size(); ++j)
    {
        const SCPTARGSLOT& as = c.pbuilder->alay[j];
        if (as.ib == ib) { if (picomp) *picomp = 0; return int(j); }
        if (as.otyp == 2 && ib > as.ib && ib < as.ib + 12) { if (picomp) *picomp = (ib - as.ib) / 4; return int(j); }
    }
    return -1;
}

bool CCutsceneEditor::FPatchValue(CSCMD& c, int iarg, int icomp, double v, const std::string& label)
{
    if (c.ifield < 0 || iarg < 0 || iarg >= int(c.aval.size())) { e.Status(T("this command's record cannot be edited")); return false; }
    const CSVAL& val = c.aval[std::size_t(iarg)];
    const int k = val.w & 0x7FFF;
    uint8_t b[4] = {};
    int cb = val.cb;
    std::size_t ib = val.ib;
    if (k == 2 || k == 9 || k == 10)
    {
        const float g = float(v);
        if (!std::isfinite(g)) return false;
        std::memcpy(b, &g, 4);
        ib += std::size_t(4 * icomp);
        cb = 4;
    }
    else if (FFloatKind(k))
    {
        const float g = float(v);
        if (!std::isfinite(g)) return false;
        std::memcpy(b, &g, 4);
    }
    else if (cb >= 1 && cb <= 4)
    {
        const int64_t n = int64_t(std::llround(v));
        for (int i = 0; i < cb; ++i) b[i] = uint8_t(uint64_t(n) >> (8 * i));
    }
    else { e.Status(T("this value cannot be edited")); return false; }
    e.m_proj.begin(label);
    const bool fOk = e.m_proj.patch(c.rel, ib, b, std::size_t(cb));
    e.m_proj.end();
    if (fOk) e.Status(label);
    return fOk;
}

bool CCutsceneEditor::FSetSlot(CSCMD& c, int ib, double v, const std::string& label)
{
    int icomp = 0;
    const int iarg = IargOfSlot(c, ib, &icomp);
    if (iarg < 0) { e.Status(T("this value is not in the record (the game sets it)")); return false; }
    const SCPTARGSLOT& as = c.pbuilder->alay[std::size_t(iarg)];
    if (as.otyp == 2 && as.fRad) v = v / 0.017453294;   // (the record keeps degrees)
    return FPatchValue(c, iarg, icomp, v, label);
}

// a new value's bytes (its kind word and value), as the record's other commands give that argument
bool CCutsceneEditor::FValueBytes(int kind, int otyp, double v, std::vector<uint8_t>& ab) const
{
    (void)kind;
    // (the kinds the worlds use for the builders' argument types: 1 a float, 2 a vector, else a 32-bit int (kind 6)
    // or a bool (kind 0); notes/CUTSCENES)
    uint16_t w = otyp == 1 ? 1 : otyp == 2 ? 2 : otyp == 0 ? 0 : 6;
    ab.insert(ab.end(), reinterpret_cast<uint8_t*>(&w), reinterpret_cast<uint8_t*>(&w) + 2);
    if (w == 1) { const float g = float(v); ab.insert(ab.end(), reinterpret_cast<const uint8_t*>(&g), reinterpret_cast<const uint8_t*>(&g) + 4); }
    else if (w == 2) { const float g[3] = {float(v), float(v), float(v)}; ab.insert(ab.end(), reinterpret_cast<const uint8_t*>(g), reinterpret_cast<const uint8_t*>(g) + 12); }
    else if (w == 0) ab.push_back(uint8_t(v != 0));
    else { const int32_t n = int32_t(std::llround(v)); ab.insert(ab.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4); }
    return true;
}

bool CCutsceneEditor::FInsertCommand(int iBefore, int kind, int variant, const std::vector<std::pair<int, double>>& aset, const std::string& label)
{
    const cut::BUILDER* pb = cut::PbuilderOfKind(kind, variant);
    if (!pb) { e.Status(T("no builder for that command in this game")); return false; }
    if (!m_decodeNote.empty()) { e.Status(m_decodeNote); return false; }
    // where: before command iBefore's option (else after the last command's)
    std::string rel;
    std::size_t ib = 0;
    if (iBefore >= 1 && iBefore < int(m_acmd.size()) && m_acmd[std::size_t(iBefore)].ifield >= 0)
        rel = m_acmd[std::size_t(iBefore)].rel, ib = m_acmd[std::size_t(iBefore)].ibOpt;
    else
    {
        for (int i = int(m_acmd.size()) - 1; i >= 1 && rel.empty(); --i)
            if (m_acmd[std::size_t(i)].ifield >= 0) rel = m_acmd[std::size_t(i)].rel, ib = m_acmd[std::size_t(i)].ibOpt + m_acmd[std::size_t(i)].cbOpt;
    }
    if (rel.empty()) { e.Status(T("the script has no commands in its record to add next to")); return false; }
    // its values: a copy of another command of that builder (the kinds its values are stored as), else made
    std::vector<uint8_t> abOpt;
    const int16_t eopid = int16_t(pb->eopid);
    abOpt.insert(abOpt.end(), reinterpret_cast<const uint8_t*>(&eopid), reinterpret_cast<const uint8_t*>(&eopid) + 2);
    std::vector<std::size_t> aibVal(std::size_t(pb->cArgs), 0);
    std::vector<int> akind(std::size_t(pb->cArgs), -1);
    bool fCopied = false;
    for (const EDITFIELD& f : e.m_rec.afield)
    {
        if (f.fk != EDITFK_OptionArray || f.eopid != pb->eopid || f.ibEnd <= f.ib) continue;
        std::string r2;
        std::size_t i2 = 0;
        const std::vector<uint8_t>* pab = nullptr;
        if (!e.FLocate(f.ib, f.ibEnd - f.ib, r2, i2) || !(pab = e.m_proj.partRef(r2)) || i2 + (f.ibEnd - f.ib) > pab->size()) continue;
        std::size_t i = i2;
        const std::size_t iEnd = i2 + (f.ibEnd - f.ib);
        std::vector<uint8_t> abv;
        bool fOk = true;
        for (int j = 0; j < pb->cArgs && fOk; ++j)
        {
            if (i + 2 > iEnd) { fOk = false; break; }
            uint16_t w;
            std::memcpy(&w, pab->data() + i, 2);
            if (w & 0x8000) { fOk = false; break; }   // (a list: made below)
            const int cb = CbOfValueKind(w);
            akind[std::size_t(j)] = w;
            aibVal[std::size_t(j)] = abOpt.size() + abv.size() + 2;
            abv.insert(abv.end(), pab->data() + i, pab->data() + i + 2 + std::size_t(cb));
            i += 2 + std::size_t(cb);
        }
        if (!fOk) continue;
        abOpt.insert(abOpt.end(), abv.begin(), abv.end());
        fCopied = true;
        break;
    }
    if (!fCopied)
        for (int j = 0; j < pb->cArgs; ++j)
        {
            const int otyp = pb->alay[std::size_t(j)].otyp;
            if (otyp == 0x8006)
            {
                const uint16_t w = 0x8006;
                abOpt.insert(abOpt.end(), reinterpret_cast<const uint8_t*>(&w), reinterpret_cast<const uint8_t*>(&w) + 2);
                abOpt.push_back(0);
                continue;
            }
            std::vector<uint8_t> abv;
            FValueBytes(kind, otyp, otyp == 1 ? 0.0 : otyp == 2 ? 0.0 : (otyp == 0 ? 0.0 : -1.0), abv);
            akind[std::size_t(j)] = abv[0] | (abv[1] << 8);
            aibVal[std::size_t(j)] = abOpt.size() + 2;
            abOpt.insert(abOpt.end(), abv.begin(), abv.end());
        }
    // the values asked for (by slot)
    for (const auto& [slot, v0] : aset)
        for (std::size_t j = 0; j < pb->alay.size(); ++j)
        {
            const SCPTARGSLOT& as = pb->alay[j];
            int icomp = -1;
            if (as.ib == slot) icomp = 0;
            else if (as.otyp == 2 && slot > as.ib && slot < as.ib + 12) icomp = (slot - as.ib) / 4;
            if (icomp < 0 || akind[j] < 0) continue;
            // (a bool value given another number than 0 / 1: stored as a 32-bit int (kind 6), which the loader hands to
            // the builder as it is; the custom title card's "no waiting" word 0x21, editor_cutpresent.inc)
            if (akind[j] == 0 && v0 != 0.0 && v0 != 1.0)
            {
                const std::size_t ibv = aibVal[j];
                const uint16_t w6 = 6;
                std::memcpy(abOpt.data() + ibv - 2, &w6, 2);
                abOpt.insert(abOpt.begin() + std::ptrdiff_t(ibv) + 1, 3, uint8_t(0));
                for (std::size_t q = 0; q < aibVal.size(); ++q)
                    if (aibVal[q] > ibv) aibVal[q] += 3;
                akind[j] = 6;
            }
            const int k = akind[j];
            double v = v0;
            if (as.otyp == 2 && as.fRad) v /= 0.017453294;
            uint8_t* p = abOpt.data() + aibVal[j];
            if (k == 2 || k == 9) { const float g = float(v); std::memcpy(p + 4 * icomp, &g, 4); }
            else if (FFloatKind(k)) { const float g = float(v); std::memcpy(p, &g, 4); }
            else
            {
                const int cb = CbOfValueKind(k);
                const int64_t n = int64_t(std::llround(v));
                for (int q = 0; q < cb; ++q) p[q] = uint8_t(uint64_t(n) >> (8 * q));
            }
        }
    e.m_proj.begin(label, true);
    const bool fOk = e.m_proj.spliceBytes(rel, ib, 0, abOpt, e.EnclosingSizeFields(rel, ib));
    e.m_proj.end();
    if (!fOk) { e.Status(T("could not write")); return false; }
    e.Status(label);
    const int oidSel = e.m_plSel ? e.m_plSel->oid : 0;
    e.Reload(oidSel);
    return true;
}

bool CCutsceneEditor::FDeleteCommand(int i)
{
    if (i < 1 || i >= int(m_acmd.size())) return false;
    CSCMD& c = m_acmd[std::size_t(i)];
    if (c.ifield < 0) { e.Status(T("this command's record cannot be edited")); return false; }
    // a block goes with its commands (its opening to its end)
    int iLast = i;
    if (c.kind == 0)
    {
        int d = 0;
        for (int k = i + 1; k < int(m_acmd.size()); ++k)
        {
            if (m_acmd[std::size_t(k)].kind == 0) ++d;
            else if (m_acmd[std::size_t(k)].kind == 1 && d-- == 0) { iLast = k; break; }
        }
    }
    if (c.kind == 1) { e.Status(T("delete the block's start instead")); return false; }
    const CSCMD& cl = m_acmd[std::size_t(iLast)];
    if (cl.ifield < 0 || cl.rel != c.rel) return false;
    const std::string label = std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("delete") + " " + Label(c);
    e.m_proj.begin(label, true);
    const bool fOk = e.m_proj.spliceBytes(c.rel, c.ibOpt, (cl.ibOpt + cl.cbOpt) - c.ibOpt, {}, e.EnclosingSizeFields(c.rel, c.ibOpt));
    e.m_proj.end();
    if (!fOk) { e.Status(T("could not write")); return false; }
    e.Status(label);
    m_isel = -1;
    e.Reload(e.m_plSel ? e.m_plSel->oid : 0);
    return true;
}

// the slot that is the clip's own length (-1: none)
int CCutsceneEditor::IbDuration(const CSCMD& c) const
{
    switch (c.kind)
    {
    case 0x23: case 0x33: case 0x52: return 0x10;
    case 0x2C: case 0x2D: case 0x53: return 0x14;
    case 0x38: return 0x40;
    default: return -1;
    }
}

bool CCutsceneEditor::FResizeClip(int i, float dur)
{
    CSCMD& c = m_acmd[std::size_t(i)];
    const int ib = IbDuration(c);
    if (ib < 0) return false;
    dur = std::max(0.0f, std::round(dur * 60.0f) / 60.0f);
    return FSetSlot(c, ib, dur, std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + Label(c) + " " + T("lasts") + " " + std::to_string(dur).substr(0, 5) + " s");
}

// a clip moved by dt: the wait before it on its track longer / shorter (a wait added when there is none)
bool CCutsceneEditor::FMoveClip(int i, float dt)
{
    dt = std::round(dt * 60.0f) / 60.0f;
    if (std::fabs(dt) < 1e-3f || i < 1) return false;
    CSCMD& c = m_acmd[std::size_t(i)];
    // the command before it in its block (not a nested block's)
    int iPrev = -1;
    for (int k = i - 1; k >= 1; --k)
    {
        const CSCMD& q = m_acmd[std::size_t(k)];
        if (q.icBlock == c.icBlock && q.kind != 1) { iPrev = k; break; }
        if (q.kind == 0 && q.ic == c.icBlock) break;
        if (q.kind == 1 && q.icBlock == c.icBlock)
        {
            // (a nested block before it: the command before that block)
            continue;
        }
    }
    if (iPrev >= 1)
    {
        CSCMD& p = m_acmd[std::size_t(iPrev)];
        if (p.kind == 0x23 && p.an[1] == -1 && p.an[2] == -1)
        {
            const float g = p.G(0x10) + dt;
            if (g <= 1e-3f) return FDeleteCommand(iPrev);
            return FSetSlot(p, 0x10, g, std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("wait") + " " + std::to_string(g).substr(0, 5) + " s");
        }
    }
    if (dt < 0.0f) { e.Status(T("there is no wait before it to shorten")); return false; }
    return FInsertCommand(i, 0x23, 0, {{0x10, dt}, {0x14, -1}, {0x18, -1}}, std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("a wait of") + " " +
                                                                                 std::to_string(dt).substr(0, 5) + " s " + T("before") + " " + Label(c));
}

// a camera shot's view (its start, or its end for "Shot end") from the editor's view
bool CCutsceneEditor::FShotFromView(int i)
{
    CSCMD& c = m_acmd[std::size_t(i)];
    if (c.kind != 0x38 && c.kind != 0x39 && c.kind != 0x3F) return false;
    const CAMERA& cam = m_fLookActive ? m_camSaved : e.m_cam;
    glm::mat3 m;
    if (cam.fMat) m = cam.mat;
    else
    {
        const glm::vec3 d = cam.Dir();
        const glm::vec3 left = glm::normalize(glm::cross(glm::vec3(0, 0, 1), d));
        m = glm::mat3(d, left, glm::cross(d, left));
    }
    const glm::vec3 eul = cmm::DecomposeEulerMatrix(m);
    const std::string label = std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + Label(c) + " " + T("from the view");
    e.m_proj.begin(label);
    bool fOk = true;
    int icomp = 0;
    const int ipos = IargOfSlot(c, 0x10, &icomp), iang = IargOfSlot(c, 0x20, &icomp), ifov = IargOfSlot(c, 0x30, &icomp);
    for (int q = 0; q < 3 && fOk; ++q) fOk = FPatchValue(c, ipos, q, cam.pos[q], label) && FPatchValue(c, iang, q, double(eul[q]) / 0.017453294, label);
    if (fOk && c.kind != 0x3F) fOk = FPatchValue(c, ifov, 0, glm::radians(cam.fovY), label);
    e.m_proj.end();
    return fOk;
}

void CCutsceneEditor::PlayInGame(float t)
{
    if (!e.FSave()) return;
#ifdef _WIN32
    const auto exe = cane::executableDirectory() / EDGAME_EXE ".exe";
#else
    const auto exe = cane::executableDirectory() / EDGAME_EXE;
#endif
    char sz[64];
    std::snprintf(sz, sizeof sz, "%d@%.3f", m_oid, t);
    std::error_code ec;
    std::vector<std::string> args{"--data-dir", ce::utf8(cane::fs::absolute(e.m_inst.dir, ec)), "--user-dir", cane::utf8(cane::fs::absolute(cane::userDirectory(), ec)),
                                  "--mod-package", ce::utf8(cane::fs::absolute(e.m_proj.dir, ec)), "--world", e.WorldArg(e.m_eo.record), "--play-scpt", sz};
    if (e.m_fPlayDry)
    {
        std::string line;
        for (std::size_t i = 4; i < args.size(); ++i) line += (i > 4 ? " " : "") + args[i];
        std::printf("editor: play: %s\n", line.c_str());
        return;
    }
    if (!ce::spawnDetached(exe, args)) e.Status("could not start " + ce::utf8(exe));
    else e.Status(std::string(T("the cutscene plays in the game from")) + " " + std::to_string(t).substr(0, 5) + " s");
}

// A new cutscene: a copy of the world's smallest script, its commands replaced by a short scene (the game paused, a
// camera shot from the view, a wait, the camera given back)
int CCutsceneEditor::CreateCutscene(std::string& error)
{
    int iobjBest = -1, cBest = INT_MAX;
    for (std::size_t i = 0; i < e.m_rec.aobj.size(); ++i)
    {
        const EDITOBJREC& r = e.m_rec.aobj[i];
        if (r.cid != EDCID(0x72) || !r.plo || r.depth != 0) continue;
        const int c = CScptCommands(r.plo);
        bool fBuilders = false;
        if (const auto it = e.m_mpobj.find(r.plo); it != e.m_mpobj.end())
            for (int k : it->second.aiarr) fBuilders = fBuilders || cut::PbuilderOfEopid(e.m_rec.afield[std::size_t(k)].eopid);
        if (fBuilders && c > 1 && c < cBest) cBest = c, iobjBest = int(i);
    }
    if (iobjBest < 0) { error = T("no script in this world to start from"); return 0; }
    // (into a group that has scripts, so the new scene can have a script module of its own: its fades, its events'
    // handlers; the level's group, else another level group, else the copied script's own; the copy's module reset)
    int igrpDest = e.LevelGroup();
    auto fScripts = [&](int ig) { return ig >= 0 && ig < int(e.m_index.groups.size()) && !e.m_index.groups[std::size_t(ig)].scriptsPart.empty(); };
    if (!fScripts(igrpDest))
    {
        int igrpAlt = -1;
        for (const ce::GroupInfo& g : e.m_index.groups)
            if (g.level && !g.scriptsPart.empty() && !g.objectsPart.empty() && igrpAlt < 0) igrpAlt = g.index;
        std::string relSrc;
        std::size_t ibSrc = 0, cbSrc = 0;
        if (igrpAlt < 0 && e.FRecordRange(iobjBest, relSrc, ibSrc, cbSrc))
            for (const ce::GroupInfo& g : e.m_index.groups)
                if (g.objectsPart == relSrc && !g.scriptsPart.empty()) igrpAlt = g.index;
        if (igrpAlt >= 0) igrpDest = igrpAlt;
    }
    const int oid = e.CopyRecord(iobjBest, igrpDest, nullptr, false, error);
    if (!oid) return 0;
    for (std::size_t i = 0; i < e.m_rec.aobj.size(); ++i)
        if (e.m_rec.aobj[i].oid == oid && e.m_rec.aobj[i].cid == EDCID(0x72) && e.m_rec.aobj[i].depth == 0)
        {
            std::string rel;
            std::size_t ib = 0, cb = 0;
            const std::vector<uint8_t>* pab = nullptr;
            if (!e.FRecordRange(int(i), rel, ib, cb) || !(pab = e.m_proj.partRef(rel)) || ib + 16 > pab->size()) break;
            int16_t n;
            const std::size_t ibN = ib + IbOidOfRecord(pab->data() + ib) + 4;
            std::memcpy(&n, pab->data() + ibN, 2);
            if (n != -1)
            {
                n = -1;
                e.m_proj.begin(std::string(T("New cutscene")) + " " + std::to_string(oid) + ": " + T("no script module"));
                e.m_proj.patch(rel, ibN, &n, 2);
                e.m_proj.end();
                e.Reload(0);
            }
            break;
        }
    // its commands: all but the first removed, the first replaced by the new scene's
    Open(oid);
    if (m_acmd.size() < 2 || !m_decodeNote.empty()) { error = T("the copy's commands cannot be edited"); return oid; }
    const CAMERA& cam = e.m_cam;
    const glm::vec3 d = cam.Dir();
    const glm::vec3 left = glm::normalize(glm::cross(glm::vec3(0, 0, 1), d));
    const glm::vec3 eul = cmm::DecomposeEulerMatrix(glm::mat3(d, left, glm::cross(d, left)));
    const std::vector<std::pair<int, double>> view = {{0x10, cam.pos.x}, {0x14, cam.pos.y}, {0x18, cam.pos.z}, {0x20, eul.x}, {0x24, eul.y},
                                                      {0x28, eul.z}, {0x30, glm::radians(cam.fovY)}};
    // (added at the end in order, then the copy's own commands removed)
    const int cOld = int(m_acmd.size()) - 1;
    auto add = [&](int kind, std::vector<std::pair<int, double>> a, const char* what) {
        Decode();
        return FInsertCommand(int(m_acmd.size()), kind, 0, a, std::string(T("New cutscene")) + " " + std::to_string(oid) + ": " + T(what));
    };
    bool fOk = add(0x12, {{0x10, 1}, {0x14, 3}}, "pause the game");
    auto shot = view;
    shot.push_back({0x40, 3.0});
    shot.push_back({0x44, 0});
    fOk = fOk && add(0x38, shot, "camera shot");
    auto end = view;
    end.push_back({0x40, 0});
    end.push_back({0x44, 1});
    fOk = fOk && add(0x39, end, "shot end") && add(0x3A, {{0x10, 1}}, "camera back") && add(0x12, {{0x10, 0}, {0x14, 3}}, "unpause");
    for (int k = 0; fOk && k < cOld; ++k)
    {
        Decode();
        int i = 1;
        while (i < int(m_acmd.size()) && m_acmd[std::size_t(i)].kind == 1) ++i;
        fOk = FDeleteCommand(i);
    }
    if (!fOk) error = T("the new scene's commands could not be written");
    Open(oid);
    return oid;
}

// ---- character paths (a path object the character follows, its points from the ghost)

bool CCutsceneEditor::FPathPoints(LO* ppath, std::vector<glm::vec3>& aw) const
{
    PATHDATA pd;
    if (!ppath || !e.FPathRead(ppath, pd)) return false;
    const glm::mat4 m = e.MatPathSpace(ppath);
    aw.clear();
    for (const glm::vec3& p : pd.apos) aw.push_back(glm::vec3(m * glm::vec4(p, 1.0f)));
    return true;
}

// the path's points replaced (any count; straight segments: its curve kind linear)
bool CCutsceneEditor::FSetPathPoints(LO* ppath, const std::vector<glm::vec3>& aw, const std::string& label, int curved)
{
    const auto it = e.m_mpobj.find(ppath);
    if (it == e.m_mpobj.end() || it->second.ipath < 0 || aw.size() < 2 || aw.size() > 4000) { e.Status(T("not a path stored in the level")); return false; }
    const EDITFIELD& f = e.m_rec.afield[std::size_t(it->second.ipath)];
    const std::size_t c = std::size_t(f.i);
    const std::size_t cbOld = 4 + c * 16 + (f.k ? c * 24 : 0);
    std::string rel;
    std::size_t ib = 0;
    if (!e.FLocate(f.ib, cbOld, rel, ib))
    {
        if (std::getenv("SLY2N_CUTDBG")) std::printf("editor: path %d: ib %zu c %zu k %d fk %d\n", ppath->oid, f.ib, c, f.k, int(f.fk));
        e.Status(T("not a path stored in the level"));
        return false;
    }
    if (curved < 0)
    {
        PATHDATA pd;
        curved = e.FPathRead(ppath, pd) && pd.crvk ? 1 : 0;
    }
    const glm::mat4 toPath = glm::inverse(e.MatPathSpace(ppath));
    std::vector<glm::vec3> ap;
    for (const glm::vec3& w : aw)
    {
        const glm::vec3 p(toPath * glm::vec4(w, 1.0f));
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        ap.push_back(p);
    }
    auto put = [](std::vector<uint8_t>& ab, const void* pv, std::size_t cb) { ab.insert(ab.end(), static_cast<const uint8_t*>(pv), static_cast<const uint8_t*>(pv) + cb); };
    std::vector<uint8_t> ab;
    ab.push_back(uint8_t(curved ? 1 : 0));   // (straight segments, or a curve through the points: crv.cpp's Hermite)
    ab.push_back(0);                          // (open)
    const uint16_t n = uint16_t(ap.size());
    put(ab, &n, 2);
    // (the points' parameters: their numbers, as the levels' own paths have them)
    for (std::size_t i = 0; i < ap.size(); ++i) { const float g = float(i); put(ab, &g, 4); }
    for (const glm::vec3& p : ap) put(ab, &p, 12);
    if (curved)
    {
        // (each point's tangent the Catmull-Rom one, per unit of parameter; in and out the same: a smooth curve)
        std::vector<glm::vec3> av(ap.size());
        for (std::size_t i = 0; i < ap.size(); ++i)
            av[i] = i == 0 ? ap[1] - ap[0] : i + 1 == ap.size() ? ap[i] - ap[i - 1] : (ap[i + 1] - ap[i - 1]) * 0.5f;
        for (const glm::vec3& v : av) put(ab, &v, 12);
        for (const glm::vec3& v : av) put(ab, &v, 12);
    }
    e.m_proj.begin(label, true);
    const bool fOk = e.m_proj.spliceBytes(rel, ib, cbOld, ab, e.EnclosingSizeFields(rel, ib));
    e.m_proj.end();
    if (!fOk) { e.Status(T("could not write")); return false; }
    e.Status(label);
    e.Reload(ppath->oid);
    return true;
}

// The Catmull-Rom curve through the points (as FSetPathPoints writes it), cPerSeg points a segment
std::vector<glm::vec3> CCutsceneEditor::CurvePoints(const std::vector<glm::vec3>& aw, int cPerSeg)
{
    if (aw.size() < 3) return aw;
    std::vector<glm::vec3> av(aw.size()), a;
    for (std::size_t i = 0; i < aw.size(); ++i)
        av[i] = i == 0 ? aw[1] - aw[0] : i + 1 == aw.size() ? aw[i] - aw[i - 1] : (aw[i + 1] - aw[i - 1]) * 0.5f;
    for (std::size_t i = 0; i + 1 < aw.size(); ++i)
        for (int k = 0; k < cPerSeg; ++k)
        {
            const float u = float(k) / float(cPerSeg), w = 1.0f - u;
            const float uw = u * w * w, u2w = u * u * w;
            a.push_back(aw[i] * (w * w * w + uw * 3.0f) + av[i] * uw - av[i + 1] * u2w + aw[i + 1] * (u * u * u + u2w * 3.0f));
        }
    a.push_back(aw.back());
    return a;
}

int CCutsceneEditor::IobjMarker() const
{
    for (const auto& up : e.m_psw->aploAll)
    {
        if (!FIsLoDerivedFrom(up.get(), EDCID(0x72))) continue;
        int kind = 0, b = 0;
        int32_t an[18];
        for (int i = 0; FScptCommand(up.get(), i, kind, b, an); ++i)
            if (kind == 0x24)
                if (LO* pt = PloOfOid(an[0]); pt && !FIsLoDerivedFrom(pt, EDCID(0x34)) && e.FHasMatrix(pt)) return e.IobjOfLo(pt);
    }
    return -1;
}

// a new path object (a copy of the level's plainest one) through these points
int CCutsceneEditor::NewPath(const std::vector<glm::vec3>& aw, std::string& error, bool fCurved)
{
    const int iobj = e.IobjExampleOf(EDCID(0x0A));
    if (iobj < 0 || aw.empty()) { error = T("this level has no path to copy"); return 0; }
    const glm::vec3 p0 = aw.front();
    const int oid = e.CopyRecord(iobj, e.CopyGroup(), &p0, false, error);
    if (!oid) return 0;
    LO* pp = PloOfOid(oid);
    if (std::getenv("SLY2N_CUTDBG"))
    {
        const auto it = pp ? e.m_mpobj.find(pp) : e.m_mpobj.end();
        std::printf("editor: new path %d: lo %p cid 0x%X ipath %d\n", oid, static_cast<void*>(pp), pp ? int(pp->Cid()) : -1, it != e.m_mpobj.end() ? it->second.ipath : -9);
    }
    if (!pp || !FSetPathPoints(pp, aw, std::string(T("Cutscene path")) + " " + std::to_string(oid), fCurved ? 1 : 0)) { error = T("the new path's points could not be written"); return 0; }
    return oid;
}

glm::vec3 CCutsceneEditor::ActorPosAt(int oidActor, float t, float* pheading) const
{
    for (const CSTRACE::OBJ& o : m_trace.aobj)
        if (o.oid == oidActor)
        {
            glm::vec3 p;
            glm::quat q;
            bool fIn;
            if (FObjectAt(o, t, p, q, fIn))
            {
                if (pheading) { const glm::vec3 x = glm::mat3_cast(q)[0]; *pheading = std::atan2(x.y, x.x); }
                return p;
            }
        }
    LO* pa = PloOfOid(oidActor);
    if (pheading) *pheading = 0.0f;
    if (!pa) return e.m_cam.pos + e.m_cam.Dir() * 600.0f;
    // (the preview may have moved it: its own place)
    const glm::vec3 p = e.PosWorld(pa);
    if (pheading) { const glm::mat4 m = e.MatWorld(pa); *pheading = std::atan2(m[0].y, m[0].x); }
    return p;
}

// the main track's command a new track that starts at t goes before (after the last that starts by then)
int CCutsceneEditor::InsertPoint(float t) const
{
    // the main track's first command still holding it at t (or starting after it): a new track put before it starts
    // when the main track reaches it, then waits the rest
    for (const CSCMD& c : m_acmd)
        if (c.ic > 1 && c.icBlock == 0 && c.kind != 1 && c.tExec + c.tHold > t + 1e-3f) return c.ic;
    return int(m_acmd.size());
}
float CCutsceneEditor::TFreeAt(int iBefore) const
{
    if (iBefore >= 0 && iBefore < int(m_acmd.size())) return m_acmd[std::size_t(iBefore)].tExec;
    float t = 0.0f;
    for (const CSCMD& c : m_acmd)
        if (c.icBlock == 0 && c.ic > 0) t = std::max(t, c.tExec + c.tHold);
    return t;
}

bool CCutsceneEditor::FNewCharacterPath(int oidActor, float t, const std::vector<glm::vec3>& aw0, bool fWalk, float speed, int oidAnim, std::string& error,
                                         bool fCurved, float faceDeg)
{
    if (oidActor < 0 || !PloOfOid(oidActor)) { error = T("choose a character first"); return false; }
    if (!m_decodeNote.empty()) { error = m_decodeNote; return false; }
    if (aw0.size() < 2) { error = T("a path needs two points"); return false; }
    const int oidScene = m_oid;
    const int iBefore = InsertPoint(t);
    const float tWait = std::max(0.0f, t - TFreeAt(iBefore));
    const bool fPlayer = FIsLoDerivedFrom(PloOfOid(oidActor), EDCID(0x35));
    // the player is steered (as the scenes steer Sly: "Go to" each point, a marker there: a curve sampled); the others
    // follow a path object
    const std::vector<glm::vec3> aw = fPlayer && fCurved ? CurvePoints(aw0, 4) : aw0;
    const int iobjMark = IobjMarker();
    if (iobjMark < 0) { error = T("this level has no marker to copy for the player's points"); return false; }
    // (the new objects first: each one loads the level again)
    std::vector<int> aoid;
    int oidPath = 0, oidFace = -1;
    if (fPlayer)
        for (std::size_t k = 1; k < aw.size(); ++k)
        {
            const int oid = e.CopyRecord(iobjMark, e.CopyGroup(), &aw[k], false, error);
            if (!oid) return false;
            aoid.push_back(oid);
        }
    else if (!(oidPath = NewPath(aw, error, fCurved))) return false;
    // (the others: steered to the path's start first, as a scene takes a character: its jobs of before released, so
    // the follow is its own and ends when it arrives; a job left from before held "Follow path" up for good)
    int oidFirst = -1;
    if (!fPlayer && !(oidFirst = e.CopyRecord(iobjMark, e.CopyGroup(), &aw[0], false, error))) return false;
    if (std::isfinite(faceDeg))
    {
        // (facing at the end: a marker a little way off in that direction, the character turned toward it)
        const float r = glm::radians(faceDeg);
        const glm::vec3 pf = aw.back() + glm::vec3(std::cos(r), std::sin(r), 0.0f) * 150.0f;
        if (!(oidFace = e.CopyRecord(iobjMark, e.CopyGroup(), &pf, false, error))) return false;
    }
    Open(oidScene);
    // a track of the character's own: [Track] [Wait] [Go to each point / Follow path] [Face] [Animation] [End] (the
    // track's id -1: the scene's "wait for tracks" name tracks by their character, and must not wait for this one)
    const std::string what = std::string(T("Cutscene")) + " " + std::to_string(oidScene) + ": " + NameOf(oidActor) + " " +
                             (fPlayer ? std::string(T("goes along")) + " " + std::to_string(aoid.size()) + " " + T("points") : std::string(T("follows path")) + " " + std::to_string(oidPath));
    int i = iBefore;
    bool fOk = FInsertCommand(i++, 0x00, 0, {{0x10, -1}}, what);
    if (fOk && tWait > 0.02f) { Decode(); fOk = FInsertCommand(i++, 0x23, 0, {{0x10, std::round(tWait * 60.0f) / 60.0f}, {0x14, -1}, {0x18, -1}}, what); }
    int icSel = -1;
    if (fPlayer)
        for (int oid : aoid)
        {
            if (fOk) { Decode(); fOk = FInsertCommand(i++, 0x24, fWalk ? 0 : 1, {{0x10, oid}, {0x14, 1.0}, {0x18, 40.0}, {0x1C, oidActor}, {0x20, -1}, {-2, 0}}, what); }
        }
    else if (fOk)
    {
        // ("Avoid off": along the path as drawn; avoiding, the follower asks the level's navigation for a way to it
        // and stops where it finds none: Murray stood still in 1907)
        Decode();
        fOk = FInsertCommand(i++, 0x24, 0, {{0x10, oidFirst}, {0x14, 1.0}, {0x18, 40.0}, {0x1C, oidActor}, {0x20, -1}, {-2, 0}}, what);
        Decode();
        fOk = fOk && FInsertCommand(i++, 0x25, 0, {}, what);
        Decode();
        icSel = i;
        fOk = fOk && FInsertCommand(i++, 0x27, 0, {{0x10, oidPath}, {0x14, 1}, {0x18, fWalk ? 1 : 0}, {0x1C, speed > 0.0f ? std::min(speed, 1.0f) : 1.0f}, {0x20, oidActor}, {0x24, -1}}, what);
    }
    if (fOk && oidFace >= 0) { Decode(); fOk = FInsertCommand(i++, 0x23, 0, {{0x10, 0.5}, {0x14, oidFace}, {0x18, oidActor}}, what); }
    if (fOk && oidAnim > 0) { Decode(); fOk = FInsertCommand(i++, 0x0A, 0, {{0x10, oidAnim}, {0x14, oidActor}, {0x18, 0}, {-2, 0}}, what); }
    if (fOk) { Decode(); fOk = FInsertCommand(i++, 0x01, 0, {}, what); }
    if (!fOk) { error = T("the commands could not be written"); return false; }
    Decode();
    if (icSel >= 0) m_isel = icSel;
    return true;
}

// the characters of the scene (its tracks') and the level's actors, as a combo
bool CCutsceneEditor::FCharacterCombo(const char* label, int& oid)
{
    bool f = false;
    if (ImGui::BeginCombo(label, oid >= 0 ? NameOf(oid).c_str() : T("choose")))
    {
        std::set<int> set;
        for (const CSTRACK& tr : m_atrack)
            if (tr.tk == cut::TK_Actor && tr.oid >= 0) set.insert(tr.oid);
        for (const auto& up : e.m_psw->aploAll)
            if (FIsLoDerivedFrom(up.get(), EDCID(0x34)) && set.size() < 40) set.insert(up->oid);
        for (int o : set)
            if (ImGui::Selectable((NameOf(o) + "##pa" + std::to_string(o)).c_str(), o == oid)) { oid = o; f = true; }
        ImGui::EndCombo();
    }
    return f;
}

void CCutsceneEditor::DrawPathPanel()
{
    ImGui::SeparatorText(T("Character path"));
    FCharacterCombo(T("character"), m_oidPathActor);
    ImGui::Checkbox(T("walk"), &m_fPathWalk);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputFloat(T("speed"), &m_pathSpeed, 0, 0, "%.2f");
    EdTip(T("How far its stick is pushed: 1 its own walk / run speed, 0.5 half of it (0 would stand still)"));
    ImGui::SetNextItemWidth(70);
    ImGui::InputFloat(T("length"), &m_pathLength, 0, 0, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt(T("animation on arrival"), &m_oidArriveAnim, 0, 0);
    EdTip(T("An animation (ASEG) played when it arrives (-1: none)"));
    ImGui::Checkbox(T("curved"), &m_fPathCurved);
    EdTip(T("A smooth curve through its points (off: straight from point to point)"));
    ImGui::SameLine();
    ImGui::Checkbox(T("face at the end"), &m_fPathFace);
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_fPathFace);
    ImGui::SetNextItemWidth(70);
    ImGui::InputFloat("##facedeg", &m_pathFaceDeg, 0, 0, "%.0f deg");
    ImGui::SameLine();
    if (ImGui::SmallButton(T("the view's"))) { const glm::vec3 d = e.m_cam.Dir(); m_pathFaceDeg = glm::degrees(std::atan2(d.y, d.x)); }
    ImGui::EndDisabled();
    EdTip(T("Where the character turns to when it arrives (degrees: 0 along +x, 90 along +y); \"the view's\": the way the view looks"));
    if (ImGui::Button(T("New path from the playhead")))
    {
        float h = 0.0f;
        const glm::vec3 p0 = ActorPosAt(m_oidPathActor, m_t, &h);
        glm::vec3 d = e.m_cam.Dir();
        d.z = 0.0f;
        d = glm::length(d) < 1e-3f ? glm::vec3(std::cos(h), std::sin(h), 0) : glm::normalize(d);
        // (curved: the middle point a little to the side, so the curve shows)
        const glm::vec3 side = m_fPathCurved ? glm::vec3(-d.y, d.x, 0.0f) * (m_pathLength * 0.2f) : glm::vec3(0.0f);
        std::string error;
        if (!FNewCharacterPath(m_oidPathActor, m_t, {p0, p0 + d * (m_pathLength * 0.5f) + side, p0 + d * m_pathLength}, m_fPathWalk, m_pathSpeed, m_oidArriveAnim, error,
                               m_fPathCurved, m_fPathFace ? m_pathFaceDeg : NAN))
            e.Status(error);
    }
    EdTip(T("The character (its ghost at the playhead) walks a new path from where it is, along the view; drag its points in the view, add points, set walk / run and the speed on the clip"));
}

// ---- recorded play sessions

void CCutsceneEditor::RecordSession()
{
#ifdef _WIN32
    const auto exe = cane::executableDirectory() / EDGAME_EXE ".exe";
#else
    const auto exe = cane::executableDirectory() / EDGAME_EXE;
#endif
    std::error_code ec;
    cane::fs::create_directories(ce::pathOf(TraceDir()), ec);
    cane::fs::remove(ce::pathOf(SessionPath()), ec);
    std::vector<std::string> args{"--data-dir", ce::utf8(cane::fs::absolute(e.m_inst.dir, ec)), "--user-dir", cane::utf8(cane::fs::absolute(cane::userDirectory(), ec)),
                                  "--world", e.WorldArg(e.m_eo.record), "--record-session", ce::utf8(cane::fs::absolute(ce::pathOf(SessionPath()), ec))};
    if (e.m_fPlayDry) { std::printf("editor: record: %s\n", SessionPath().c_str()); return; }
    if (!ce::spawnDetached(exe, args)) { e.Status("could not start " + ce::utf8(exe)); return; }
    m_fRecording = true;
    e.Status(T("recording: play, then close the game; the recording comes back here"));
}

// A recording made into a track: the stops become waits, each stretch of moving a path the character follows (walk
// or run by its speed), the character placed at the start
bool CCutsceneEditor::FImportSession(const std::string& path, int oidActor, float t, std::string& error)
{
    std::vector<uint8_t> ab;
    if (!ce::readFile(ce::pathOf(path), ab)) { error = T("no recording"); return false; }
    const nlohmann::json j = nlohmann::json::parse(ab.begin(), ab.end(), nullptr, false);
    if (!j.is_object() || j.value("format", std::string()) != "sly2-play-session" || !j["steps"].is_array() || j["steps"].size() < 2) { error = T("not a play session"); return false; }
    // (each step: t oid x y z heading speed vz state anim stick_x stick_y buttons)
    struct STEP { float t; glm::vec3 p; float v; int kind = 0, anim = -1, btn = 0; };
    std::vector<STEP> as;
    for (const auto& r : j["steps"])
        if (r.is_array() && r.size() >= 7)
        {
            STEP st{r[0].get<float>(), {r[2].get<float>(), r[3].get<float>(), r[4].get<float>()}, r[6].get<float>()};
            if (r.size() >= 10) st.kind = r[8].get<int>(), st.anim = r[9].get<int>();
            if (r.size() >= 13) st.btn = r[12].get<int>();
            as.push_back(st);
        }
    if (oidActor < 0 && !j["steps"].empty()) oidActor = j["steps"][0][1].get<int>();
    // stretches: jumping (in the air: jump kind 1, at least 3 steps), moving (30 / s and faster), stopped (at least 0.4 s;
    // a shorter stop is part of the walk)
    enum { SK_Stop, SK_Move, SK_Jump };
    struct SEG { int k; std::size_t i0, i1; };
    std::vector<SEG> aseg;
    auto cat = [&](std::size_t i) { return as[i].kind == 1 ? SK_Jump : as[i].v >= 30.0f ? SK_Move : SK_Stop; };
    for (std::size_t i = 0; i < as.size();)
    {
        std::size_t k = i;
        int c = cat(i);
        while (k + 1 < as.size() && cat(k + 1) == c) ++k;
        if (c == SK_Jump && k - i < 2) c = SK_Move;
        if (!aseg.empty() && c == SK_Stop && as[k].t - as[i].t < 0.4f && aseg.back().k == SK_Move) aseg.back().i1 = k;
        else if (!aseg.empty() && aseg.back().k == c && c != SK_Jump) aseg.back().i1 = k;
        else aseg.push_back({c, i, k});
        i = k + 1;
    }
    // the points of a stretch: the steps simplified (within 25 units)
    auto simplify = [&](std::size_t i0, std::size_t i1) {
        std::vector<glm::vec3> a;
        for (std::size_t i = i0; i <= i1; ++i) a.push_back(as[i].p);
        std::vector<bool> keep(a.size(), false);
        keep.front() = keep.back() = true;
        std::function<void(std::size_t, std::size_t)> dp = [&](std::size_t lo, std::size_t hi) {
            if (hi <= lo + 1) return;
            float dMax = 0.0f;
            std::size_t iMax = lo;
            const glm::vec3 ab2 = a[hi] - a[lo];
            for (std::size_t i = lo + 1; i < hi; ++i)
            {
                const float l2 = glm::dot(ab2, ab2);
                const float u = l2 > 0 ? std::clamp(glm::dot(a[i] - a[lo], ab2) / l2, 0.0f, 1.0f) : 0.0f;
                const float d = glm::length(a[i] - (a[lo] + ab2 * u));
                if (d > dMax) dMax = d, iMax = i;
            }
            if (dMax > 25.0f) { keep[iMax] = true; dp(lo, iMax); dp(iMax, hi); }
        };
        dp(0, a.size() - 1);
        std::vector<glm::vec3> r;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (keep[i]) r.push_back(a[i]);
        return r;
    };
    const int oidScene = m_oid;
    // the player is steered to markers (as the scenes steer Sly), the others follow paths; jumps go to a marker where it
    // landed; made first (each a new object: the level is loaded again), then the commands
    const bool fPlayer = FIsLoDerivedFrom(PloOfOid(oidActor), EDCID(0x35));
    bool fJumps = false;
    for (const SEG& g : aseg) fJumps = fJumps || g.k == SK_Jump;
    const int iobjMark = IobjMarker();
    if (iobjMark < 0) { error = T("this level has no marker to copy for the player's points"); return false; }
    (void)fJumps;
    // (the others: steered to the start once placed, which releases their jobs of before: see FNewCharacterPath)
    int oidGo = -1;
    if (!fPlayer && !(oidGo = e.CopyRecord(iobjMark, e.CopyGroup(), &as[0].p, false, error))) return false;
    // a part: a wait, a move (markers / a path), a jump (to a marker) or an animation (an action while standing)
    enum { PK_Wait, PK_Move, PK_Jump, PK_Anim };
    struct PART { int k; float dt; std::vector<int> aoid; bool fWalk; float speed; int clip; };
    std::vector<PART> apart;
    int oidStart = -1;
    for (const SEG& g : aseg)
    {
        const float dt = as[g.i1].t - as[g.i0].t;
        if (g.k == SK_Jump)
        {
            const glm::vec3 pLand = as[std::min(g.i1 + 1, as.size() - 1)].p;
            const int oid = e.CopyRecord(iobjMark, e.CopyGroup(), &pLand, false, error);
            if (!oid) return false;
            apart.push_back({PK_Jump, dt, {oid}, true, 0, -1});
            continue;
        }
        if (g.k == SK_Stop)
        {
            // (an action while standing: a button pressed and a clip other than the stop's own starting within 0.3 s)
            const int idle = as[g.i0].anim;
            float tFrom = as[g.i0].t;
            for (std::size_t k = g.i0 + 1; k <= g.i1; ++k)
            {
                if (as[k].anim == as[k - 1].anim || as[k].anim == -1 || as[k].anim == idle) continue;
                bool fPressed = false;
                for (std::size_t q = k; q > g.i0 && as[k].t - as[q].t <= 0.3f; --q)
                    fPressed = fPressed || (as[q].btn & (0x80 | 0x20 | 0x10 | 0x08 | 0x04)) != 0;   // (the game's layout: square, circle, triangle, R1, L1)
                if (!fPressed) continue;
                std::size_t kEnd = k;
                while (kEnd + 1 <= g.i1 && as[kEnd + 1].anim == as[k].anim) ++kEnd;
                if (as[k].t - tFrom > 0.05f) apart.push_back({PK_Wait, as[k].t - tFrom, {}, true, 0, -1});
                apart.push_back({PK_Anim, as[kEnd].t - as[k].t, {}, true, 0, as[k].anim});
                tFrom = as[kEnd].t;
                k = kEnd;
            }
            if (as[g.i1].t - tFrom > 0.05f) apart.push_back({PK_Wait, as[g.i1].t - tFrom, {}, true, 0, -1});
            continue;
        }
        std::vector<glm::vec3> aw = simplify(g.i0, g.i1);
        if (aw.size() < 2 || glm::length(aw.back() - aw.front()) < 10.0f) { apart.push_back({PK_Wait, dt, {}, true, 0, -1}); continue; }
        float len = 0.0f;
        for (std::size_t i = 1; i < aw.size(); ++i) len += glm::length(aw[i] - aw[i - 1]);
        const float v = dt > 0.05f ? len / dt : 300.0f;
        PART pt{PK_Move, dt, {}, v < 450.0f, std::round(v), -1};
        if (fPlayer)
        {
            for (std::size_t k = oidStart < 0 ? 0 : 1; k < aw.size(); ++k)
            {
                const int oid = e.CopyRecord(iobjMark, e.CopyGroup(), &aw[k], false, error);
                if (!oid) return false;
                if (oidStart < 0) oidStart = oid; else pt.aoid.push_back(oid);
            }
        }
        else
        {
            const int oidPath = NewPath(aw, error);
            if (!oidPath) return false;
            if (oidStart < 0) oidStart = oidPath;
            pt.aoid.push_back(oidPath);
        }
        apart.push_back(std::move(pt));
    }
    Open(oidScene);
    if (!m_decodeNote.empty()) { error = m_decodeNote; return false; }
    const int iBefore = InsertPoint(t);
    const float tWait = std::max(0.0f, t - TFreeAt(iBefore));
    const std::string what = std::string(T("Cutscene")) + " " + std::to_string(oidScene) + ": " + T("the recording as the track of") + " " + NameOf(oidActor);
    int i = iBefore;
    bool fOk = FInsertCommand(i++, 0x00, 0, {{0x10, -1}}, what);
    if (fOk && tWait > 0.02f) { Decode(); fOk = FInsertCommand(i++, 0x23, 0, {{0x10, tWait}, {0x14, -1}, {0x18, -1}}, what); }
    // (the character put at the recording's start)
    if (fOk && oidStart >= 0) { Decode(); fOk = FInsertCommand(i++, 0x16, 0, {{0x10, oidStart}, {0x14, 0}, {0x18, oidActor}}, what); }
    if (fOk && !fPlayer)
    {
        Decode();
        fOk = FInsertCommand(i++, 0x24, 0, {{0x10, oidGo}, {0x14, 1.0}, {0x18, 40.0}, {0x1C, oidActor}, {0x20, -1}, {-2, 0}}, what);
        Decode();
        fOk = fOk && FInsertCommand(i++, 0x25, 0, {}, what);   // (along its paths as recorded: "Avoid off")
    }
    int cJump = 0, cAnim = 0;
    for (const PART& pt : apart)
    {
        if (!fOk) break;
        Decode();
        switch (pt.k)
        {
        case PK_Wait: fOk = FInsertCommand(i++, 0x23, 0, {{0x10, std::round(pt.dt * 60.0f) / 60.0f}, {0x14, -1}, {0x18, -1}}, what); break;
        case PK_Jump: ++cJump; fOk = FInsertCommand(i++, 0x2A, 0, {{0x10, pt.aoid[0]}, {0x14, 0}, {0x18, 0}, {0x1C, oidActor}, {-2, 0}}, what); break;
        case PK_Anim: ++cAnim; fOk = FInsertCommand(i++, 0x0A, 0, {{0x10, pt.clip}, {0x14, oidActor}, {0x18, 0}, {-2, 0}}, what); break;
        default:
            for (std::size_t q = 0; q < pt.aoid.size() && fOk; ++q)
            {
                if (q) Decode();
                if (fPlayer) fOk = FInsertCommand(i++, 0x24, pt.fWalk ? 0 : 1, {{0x10, pt.aoid[q]}, {0x14, 1.0}, {0x18, 40.0}, {0x1C, oidActor}, {0x20, -1}, {-2, 0}}, what);
                else fOk = FInsertCommand(i++, 0x27, 0, {{0x10, pt.aoid[q]}, {0x14, 1}, {0x18, pt.fWalk ? 1 : 0}, {0x1C, 1.0}, {0x20, oidActor}, {0x24, -1}}, what);
            }
            break;
        }
    }
    if (fOk) { Decode(); fOk = FInsertCommand(i++, 0x01, 0, {}, what); }
    if (!fOk) { if (error.empty()) error = T("the commands could not be written"); return false; }
    Decode();
    e.Status(what + " (" + std::to_string(apart.size()) + " " + T("parts") + ", " + std::to_string(cJump) + " " + T("jumps") + ", " + std::to_string(cAnim) + " " + T("animations") + ")");
    std::printf("editor: cutimport: %zu parts, %d jumps, %d animations\n", apart.size(), cJump, cAnim);
    return true;
}

// ---- camera keys

int CCutsceneEditor::IcShotOf(int icEnd) const
{
    if (icEnd < 1 || icEnd >= int(m_acmd.size()) || m_acmd[std::size_t(icEnd)].kind != 0x39) return -1;
    for (int k = icEnd - 1; k >= 1; --k)
    {
        const CSCMD& q = m_acmd[std::size_t(k)];
        if (q.kind == 0x38 && q.icBlock == m_acmd[std::size_t(icEnd)].icBlock) return k;
        if (q.kind == 0x39 && q.icBlock == m_acmd[std::size_t(icEnd)].icBlock) return -1;
    }
    return -1;
}

// the easing curves offered (the game's 0, 1, 3 and the PC's ease out), with the curve drawn
bool CCutsceneEditor::FEaseCombo(const char* id, int& ease)
{
    static const int s_a[] = {0, 1, cut::ICURVE_EASEOUT, 3};
    bool f = false;
    if (ImGui::BeginCombo(id, T(cut::SzEase(ease))))
    {
        for (int k : s_a)
            if (ImGui::Selectable(T(cut::SzEase(k)), k == ease)) { ease = k; f = true; }
        ImGui::EndCombo();
    }
    // (the curve: the share of the move done over its time)
    ImGui::SameLine();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float w = 44.0f, h = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(o, ImVec2(o.x + w, o.y + h), st::u32(st::border));
    ImVec2 prev(o.x, o.y + h);
    for (int i = 1; i <= 16; ++i)
    {
        const float u = float(i) / 16.0f, v = std::clamp(cut::SEase(ease, u), -0.2f, 1.2f);
        const ImVec2 p(o.x + u * w, o.y + h - v * h);
        dl->AddLine(prev, p, st::u32(st::accent), 1.5f);
        prev = p;
    }
    ImGui::Dummy(ImVec2(w, h));
    return f;
}

// A camera key at t from the view: a track of its own (it holds no other) that waits until t - blend, then moves the
// camera from where it is to the view over blend seconds (eased) and leaves it there (not released: the scene's next
// shot or release takes it)
bool CCutsceneEditor::FNewCameraKey(float t, float blend, int ease, std::string& error)
{
    if (!m_decodeNote.empty()) { error = m_decodeNote; return false; }
    blend = std::clamp(std::round(blend * 60.0f) / 60.0f, c_dtFrame, 30.0f);
    const float tStart = std::max(0.0f, t - blend);
    blend = t - tStart > c_dtFrame ? t - tStart : blend;
    const int iBefore = InsertPoint(tStart);
    const float tWait = std::max(0.0f, tStart - TFreeAt(iBefore));
    const CAMERA& cam = m_fLookActive ? m_camSaved : e.m_cam;
    glm::mat3 m;
    if (cam.fMat) m = cam.mat;
    else
    {
        const glm::vec3 d = cam.Dir();
        const glm::vec3 left = glm::normalize(glm::cross(glm::vec3(0, 0, 1), d));
        m = glm::mat3(d, left, glm::cross(d, left));
    }
    const glm::vec3 eul = cmm::DecomposeEulerMatrix(m);
    char sz[64];
    std::snprintf(sz, sizeof sz, "%.2f", t);
    const std::string what = std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("a camera key at") + " " + sz + " s";
    int i = iBefore;
    bool fOk = FInsertCommand(i++, 0x00, 0, {{0x10, -1}}, what);
    if (fOk && tWait > 0.02f) { Decode(); fOk = FInsertCommand(i++, 0x23, 0, {{0x10, std::round(tWait * 60.0f) / 60.0f}, {0x14, -1}, {0x18, -1}}, what); }
    // (the move's start 0 0 0: the camera as it is then)
    if (fOk) { Decode(); fOk = FInsertCommand(i++, 0x38, 0, {{0x10, 0}, {0x14, 0}, {0x18, 0}, {0x20, 0}, {0x24, 0}, {0x28, 0}, {0x30, 0}, {0x40, blend}, {0x44, ease}}, what); }
    if (fOk)
    {
        Decode();
        fOk = FInsertCommand(i++, 0x39, 0, {{0x10, cam.pos.x}, {0x14, cam.pos.y}, {0x18, cam.pos.z}, {0x20, eul.x}, {0x24, eul.y}, {0x28, eul.z}, {0x30, glm::radians(cam.fovY)}, {0x40, 0}, {0x44, 0}}, what);
    }
    if (fOk) { Decode(); fOk = FInsertCommand(i++, 0x01, 0, {}, what); }
    if (!fOk) { error = T("the commands could not be written"); return false; }
    Decode();
    m_isel = i - 2;   // (its end: the key)
    return true;
}

// ---- when it plays, what a skip does (the package's rule, platform/mod_package.h CutsceneRule)

cane::mods::CutsceneRule CCutsceneEditor::RuleOf() const
{
    for (const cane::mods::CutsceneRule& r : cane::mods::cutsceneRules(e.m_proj.manifest))
        if (r.world == e.m_index.entry.name && r.script == m_oid) return r;
    cane::mods::CutsceneRule r;
    r.world = e.m_index.entry.name;
    r.script = m_oid;
    return r;
}

void CCutsceneEditor::SetRule(const cane::mods::CutsceneRule& r)
{
    cane::mods::setCutsceneRule(e.m_proj.manifest, r);
    e.Status(std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("when it plays / its skip set (saved with the project)"));
}

void CCutsceneEditor::DrawRulePanel()
{
    ImGui::SeparatorText(T("When it plays"));
    cane::mods::CutsceneRule r = RuleOf();
    bool fChanged = false;
    fChanged |= ImGui::Checkbox(T("only the first time"), &r.once);
    EdTip(T("The script starts only the first time in a save (kept next to the save slot); later starts are left out"));
    bool fTask = r.task >= 0;
    if (ImGui::Checkbox(T("only while a task is"), &fTask)) { r.task = fTask ? 0 : -1; if (fTask && r.states.empty()) r.states = {1}; fChanged = true; }
    if (fTask)
    {
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("##rtask", &r.task, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) fChanged = true;
        ImGui::SameLine();
        if (ImGui::SmallButton(TL("selection", "rt").c_str()) && e.m_plSel) { r.task = e.m_plSel->oid; fChanged = true; }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", PloOfOid(r.task) ? NameOf(r.task).c_str() : T("(not in this world)"));
        for (int n = 0; n <= 3; ++n)
        {
            bool f = std::find(r.states.begin(), r.states.end(), n) != r.states.end();
            if (n) ImGui::SameLine();
            if (ImGui::Checkbox((std::string(T(SzTaskState(n))) + "##rs" + std::to_string(n)).c_str(), &f))
            {
                r.states.erase(std::remove(r.states.begin(), r.states.end(), n), r.states.end());
                if (f) r.states.push_back(n);
                std::sort(r.states.begin(), r.states.end());
                fChanged = true;
            }
        }
    }
    ImGui::SeparatorText(T("Skip"));
    static const char* s_askip[] = {"auto", "always", "never"};
    int iskip = r.skip == "always" ? 1 : r.skip == "never" ? 2 : 0;
    const char* askipName[] = {T("as the game decides (scenes with lines or 3+ shots)"), T("always skippable"), T("never skippable")};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##rskip", &iskip, askipName, 3)) { r.skip = s_askip[iskip]; fChanged = true; }
    EdTip(T("Whether holding the skip button skips this scene. A skip plays the rest fast to its end (the tasks, places and objects end as when watched)"));
    // (the labels of the script: where a skip may stop)
    std::vector<int> alabel;
    for (const CSCMD& c : m_acmd)
        if (c.kind == 0x13) alabel.push_back(c.an[0]);
    std::string cur = r.skipToLabel < 0 ? std::string(T("the scene's end")) : std::string(T("label")) + " " + std::to_string(r.skipToLabel);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("a skip runs to"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##rlabel", cur.c_str()))
    {
        if (ImGui::Selectable(T("the scene's end"), r.skipToLabel < 0)) { r.skipToLabel = -1; fChanged = true; }
        for (int l : alabel)
            if (ImGui::Selectable((std::string(T("label")) + " " + std::to_string(l)).c_str(), l == r.skipToLabel)) { r.skipToLabel = l; fChanged = true; }
        ImGui::EndCombo();
    }
    EdTip(T("A label: the skip stops where the script reaches it and the rest plays (add a Label command there first)"));
    if (fChanged) SetRule(r);
}

// An event at a time: a track of its own that waits until t, then runs the command (a task's state, an object shown /
// hidden, a sound, the script's own splice event ...)
bool CCutsceneEditor::FAddAtTime(float t, int kind, int variant, const std::vector<std::pair<int, double>>& aset, const std::string& label)
{
    if (!m_decodeNote.empty()) { e.Status(m_decodeNote); return false; }
    const int iBefore = InsertPoint(t);
    const float tWait = std::max(0.0f, t - TFreeAt(iBefore));
    int i = iBefore;
    bool fOk = FInsertCommand(i++, 0x00, 0, {{0x10, -1}}, label);
    if (fOk && tWait > 0.02f) { Decode(); fOk = FInsertCommand(i++, 0x23, 0, {{0x10, std::round(tWait * 60.0f) / 60.0f}, {0x14, -1}, {0x18, -1}}, label); }
    if (fOk) { Decode(); fOk = FInsertCommand(i++, kind, variant, aset, label); }
    if (fOk) { Decode(); fOk = FInsertCommand(i++, 0x01, 0, {}, label); }
    Decode();
    if (fOk) m_isel = i - 2;
    return fOk;
}

void CCutsceneEditor::DrawEventPanel()
{
    ImGui::SeparatorText(T("Event at the playhead"));
    // (kind, its value's name)
    struct EV { int kind; const char* name; const char* value; bool fObject; };
    static const EV s_aev[] = {{0x11, "Set task state", "state", true}, {0x2C, "Show object", nullptr, true}, {0x2D, "Hide object", nullptr, true},
                               {0x36, "Sound", "sound id", false}, {0x09, "Script event (splice)", "value", false}, {0x4C, "Music", "music", false},
                               {0x44, "Title card", nullptr, false}, {0x33, "Rumble", nullptr, false}, {0x32, "Camera shake", "kind", false}};
    const int cev = int(sizeof s_aev / sizeof s_aev[0]);
    m_eventKind = std::clamp(m_eventKind, 0, cev - 1);
    const EV& ev = s_aev[m_eventKind];
    ImGui::SetNextItemWidth(170);
    if (ImGui::BeginCombo("##evk", T(ev.name)))
    {
        for (int k = 0; k < cev; ++k)
            if (ImGui::Selectable(T(s_aev[k].name), k == m_eventKind)) m_eventKind = k;
        ImGui::EndCombo();
    }
    if (ev.fObject)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("##evo", &m_eventObject, 0, 0);
        ImGui::SameLine();
        if (ImGui::SmallButton(TL("selection", "evo").c_str()) && e.m_plSel) m_eventObject = e.m_plSel->oid;
        EdTip(T("The task / object (its id; selection: the one selected in the 3D view)"));
    }
    if (ev.value)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt((std::string("##evv") + ev.value).c_str(), &m_eventValue, 0, 0);
        EdTip(T(ev.value));
    }
    if (ImGui::Button(T("Add the event at the playhead")))
    {
        std::vector<std::pair<int, double>> a;
        switch (ev.kind)
        {
        case 0x11: a = {{0x10, m_eventObject}, {0x14, m_eventValue}}; break;
        case 0x2C: case 0x2D: a = {{0x10, m_eventObject}, {0x14, 0.0}, {-2, 1}}; break;
        case 0x36: a = {{0x10, m_eventValue}, {0x14, -1}, {0x18, 1000.0}, {0x1C, 3000.0}, {0x20, 1.0}, {0x24, 0.0}, {0x28, 1.0}}; break;
        case 0x33: a = {{0x10, 0.5}, {0x14, 0}, {-2, 1}}; break;
        case 0x32: a = {{0x10, m_eventValue}, {-2, 1}}; break;
        default: a = {{0x10, m_eventValue}}; break;
        }
        char sz[32];
        std::snprintf(sz, sizeof sz, "%.2f", m_t);
        FAddAtTime(m_t, ev.kind, 0, a, std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T(ev.name) + " " + T("at") + " " + sz + " s");
    }
    EdTip(T("A track of its own waits until the playhead's time, then runs it (the scene's own tracks are not held)"));
}

// The starter: the scene started from a task's own script when the task becomes a state, or from another object's
// script (a volume / trigger entered, an object used, a job's start / end, another script's event, a new button
// press zone), written in the readable (named) script format and checked first
namespace
{
const char* SzStarterKind(int k)
{
    static const char* s_a[] = {"a task's state", "Sly enters a volume / trigger", "an object is used", "a job starts", "a job ends",
                                "an event from another script", "a button press zone (new)"};
    return k >= 0 && k < 7 ? s_a[k] : "?";
}
}  // namespace

// kind: 0 task (oid, value: its state), 1 volume / trigger, 2 object used, 3 job start, 4 job end, 5 another script's
// event (oid: the script, value: the event's value), 6 a new button press zone (a copy of the level's use spot at pos)
bool CCutsceneEditor::FWriteStarter(int kind, int oid, int value, const glm::vec3& pos, std::vector<std::string>& adiag, std::string& error)
{
    adiag.clear();
    const std::string name = "$cutscene-" + std::to_string(m_oid);
    const std::string call = "((<geto> (<find-object> " + name + " #obj:0) .trigger) #obj:0)";
    LO* plo = PloOfOid(oid);
    switch (kind)
    {
    case 0:
        if (!plo || !FIsLoDerivedFrom(plo, EDCID(0x85))) { error = T("choose a task"); return false; }
        return e.FWriteSceneStarter(m_oid, plo, value, adiag, error);
    case 1:
        if (!plo || !(FIsLoDerivedFrom(plo, EDCID(0x05)) || FIsLoDerivedFrom(plo, EDCID(0x18)) || FIsLoDerivedFrom(plo, EDCID(0x16)))) { error = T("choose a volume, trigger volume or shape"); return false; }
        return e.FWriteObjectHandler(plo, "on-activate", {"self"}, call, {{name, m_oid}}, adiag, error, true);
    case 2:
        if (!plo) { error = T("choose the object that is used"); return false; }
        return e.FWriteObjectHandler(plo, "on-activate", {"self"}, call, {{name, m_oid}}, adiag, error, true);
    case 3: case 4:
    {
        if (!plo || !FIsLoDerivedFrom(plo, EDCID(0x84))) { error = T("choose a job"); return false; }
        int oidTask = -1;
        if (!e.FOptionInt(plo, EDEOPID(kind == 3 ? 1243 : 1244), oidTask) || !PloOfOid(oidTask)) { error = T("the job's first / last task was not found"); return false; }
        if (kind == 3) return e.FWriteObjectHandler(PloOfOid(oidTask), "on-task-start", {"self"}, call, {{name, m_oid}}, adiag, error, true);
        return e.FWriteObjectHandler(PloOfOid(oidTask), "on-task-state", {"self"}, "(if (<=> (<geto> %self .state) 2) " + call + ")", {{name, m_oid}}, adiag, error, true);
    }
    case 5:
        if (!plo || !FIsLoDerivedFrom(plo, EDCID(0x72)) || plo == m_pscpt) { error = T("choose another script"); return false; }
        return e.FWriteObjectHandler(plo, "on-event", {"self", "event"}, "(if (<=> %1 " + std::to_string(value) + ") " + call + ")", {{name, m_oid}}, adiag, error, true);
    case 6:
    {
        int iobj = -1;
        for (std::size_t i = 0; i < e.m_rec.aobj.size() && iobj < 0; ++i)
            if ((e.m_rec.aobj[i].cid == EDCID(0x23) || e.m_rec.aobj[i].cid == EDCID(0x24)) && e.m_rec.aobj[i].plo && e.m_rec.aobj[i].depth == 0) iobj = int(i);
        if (iobj < 0) { error = T("this level has no use spot to copy for a button press zone"); return false; }
        const int oidZone = e.CopyRecord(iobj, e.CopyGroup(), &pos, false, error);
        if (!oidZone) return false;
        Decode();
        if (!e.FWriteObjectHandler(PloOfOid(oidZone), "on-activate", {"self"}, call, {{name, m_oid}}, adiag, error, true)) return false;
        adiag.push_back(std::string(T("the button press zone:")) + " " + NameOf(oidZone));
        return true;
    }
    default: error = "?"; return false;
    }
}

void CCutsceneEditor::DrawStarterPanel()
{
    ImGui::SeparatorText(T("Start it from"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##stkind", T(SzStarterKind(m_starterKind))))
    {
        for (int k = 0; k < 7; ++k)
            if (ImGui::Selectable(T(SzStarterKind(k)), k == m_starterKind)) m_starterKind = k;
        ImGui::EndCombo();
    }
    LO* pt = nullptr;
    if (m_starterKind != 6)
    {
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##sttask", &m_oidStarterTask, 0, 0);
        ImGui::SameLine();
        if (ImGui::SmallButton(TL("selection", "st").c_str()) && e.m_plSel) m_oidStarterTask = e.m_plSel->oid;
        EdTip(T("The task / volume / object / job / script (its id; selection: the one selected in the 3D view)"));
        ImGui::SameLine();
        pt = PloOfOid(m_oidStarterTask);
        ImGui::TextDisabled("%s", pt ? NameOf(m_oidStarterTask).c_str() : T("(choose one)"));
    }
    if (m_starterKind == 0)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T("when it becomes"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        if (ImGui::BeginCombo("##ststate", T(SzTaskState(m_starterState))))
        {
            for (int n = 0; n <= 3; ++n)
                if (ImGui::Selectable(T(SzTaskState(n)), n == m_starterState)) m_starterState = n;
            ImGui::EndCombo();
        }
    }
    if (m_starterKind == 5)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T("its event's value"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("##stev", &m_starterEvent, 0, 0);
        EdTip(T("The value of the other script's \"Script event\" command that starts this cutscene (add one to that script where it should start)"));
        if (pt && FIsLoDerivedFrom(pt, EDCID(0x72)))
        {
            std::string sent;
            int kind = 0, b = 0;
            int32_t an[18];
            for (int i = 0; FScptCommand(pt, i, kind, b, an); ++i)
                if (kind == 0x09) sent += (sent.empty() ? "" : ", ") + std::to_string(an[0]);
            ImGui::TextDisabled("%s %s", T("the events it sends:"), sent.empty() ? T("(none yet)") : sent.c_str());
        }
    }
    if (m_starterKind == 6) ImGui::TextWrapped("%s", T("A new use spot (the level's own, copied) where the view looks: Sly presses the button there and the cutscene starts. Move it with the gizmo after."));
    if (ImGui::Button(T("Write the starter script")))
    {
        std::string error;
        glm::vec3 pos = e.m_cam.pos + e.m_cam.Dir() * 400.0f;
        m_starterNote = FWriteStarter(m_starterKind, m_oidStarterTask, m_starterKind == 5 ? m_starterEvent : m_starterState, pos, m_astarterDiag, error)
                            ? std::string(T("written in its script (readable, with names)"))
                            : error;
        e.Status(m_starterNote);
        WhoStarts();
    }
    EdTip(T("Adds to the object's own script (a task's on-task-start, a volume's or a used object's on-activate, a script's on-event; a module of its own is made when it has none) the line that starts the cutscene, as the game's own scripts start theirs. Checked first: a script with errors is not written. See it in the Scripts tab."));
    if (!m_starterNote.empty()) ImGui::TextWrapped("%s", m_starterNote.c_str());
    for (const std::string& d : m_astarterDiag) ImGui::BulletText("%s", d.c_str());
}

glm::vec4 CCutsceneEditor::RcPicture(bool fFraction) const
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float top = ImGui::GetFrameHeight();
    // (the Timeline dock's place, Draw())
    const float wRight = std::clamp(size.x * 0.25f, 330.0f, 470.0f);
    const float x0 = 64, x1 = x0 + std::max(300.0f, size.x - wRight - 12 - x0 - 6);
    const float y0 = top + 4, y1 = size.y - 36 - std::max(260.0f, (size.y - top) * 0.42f) - 6;
    const float wFree = std::max(16.0f, x1 - x0), hFree = std::max(9.0f, y1 - y0);
    const float w = std::min(wFree, hFree * 16.0f / 9.0f), h = w * 9.0f / 16.0f;
    const glm::vec4 rc(x0 + (wFree - w) * 0.5f, y0 + (hFree - h) * 0.5f, w, h);
    if (!fFraction) return rc;
    return {rc.x / std::max(1.0f, size.x), rc.y / std::max(1.0f, size.y), rc.z / std::max(1.0f, size.x), rc.w / std::max(1.0f, size.y)};
}

// The playhead's subtitle, the letterbox while the scene has the game paused with its bars, the title card (over the 3D
// view, as the game shows them). Looking through: in the picture (the game's 16:9 frame in the view's free part, the
// rest of the view dark, as a video editor's program monitor); else over the view's free part.
void CCutsceneEditor::DrawPreviewOverlay()
{
    if (!m_oid) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float top = ImGui::GetFrameHeight();
    // the area: x0, y0 (its top), x1, y1 (its bottom: the timeline's top when not looking through)
    float x0 = 0, x1 = size.x, y0 = top, y1 = size.y - 36 - std::max(260.0f, (size.y - top) * 0.42f);
    if (m_fLookActive)
    {
        const glm::vec4 rc = RcPicture(false);
        x0 = rc.x, y0 = rc.y, x1 = rc.x + rc.z, y1 = rc.y + rc.w;
        // (around the picture: dark; the world spills past it otherwise)
        const ImU32 colDark = IM_COL32(6, 8, 12, 255);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(size.x, y0), colDark);
        dl->AddRectFilled(ImVec2(0, y1), ImVec2(size.x, size.y), colDark);
        dl->AddRectFilled(ImVec2(0, y0), ImVec2(x0, y1), colDark);
        dl->AddRectFilled(ImVec2(x1, y0), ImVec2(size.x, y1), colDark);
    }
    const float wA = x1 - x0, hA = y1 - y0, xMid = (x0 + x1) * 0.5f;
    // the letterbox: from a "Pause game" with the subtitle box and its bars (what & 1, ai.cpp PauseGame) to its end
    bool fBars = false, fTitle = false;
    for (const CSCMD& c : m_acmd)
    {
        if (c.kind == 0x12 && c.an[0] && (c.an[1] & 1) && c.t0 <= m_t + 1e-4f)
        {
            // (to its own "Pause game" off: of its track, LayoutPresentation)
            const float tOff = c.icPair >= 0 ? m_acmd[std::size_t(c.icPair)].t0 : m_tEnd + 1.0f;
            fBars = fBars || m_t < tOff;
        }
        if (c.kind == 0x44 && c.t0 <= m_t && m_t < c.t1) fTitle = true;
    }
    const bool fCustom = FCustomPresent();
    // (the game's cut-scene bars: about a seventh of the picture's height each)
    const bool fBarsShown = fBars && m_fLookActive;
    const float hBar = m_fLookActive ? hA * 0.142f : size.y * 0.1f;
    if (fBarsShown)
    {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + hBar), IM_COL32(0, 0, 0, 255));
        dl->AddRectFilled(ImVec2(x0, y1 - hBar), ImVec2(x1, y1), IM_COL32(0, 0, 0, 255));
    }
    // the fade at the playhead (as the game draws it: the whole frame in its colour)
    float rgbaFade[4];
    if (m_fLookActive && FFadeAt(m_t, rgbaFade))
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImGui::ColorConvertFloat4ToU32(ImVec4(rgbaFade[0], rgbaFade[1], rgbaFade[2], rgbaFade[3])));
    // a caption (the game's hint box, its HUD ticker at the top) at the playhead: the game shows it only while the HUD is
    // up (not while the scene has the game paused)
    bool fPaused = false;
    for (const CSCMD& c : m_acmd)
        if (c.kind == 0x12 && c.an[0] && c.t0 <= m_t + 1e-4f && (c.icPair < 0 || m_t < m_acmd[std::size_t(c.icPair)].t0)) fPaused = true;
    // (the custom presentation: the caption in the scene's pause too, at the bottom as the game's subtitle; a title
    // card of its own text, large in the middle)
    for (const CSCMD& c : m_acmd)
        if (fCustom && c.kind == 0x53 && c.t0 <= m_t && m_t < c.t1 && (fPaused || FTitleCardClip(c)))
        {
            const std::string text = e.StrText(c.an[0]);
            if (text.empty()) continue;
            const bool fCard = FTitleCardClip(c);
            const float fs = ImGui::GetFontSize() * (fCard ? 2.0f : 1.0f);
            const float wrap = std::min(wA * 0.8f, 900.0f);
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, wrap, text.c_str());
            const ImVec2 p(xMid - ts.x * 0.5f, fCard ? (y0 + y1) * 0.5f - ts.y * 0.5f - hA * 0.08f
                                                     : (fBarsShown ? y1 - hBar * 0.5f - ts.y * 0.5f : y1 - ts.y - 18.0f));
            if (fCard)
            {
                dl->AddRectFilled(ImVec2(x0, p.y - 14), ImVec2(x1, p.y + ts.y + 14), IM_COL32(0, 0, 0, 150));
                dl->AddRectFilled(ImVec2(x0, p.y - 16), ImVec2(x1, p.y - 14), IM_COL32(230, 180, 60, 200));
                dl->AddRectFilled(ImVec2(x0, p.y + ts.y + 14), ImVec2(x1, p.y + ts.y + 16), IM_COL32(230, 180, 60, 200));
            }
            else if (!fBarsShown) dl->AddRectFilled(ImVec2(p.x - 10, p.y - 6), ImVec2(p.x + ts.x + 10, p.y + ts.y + 6), IM_COL32(0, 0, 0, 160), 6.0f);
            dl->AddText(nullptr, fs, p, fCard ? IM_COL32(250, 215, 120, 255) : IM_COL32(255, 255, 255, 255), text.c_str(), nullptr, wrap);
        }
    for (const CSCMD& c : m_acmd)
        if (c.kind == 0x53 && c.t0 <= m_t && m_t < c.t1 && !fPaused && !(fCustom && FTitleCardClip(c)))
        {
            const std::string text = e.StrText(c.an[0]);
            if (text.empty()) continue;
            const float wrap = std::min(wA * 0.6f, 700.0f);
            const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
            const ImVec2 p(xMid - ts.x * 0.5f, y0 + hA * 0.08f);
            dl->AddRectFilled(ImVec2(p.x - 12, p.y - 8), ImVec2(p.x + ts.x + 12, p.y + ts.y + 8), IM_COL32(20, 30, 60, 210), 8.0f);
            dl->AddRect(ImVec2(p.x - 12, p.y - 8), ImVec2(p.x + ts.x + 12, p.y + ts.y + 8), IM_COL32(240, 200, 80, 255), 8.0f, 0, 2.0f);
            dl->AddText(nullptr, 0.0f, p, IM_COL32(255, 255, 255, 255), text.c_str(), nullptr, wrap);
        }
    if (fTitle)
    {
        const char* sz = T("(title card)");
        const ImVec2 ts = ImGui::CalcTextSize(sz);
        dl->AddText(nullptr, ImGui::GetFontSize() * 2.0f, ImVec2(xMid - ts.x, (y0 + y1) * 0.5f - ts.y), st::u32(st::text), sz);
    }
    // the subtitle: the line playing at the playhead (as measured, else its clip); in the bottom bar as the game's
    int idLine = -1;
    for (const auto& l : m_trace.aline)
        if (l.t0 <= m_t && m_t < l.t1) idLine = l.id;
    if (idLine < 0)
        for (const CSCMD& c : m_acmd)
            if (c.kind == 0x45 && c.t0 <= m_t && m_t < c.t1) idLine = c.an[0];
    if (idLine >= 0)
    {
        const std::string text = e.StrText(idLine);
        if (!text.empty())
        {
            const float wrap = std::min(wA * 0.8f, 900.0f);
            const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
            const ImVec2 p(xMid - ts.x * 0.5f, fBarsShown ? y1 - hBar * 0.5f - ts.y * 0.5f : y1 - ts.y - 18);
            if (!fBarsShown) dl->AddRectFilled(ImVec2(p.x - 10, p.y - 6), ImVec2(p.x + ts.x + 10, p.y + ts.y + 6), IM_COL32(0, 0, 0, 160), 6.0f);
            dl->AddText(nullptr, 0.0f, p, IM_COL32(255, 255, 255, 255), text.c_str(), nullptr, wrap);
        }
    }
}

// ---- cinematic animations: their channels and keys

ASEG* CCutsceneEditor::PasegOfClip(const CSCMD& c) const
{
    if (c.kind != 0x0A && c.kind != 0x22 && c.kind != 0x4B) return nullptr;
    const int id = c.an[0];
    LO* pa = c.oidActor >= 0 ? PloOfOid(c.oidActor) : nullptr;
    LO* pfound = nullptr;
    std::function<void(LO*)> find = [&](LO* p) {
        if (pfound) return;
        if (p != pa && FIsLoDerivedFrom(p, EDCID(0x6D)) && p->oid == id) { pfound = p; return; }
        if (const auto it = e.m_mpchildren.find(p); it != e.m_mpchildren.end())
            for (LO* q : it->second) find(q);
    };
    if (pa) find(pa);
    if (!pfound) pfound = PloOfOid(id);
    return pfound && FIsLoDerivedFrom(pfound, EDCID(0x6D)) ? static_cast<ASEG*>(pfound) : nullptr;
}

// An animation's channels (the objects / records it moves) with their keys' times, and its timed events
std::vector<CCutsceneEditor::SUBROW> CCutsceneEditor::SubrowsOf(const CSCMD& c) const
{
    std::vector<SUBROW> a;
    ASEG* paseg = PasegOfClip(c);
    const ASEGD* pd = paseg ? PasegdOf(paseg) : nullptr;
    if (!pd) return a;
    // (the channels by their object: one row per object, its records' keys together)
    std::map<int, SUBROW> mp;
    std::vector<int> aorder;
    for (const ASEGCHN& chn : pd->achn)
    {
        auto [it, fNew] = mp.try_emplace(chn.oid);
        if (fNew)
        {
            aorder.push_back(chn.oid);
            it->second.oid = chn.oid;
            it->second.name = chn.oid == -1 || chn.oid == c.oidActor ? (c.oidActor >= 0 ? NameOf(c.oidActor) : std::string(T("its object"))) : NameOf(chn.oid);
        }
        std::vector<float>& at = it->second.at;
        if (const auto* pk = dynamic_cast<const TRKPK*>(chn.ptrkp.get()))
            for (const KEYP& k : pk->akey) at.push_back(k.t);
        if (const auto* pa = dynamic_cast<const TRKPA*>(chn.ptrkp.get())) at.insert(at.end(), pa->at.begin(), pa->at.end());
        if (const auto* pg = dynamic_cast<const TRKGK*>(chn.ptrkg.get()))
            for (const KEYG& k : pg->akey) at.push_back(k.t);
    }
    for (int oid : aorder)
    {
        SUBROW& r = mp[oid];
        std::sort(r.at.begin(), r.at.end());
        r.at.erase(std::unique(r.at.begin(), r.at.end(), [](float x, float y) { return std::fabs(x - y) < 1e-3f; }), r.at.end());
        a.push_back(std::move(r));
    }
    if (!pd->aaev[1].empty())
    {
        SUBROW r;
        r.name = T("events (sounds, effects ...)");
        r.fEvents = true;
        for (const ASEGEV& ev : pd->aaev[1]) r.at.push_back(ev.t);
        a.push_back(std::move(r));
    }
    return a;
}

#include "editor_cutclip.inc"
#include "editor_cutpresent.inc"

// ---- the dock

void CCutsceneEditor::DrawToolbar()
{
    ImGui::SetNextItemWidth(260);
    std::string cur = m_oid ? NameOf(m_oid) : std::string(T("Choose a cutscene"));
    for (const CSSCENE& s : m_ascene)
        if (s.oid == m_oid) cur = s.name;
    if (ImGui::BeginCombo("##scene", cur.c_str(), ImGuiComboFlags_HeightLarge))
    {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##f", T("search"), m_filter, sizeof m_filter);
        ImGui::Checkbox(T("Scenes only (lines, shots or the game paused)"), &m_fScenesOnly);
        for (const CSSCENE& s : m_ascene)
        {
            if (m_fScenesOnly && !s.cLine && !s.cShot && !s.cPause) continue;
            if (*m_filter && s.name.find(m_filter) == std::string::npos) continue;
            char sz[320];
            std::snprintf(sz, sizeof sz, "%s##%d", s.name.c_str(), s.oid);
            if (ImGui::Selectable(sz, s.oid == m_oid)) Open(s.oid);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%d %s, %d %s, %d %s%s", s.cCmd, T("commands"), s.cLine, T("lines"), s.cShot, T("camera shots"), s.cPause ? T(", pauses the game") : "");
        }
        ImGui::EndCombo();
    }
    EdTip(T("The world's scripts that play scenes (the game paused, camera shots, dialogue)"));
    ImGui::SameLine();
    if (ImGui::Button(T("New"))) { std::string error; if (!CreateCutscene(error)) e.Status(error); }
    EdTip(T("A new cutscene: the game paused, a camera shot from the view, the camera given back. Then add to it."));
    if (!m_oid) return;
    ImGui::SameLine();
    ImGui::BeginDisabled(m_fTracePending);
    if (ImGui::Button(T("Measure"))) StartTrace();
    ImGui::EndDisabled();
    EdTip(T("Plays the cutscene in a hidden game (the saved project) and measures each command's time, the camera and what moves"));
    ImGui::SameLine();
    if (ImGui::Button(m_fPlaying ? T("Pause") : T("Play"))) { m_fPlaying = !m_fPlaying; if (m_fPlaying && m_t >= m_tEnd - 1e-3f) m_t = 0.0f; }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) { m_fPlaying = false; m_t = std::max(0.0f, m_t - c_dtFrame); }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) { m_fPlaying = false; m_t = std::min(m_tEnd, m_t + c_dtFrame); }
    ImGui::SameLine();
    ImGui::Checkbox(T("Loop"), &m_fLoop);
    EdTip(T("Plays the range again and again (shift + drag on the ruler sets the range)"));
    ImGui::SameLine();
    ImGui::Text("%6.2f / %.2f s", m_t, m_tEnd);
    ImGui::SameLine();
    if (ImGui::Button(T("Play in game"))) PlayInGame(m_t);
    EdTip(T("The game plays the cutscene from the playhead (the project saved first)"));
    // (the second row: what is shown, how it is measured)
    ImGui::Checkbox(T("Camera"), &m_fLookThrough);
    EdTip(T("The view looks through the cutscene's camera at the playhead"));
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Scene"), &m_fPreview)) m_tApplied = -1.0f;
    EdTip(T("The objects stand where they are at the playhead (as measured)"));
    ImGui::SameLine();
    ImGui::Checkbox(T("Ghosts"), &m_fGhosts);
    EdTip(T("The characters' paths, a dot each half second; their poses half a second and a second before (blue) and after (orange) the playhead"));
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Poses"), &m_fPoses)) { RestoreScene(); m_tApplied = -1.0f; }
    EdTip(T("The characters posed by the animation playing on them at the playhead, as the game poses them (off: their rest pose)"));
    ImGui::SameLine();
    if (ImGui::Checkbox(T("By script track"), &m_fByRun)) BuildTracks();
    EdTip(T("Off: a track per kind and per character; on: the script's own tracks (its blocks, which run side by side)"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    const char* amode[] = {T("Measure it alone"), T("Measure from its starter")};
    if (ImGui::Combo("##mmode", &m_measureMode, amode, 2)) StartTrace();
    EdTip(T("Alone: the script started in the world as it is (quick). From its starter: the script that starts it plays first (the characters placed as the game places them; it may wait for gameplay)"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt(T("at task"), &m_taskMeasure, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) StartTrace();
    EdTip(T("The world started at this task (a job's start: its objects and characters there; 0: the world's start)"));
    if (!m_traceNote.empty() || !m_decodeNote.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(st::vec(m_decodeNote.empty() ? st::muted : st::danger), "%s", (m_decodeNote.empty() ? m_traceNote : m_decodeNote).c_str());
    }
}

void CCutsceneEditor::DrawTracks(float w, float h)
{
    const float wHead = 190.0f, hRuler = 22.0f, hRow = 24.0f;
    // (the wheel scrolls the tracks; ctrl + wheel zooms the time, shift + wheel scrolls it)
    ImGui::BeginChild("##tracks", ImVec2(w, h), false, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift ? ImGuiWindowFlags_NoScrollWithMouse : 0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float wTime = std::max(50.0f, w - wHead - 8.0f);
    if (m_fFitPending)
    {
        // (a few pixels before 0: the instant commands at the start show whole)
        m_pps = std::max(4.0f, wTime / std::max(1.0f, m_tEnd * 1.04f));
        m_tView = -10.0f / m_pps;
        m_fFitPending = false;
    }
    auto xOf = [&](float t) { return o.x + wHead + (t - m_tView) * m_pps; };
    auto tOf = [&](float x) { return m_tView + (x - o.x - wHead) / m_pps; };
    ImGuiIO& io = ImGui::GetIO();
    const bool fHover = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    // zoom (ctrl + wheel) about the mouse, scroll (wheel / shift + wheel)
    if (fHover && io.MouseWheel != 0.0f)
    {
        if (io.KeyCtrl)
        {
            const float tm = tOf(io.MousePos.x);
            m_pps = std::clamp(m_pps * (io.MouseWheel > 0 ? 1.25f : 0.8f), 2.0f, 2000.0f);
            m_tView = tm - (io.MousePos.x - o.x - wHead) / m_pps;
        }
        else if (io.KeyShift) m_tView -= io.MouseWheel * 40.0f / m_pps;
        m_tView = std::clamp(m_tView, -1.0f, m_tEnd);
    }
    // ruler
    dl->AddRectFilled(ImVec2(o.x, o.y), ImVec2(o.x + w, o.y + hRuler), st::u32(st::raised));
    // (the ticks at least ~56 pixels apart: their labels do not run into each other)
    float step = 60.0f;
    for (float st : {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f})
        if (st * m_pps >= 56.0f) { step = st; break; }
    for (float t = std::floor(std::max(0.0f, m_tView) / step) * step; xOf(t) < o.x + w; t += step)
    {
        const float x = xOf(t);
        if (x < o.x + wHead) continue;
        dl->AddLine(ImVec2(x, o.y + hRuler - 6), ImVec2(x, o.y + hRuler), st::u32(st::muted));
        char sz[16];
        std::snprintf(sz, sizeof sz, step < 0.25f ? "%.1f" : step < 1 ? "%.2f" : "%.0f", t);
        dl->AddText(ImVec2(x + 2, o.y + 3), st::u32(st::text2), sz);
    }
    // the loop range
    if (m_tLoop1 > m_tLoop0)
        dl->AddRectFilled(ImVec2(std::max(o.x + wHead, xOf(m_tLoop0)), o.y), ImVec2(xOf(m_tLoop1), o.y + hRuler), st::u32(st::success, m_fLoop ? 0.35f : 0.15f));
    // the ruler scrubs (shift: the loop range)
    ImGui::SetCursorScreenPos(ImVec2(o.x + wHead, o.y));
    ImGui::InvisibleButton("##ruler", ImVec2(wTime, hRuler));
    if (ImGui::IsItemActivated()) { m_dragKind = io.KeyShift ? 4 : 3; m_dragT0 = std::max(0.0f, tOf(io.MousePos.x)); m_fPlaying = false; }
    if (ImGui::IsItemActive())
    {
        const float t = std::clamp(tOf(io.MousePos.x), 0.0f, m_tEnd);
        if (m_dragKind == 4) { m_tLoop0 = std::min(m_dragT0, t); m_tLoop1 = std::max(m_dragT0, t); m_fLoop = true; }
        else m_t = t;
    }
    // the rows
    float y = o.y + hRuler + 2;
    std::vector<std::pair<ImVec2, ImVec2>> aclip(m_acmd.size(), {ImVec2(0, 0), ImVec2(0, 0)});
    static const uint32_t s_argb[] = {0x6C80A3, 0x3AA3F0, 0xF4C531, 0x3CC66B, 0xB57CF0, 0xF08A4B, 0xF0506E, 0xD9D9D9};
    ImGui::PushClipRect(ImVec2(o.x, o.y), ImVec2(o.x + w, o.y + h), true);
    // (the selected animation's channels: sub-rows under its track, a diamond per key)
    std::vector<SUBROW> asub;
    bool fSubHot = false;
    if (m_isel > 0 && m_isel < int(m_acmd.size())) asub = SubrowsOf(m_acmd[std::size_t(m_isel)]);
    if (asub.size() > 14) { const std::size_t c = asub.size() - 13; asub.resize(13); asub.push_back({std::string("+") + std::to_string(c) + " " + T("more channels"), {}, false}); }
    for (std::size_t it = 0; it < m_atrack.size(); ++it)
    {
        const CSTRACK& tr = m_atrack[it];
        const float hTrack = hRow * float(tr.clane);
        const bool fSub = !asub.empty() && m_acmd[std::size_t(m_isel)].itrack == int(it);
        const float hSub = fSub ? hRow * 0.8f * float(asub.size()) : 0.0f;
        const uint32_t rgb = s_argb[std::size_t(m_fByRun ? 0 : tr.tk)];
        dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + w, y + hTrack - 2), st::u32(it % 2 ? st::surface : st::input));
        dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + 4, y + hTrack - 2), st::u32(rgb));
        dl->AddText(ImVec2(o.x + 8, y + 4), st::u32(st::text), Utf8Cut(tr.name, 26).c_str());
        if (tr.oid >= 0)
        {
            ImGui::SetCursorScreenPos(ImVec2(o.x, y));
            ImGui::InvisibleButton(("##th" + std::to_string(it)).c_str(), ImVec2(wHead - 4, hRow - 2));
            if (ImGui::IsItemClicked())
                if (LO* p = PloOfOid(tr.oid)) { e.SelectOnly(p); e.FrameSelected(); }
            EdTip(T("Click: select the character in the 3D view"));
        }
        ImGui::PushClipRect(ImVec2(o.x + wHead, y), ImVec2(o.x + w, y + hTrack), true);
        const float yTrack = y;
        for (int ic : tr.aicmd)
        {
            const CSCMD& c = m_acmd[std::size_t(ic)];
            const float y = yTrack + hRow * float(c.lane);
            float t0 = c.t0, t1 = c.t1;
            if (int(ic) == m_isel && m_dragKind == 1) t0 += m_dragDt, t1 += m_dragDt;
            if (int(ic) == m_isel && m_dragKind == 2) t1 = std::max(t0, t1 + m_dragDt);
            const float x0 = xOf(t0), x1 = std::max(xOf(t1), x0 + 1.0f);
            const bool fSel = int(ic) == m_isel;
            const float a = c.fRan || !m_trace.fOk ? 1.0f : 0.4f;
            if (x1 - x0 < 6.0f)
            {
                // an instant command: a diamond
                const float xm = x0, ym = y + hRow * 0.5f - 1;
                dl->AddQuadFilled(ImVec2(xm, ym - 7), ImVec2(xm + 6, ym), ImVec2(xm, ym + 7), ImVec2(xm - 6, ym), st::u32(rgb, a));
                if (fSel) dl->AddQuad(ImVec2(xm, ym - 8), ImVec2(xm + 7, ym), ImVec2(xm, ym + 8), ImVec2(xm - 7, ym), st::u32(st::text), 2.0f);
                aclip[std::size_t(ic)] = {ImVec2(xm - 7, y), ImVec2(xm + 7, y + hRow - 2)};
            }
            else
            {
                dl->AddRectFilled(ImVec2(x0, y + 2), ImVec2(x1, y + hRow - 4), st::u32(rgb, 0.55f * a), 4.0f);
                if (c.tHold > 0.0f && c.tHold < t1 - t0 - 1e-3f)   // (the part the track waits for)
                    dl->AddRectFilled(ImVec2(x0, y + hRow - 7), ImVec2(xOf(t0 + c.tHold), y + hRow - 4), st::u32(rgb, a));
                dl->AddRect(ImVec2(x0, y + 2), ImVec2(x1, y + hRow - 4), fSel ? st::u32(st::text) : st::u32(rgb, a), 4.0f, 0, fSel ? 2.0f : 1.0f);
                if (c.fEstimate) dl->AddText(ImVec2(x1 - 10, y + 3), st::u32(st::text2), "~");
                const std::string l = Label(c);
                dl->PushClipRect(ImVec2(x0 + 3, y), ImVec2(x1 - 3, y + hRow), true);
                dl->AddText(ImVec2(x0 + 5, y + 4), st::u32(st::text), l.c_str());
                dl->PopClipRect();
                aclip[std::size_t(ic)] = {ImVec2(x0, y + 2), ImVec2(x1, y + hRow - 4)};
            }
        }
        ImGui::PopClipRect();
        if (fSub)
        {
            // the channels: their object, a diamond at each key (clip time from the clip's start); click: the key selected
            // and the playhead there; drag: the key moved (each of the row's channels' keys at that time); double-click
            // an empty spot: a key added there
            const CSCMD& c = m_acmd[std::size_t(m_isel)];
            float ys = y + hTrack;
            const float hs = hRow * 0.8f;
            for (std::size_t ir = 0; ir < asub.size(); ++ir)
            {
                const SUBROW& r = asub[ir];
                const bool fEditRow = r.oid != INT_MIN || r.fEvents;
                dl->AddRectFilled(ImVec2(o.x + 12, ys), ImVec2(o.x + w, ys + hs - 1), st::u32(st::bg, 0.6f));
                dl->AddText(ImVec2(o.x + 16, ys + 2), st::u32(r.fEvents ? st::cta : st::text2), Utf8Cut(r.name, 24).c_str());
                ImGui::PushClipRect(ImVec2(o.x + wHead, ys), ImVec2(o.x + w, ys + hs), true);
                const bool fRowHot = fHover && io.MousePos.y >= ys && io.MousePos.y < ys + hs && io.MousePos.x > o.x + wHead;
                bool fOnKey = false;
                for (float tk : r.at)
                {
                    const bool fDragged = m_dragKind == 5 && m_keyDragRow == int(ir) && std::fabs(tk - m_keyDragT0) < 1e-3f;
                    const float tShown = fDragged ? std::max(0.0f, tk + m_dragDt) : tk;
                    const float x = xOf(c.t0 + tShown), ym = ys + hs * 0.5f;
                    const bool fHot = std::fabs(io.MousePos.x - x) < 5 && std::fabs(io.MousePos.y - ym) < hs * 0.5f && fHover;
                    const bool fSelKey = m_keyRow == int(ir) && std::fabs(m_keyT - tk) < 1e-3f;
                    dl->AddQuadFilled(ImVec2(x, ym - 4), ImVec2(x + 4, ym), ImVec2(x, ym + 4), ImVec2(x - 4, ym), st::u32(r.fEvents ? st::cta : st::accent, fHot || fSelKey ? 1.0f : 0.75f));
                    if (fSelKey) dl->AddQuad(ImVec2(x, ym - 6), ImVec2(x + 6, ym), ImVec2(x, ym + 6), ImVec2(x - 6, ym), st::u32(st::text), 1.5f);
                    if (fHot && m_dragKind == 0)
                    {
                        fSubHot = fOnKey = true;
                        ImGui::SetTooltip("%s: %s %.2f s (%.2f s %s)%s", r.name.c_str(), r.fEvents ? T("event at") : T("key at"), tk, c.t0 + tk, T("in the scene"),
                                          fEditRow ? T("\ndrag: move it; Delete: remove it") : "");
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        {
                            m_t = std::clamp(c.t0 + tk, 0.0f, m_tEnd);
                            m_fPlaying = false;
                            m_keyRow = int(ir);
                            m_keyT = tk;
                            if (fEditRow) { m_dragKind = 5; m_keyDragRow = int(ir); m_keyDragT0 = tk; m_dragT0 = tOf(io.MousePos.x); m_dragDt = 0.0f; }
                        }
                    }
                }
                if (fRowHot && !fOnKey)
                {
                    fSubHot = true;
                    if (fEditRow && !r.fEvents && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        const float tc = std::max(0.0f, std::round((tOf(io.MousePos.x) - c.t0) * 60.0f) / 60.0f);
                        if (FEditClipKeys(c, r, tc, 0.0f, true)) { m_keyRow = int(ir); m_keyT = tc; }
                    }
                }
                ImGui::PopClipRect();
                ys += hs;
            }
        }
        y += hTrack + hSub;
    }
    // clicks and drags on the clips
    const bool fInRows = fHover && io.MousePos.y > o.y + hRuler && io.MousePos.x > o.x + wHead;
    int ihover = -1;
    for (std::size_t i = 0; i < aclip.size(); ++i)
        if (aclip[i].second.x > aclip[i].first.x && io.MousePos.x >= aclip[i].first.x && io.MousePos.x <= aclip[i].second.x &&
            io.MousePos.y >= aclip[i].first.y && io.MousePos.y <= aclip[i].second.y)
            ihover = int(i);
    if (ihover >= 0 && fInRows && m_dragKind == 0)
    {
        const CSCMD& c = m_acmd[std::size_t(ihover)];
        const cut::CMDDESC& d = cut::DescOfKind(c.kind);
        ImGui::SetTooltip("%s\n%s\n%.2f .. %.2f s%s", Label(c).c_str(), T(d.tip), c.t0, c.t1, c.fRan ? "" : m_trace.fOk ? T(" (not run in the measured play)") : T(" (estimated)"));
        const bool fEdge = (IbDuration(c) >= 0 || FPresentEdge(c)) && io.MousePos.x > aclip[std::size_t(ihover)].second.x - 6;
        if (fEdge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (m_isel != ihover) m_keyT = -1.0f;
            m_isel = ihover;
            m_dragKind = fEdge ? 2 : 1;
            m_dragT0 = tOf(io.MousePos.x);
            m_dragDt = 0.0f;
        }
    }
    else if (fInRows && !fSubHot && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_dragKind == 0) { m_isel = -1; m_t = std::clamp(tOf(io.MousePos.x), 0.0f, m_tEnd); }
    if (m_dragKind == 1 || m_dragKind == 2)
    {
        m_dragDt = std::round((tOf(io.MousePos.x) - m_dragT0) * 60.0f) / 60.0f;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const int i = m_isel, k = m_dragKind;
            const float dt = m_dragDt;
            m_dragKind = 0;
            m_dragDt = 0.0f;
            if (std::fabs(dt) >= c_dtFrame && i >= 0 && i < int(m_acmd.size()))
            {
                if (k == 1) FMoveClip(i, dt);
                else if (FPresentEdge(m_acmd[std::size_t(i)])) FResizePresentation(i, dt);
                else FResizeClip(i, (m_acmd[std::size_t(i)].t1 - m_acmd[std::size_t(i)].t0) + dt);
            }
        }
    }
    else if (m_dragKind == 5)
    {
        // (a key dragged on its row: moved by whole frames when let go)
        m_dragDt = std::round((tOf(io.MousePos.x) - m_dragT0) * 60.0f) / 60.0f;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const float dt = m_dragDt, t0 = m_keyDragT0;
            const int ir = m_keyDragRow;
            m_dragKind = 0;
            m_dragDt = 0.0f;
            m_keyDragRow = -1;
            if (std::fabs(dt) >= c_dtFrame && ir >= 0 && ir < int(asub.size()) && m_isel > 0 && m_isel < int(m_acmd.size()))
                if (FEditClipKeys(m_acmd[std::size_t(m_isel)], asub[std::size_t(ir)], t0, std::max(0.0f, t0 + dt), false)) m_keyT = std::max(0.0f, t0 + dt);
        }
    }
    else if (m_dragKind >= 3 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_dragKind = 0;
    // the playhead
    const float xp = xOf(m_t);
    if (xp >= o.x + wHead) dl->AddLine(ImVec2(xp, o.y), ImVec2(xp, o.y + h), st::u32(st::danger), 2.0f);
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(o.x, std::max(y, o.y + h - 1)));
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::EndChild();
    // keys: space play / pause, arrows a step
    if (fHover && !ImGui::GetIO().WantTextInput)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) m_fPlaying = !m_fPlaying;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { m_isel = -1; m_keyT = -1.0f; }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) m_t = std::max(0.0f, m_t - c_dtFrame);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) m_t = std::min(m_tEnd, m_t + c_dtFrame);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_isel > 0)
        {
            // (a selected animation key first: the key removed; else the clip)
            const std::vector<SUBROW> asubK = SubrowsOf(m_acmd[std::size_t(m_isel)]);
            if (m_keyT >= 0.0f && m_keyRow >= 0 && m_keyRow < int(asubK.size()))
            {
                if (FEditClipKeys(m_acmd[std::size_t(m_isel)], asubK[std::size_t(m_keyRow)], m_keyT, -1.0f, false)) m_keyT = -1.0f;
            }
            else FDeleteCommand(m_isel);
        }
    }
}

void CCutsceneEditor::DrawAddMenu(int icBefore)
{
    for (int tk = 0; tk < cut::TK_Max; ++tk)
    {
        if (!ImGui::BeginMenu(T(cut::SzTrack(cut::TRACKK(tk))))) continue;
        for (const cut::CMDDESC& d : cut::AllCommands())
        {
            if (d.tk != tk || d.kind == 0x01 || !cut::PbuilderOfKind(d.kind)) continue;
            if (ImGui::MenuItem(T(d.name)))
            {
                std::vector<std::pair<int, double>> aset;
                // sensible defaults: no object, a second, the playhead's view for a shot
                for (const cut::PARDESC& p : d.apar)
                    if (p.k == cut::PK_Object || p.k == cut::PK_Actor || p.k == cut::PK_Anim || p.k == cut::PK_Path || p.k == cut::PK_Script) aset.push_back({p.ib, -1});
                    else if (p.k == cut::PK_Seconds) aset.push_back({p.ib, 1.0});
                    else if (p.k == cut::PK_Stick) aset.push_back({p.ib, 1.0});
                    else if (p.k == cut::PK_Distance) aset.push_back({p.ib, 50.0});
                if (d.kind == 0x00) aset.push_back({0x10, -1});
                FInsertCommand(icBefore, d.kind, 0, aset, std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("add") + " " + T(d.name));
                if (d.kind == 0x00)
                {
                    // (a block needs its end)
                    Decode();
                    int i = icBefore < int(m_acmd.size()) ? icBefore + 1 : int(m_acmd.size());
                    FInsertCommand(i, 0x01, 0, {}, std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("add") + " " + T("End"));
                }
            }
            EdTip(T(d.tip));
        }
        ImGui::EndMenu();
    }
}

void CCutsceneEditor::DrawInspector()
{
    ImGui::BeginChild("##csinsp", ImVec2(0, 0), true);
    if (m_isel < 0 || m_isel >= int(m_acmd.size()))
    {
        ImGui::TextColored(st::vec(st::muted), "%s", T("Cutscene"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s %d: %zu %s", T("script"), m_oid, m_acmd.size(), T("commands"));
        // (the scene's settings in tabs: what it is, how it starts, what to add at the playhead, the characters)
        if (!ImGui::BeginTabBar("##cstabs")) { ImGui::EndChild(); return; }
        const char* aszTab[] = {T("Scene"), T("Start it"), T("Add"), T("Characters")};
        int itabNow = m_itab;
        for (int k = 0; k < 4; ++k)
        {
            ImGuiTabItemFlags fl = m_itabWanted == k ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem((std::string(aszTab[k]) + "###cstab" + std::to_string(k)).c_str(), nullptr, fl)) { itabNow = k; ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
        m_itabWanted = -1;
        m_itab = itabNow;
        ImGui::BeginChild("##cstab", ImVec2(0, 0), false);
        if (m_itab == 0)
        {
        ImGui::SeparatorText(T("Started by"));
        if (m_astarts.empty()) ImGui::TextDisabled("%s", T("(nothing found: a splice script may start it by its own name)"));
        for (const std::string& s : m_astarts) ImGui::BulletText("%s", s.c_str());
        ImGui::SeparatorText(T("What it does"));
        if (m_aeffects.empty()) ImGui::TextDisabled("%s", T("(no tasks, scripts or objects changed)"));
        for (const std::string& s : m_aeffects) ImGui::BulletText("%s", s.c_str());
        DrawRulePanel();
        }
        if (m_itab == 1) DrawStarterPanel();
        if (m_itab == 2)
        {
        if (ImGui::Button(T("Add a command at the end..."))) ImGui::OpenPopup("##addend");
        if (ImGui::BeginPopup("##addend")) { DrawAddMenu(int(m_acmd.size())); ImGui::EndPopup(); }
        ImGui::SeparatorText(T("Camera key"));
        ImGui::SetNextItemWidth(70);
        ImGui::InputFloat(TL("move", "ckb").c_str(), &m_keyBlend, 0, 0, "%.2f s");
        EdTip(T("How long the camera takes to get there (it ends at the playhead)"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        FEaseCombo("##ckease", m_keyEase);
        if (ImGui::Button(T("Camera key at the playhead from the view")))
        {
            std::string error;
            if (!FNewCameraKey(m_t, m_keyBlend, m_keyEase, error)) e.Status(error);
        }
        EdTip(T("Fly the view where the camera should be (turn Camera off to fly freely), then: the camera moves there from where it is, arriving at the playhead, and stays until the scene's next shot"));
        DrawPresentPanel();
        DrawEventPanel();
        }
        if (m_itab == 3)
        {
        DrawPathPanel();
        DrawNewClipPanel();
        ImGui::SeparatorText(T("Play session"));
        ImGui::BeginDisabled(m_fRecording);
        if (ImGui::Button(m_fRecording ? T("Recording...") : T("Record a play session"))) RecordSession();
        ImGui::EndDisabled();
        EdTip(T("The game starts in this level; play what the character should do, then close it. The recording comes back here."));
        std::error_code ec;
        if (cane::fs::exists(ce::pathOf(SessionPath()), ec))
        {
            ImGui::SameLine();
            if (ImGui::Button(T("Make a track from it")))
            {
                std::string error;
                if (!FImportSession(SessionPath(), m_oidPathActor, m_t, error)) e.Status(error);
            }
            EdTip(T("The recording as the chosen character's track from the playhead: its stops as waits, its moves as paths it walks or runs along"));
        }
        }
        ImGui::EndChild();
        ImGui::EndChild();
        return;
    }
    CSCMD& c = m_acmd[std::size_t(m_isel)];
    const cut::CMDDESC& d = cut::DescOfKind(c.kind);
    if (ImGui::SmallButton(T("< the scene"))) { m_isel = -1; m_keyT = -1.0f; ImGui::EndChild(); return; }
    EdTip(T("Back to the scene's settings (Esc, or a click on an empty spot of the tracks)"));
    ImGui::SameLine();
    ImGui::TextColored(st::vec(st::accent), "%s", FFadeClip(c) ? T("Fade") : T(d.name));
    ImGui::SameLine();
    ImGui::TextDisabled("(%s %d, 0x%X)", T("command"), c.ic, c.kind);
    ImGui::TextWrapped("%s", T(d.tip));
    ImGui::TextColored(st::vec(st::muted), "%.2f .. %.2f s%s", c.t0, c.t1, c.fMeasured ? T(" (measured)") : T(" (estimated)"));
    const bool fEditable = c.ifield >= 0 && m_decodeNote.empty();
    // (an animation: its keys first, what one edits it for; its arguments below)
    if (c.kind == 0x0A || c.kind == 0x22 || c.kind == 0x4B)
    {
        DrawClipPanel(c);
        ImGui::SeparatorText(T("Arguments"));
    }
    ImGui::BeginDisabled(!fEditable);
    const bool fTable = ImGui::BeginTable("##pars", 2, ImGuiTableFlags_SizingStretchProp);
    if (fTable)
    {
        ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
    }
    for (const cut::PARDESC& p : d.apar)
    {
        ImGui::PushID(p.ib);
        if (fTable)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(p.name));
            if (*p.tip) EdTip(T(p.tip));
            ImGui::TableSetColumnIndex(1);
        }
        const std::string label = std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + Label(c) + ": " + T(p.name);
        if (p.ib == -2)
        {
            bool f = c.b != 0;
            if (ImGui::Checkbox(fTable ? "##nowait" : T(p.name), &f))
            {
                const int iarg = IargOfSlot(c, -2);
                if (iarg >= 0) FPatchValue(c, iarg, 0, f ? 1 : 0, label);
            }
            EdTip(T(p.tip));
            ImGui::PopID();
            continue;
        }
        const bool fInRecord = IargOfSlot(c, p.ib) >= 0;
        ImGui::SetNextItemWidth(fTable ? -FLT_MIN : 130.0f);
        const std::string id = std::string("##") + p.name;
        switch (p.k)
        {
        case cut::PK_Pos: case cut::PK_Angles:
        {
            float v[3] = {c.G(uint32_t(p.ib)), c.G(uint32_t(p.ib + 4)), c.G(uint32_t(p.ib + 8))};
            if (p.k == cut::PK_Angles) for (float& g : v) g = glm::degrees(g);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputFloat3(id.c_str(), v, "%.1f", ImGuiInputTextFlags_EnterReturnsTrue))
                for (int q = 0; q < 3; ++q) FSetSlot(c, p.ib + 4 * q, p.k == cut::PK_Angles ? glm::radians(v[q]) : v[q], label);
            break;
        }
        case cut::PK_Float: case cut::PK_Seconds: case cut::PK_Distance: case cut::PK_Speed: case cut::PK_Stick: case cut::PK_Fov:
        {
            float g = c.G(uint32_t(p.ib));
            if (p.k == cut::PK_Fov) g = glm::degrees(g);
            if (ImGui::InputFloat(id.c_str(), &g, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue))
                FSetSlot(c, p.ib, p.k == cut::PK_Fov ? glm::radians(g) : g, label);
            break;
        }
        case cut::PK_Ease:
        {
            int ease = c.N(uint32_t(p.ib));
            ImGui::SetNextItemWidth(130.0f);
            if (FEaseCombo(id.c_str(), ease)) FSetSlot(c, p.ib, ease, label);
            break;
        }
        case cut::PK_Bool:
        {
            bool f = c.N(uint32_t(p.ib)) != 0;
            if (ImGui::Checkbox(id.c_str(), &f)) FSetSlot(c, p.ib, f ? 1 : 0, label);
            break;
        }
        default:
        {
            int n = c.N(uint32_t(p.ib));
            const bool fObj = p.k == cut::PK_Object || p.k == cut::PK_Actor || p.k == cut::PK_Anim || p.k == cut::PK_Path || p.k == cut::PK_Script;
            if (fObj) ImGui::SetNextItemWidth(90.0f);
            if (ImGui::InputInt(id.c_str(), &n, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) FSetSlot(c, p.ib, n, label);
            if (fObj)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", n == -1 ? T("(none)") : PloOfOid(n) ? e.NameOfObject(PloOfOid(n)).c_str() : T("(not in this world)"));
                if (ImGui::SmallButton(T("selection")) && e.m_plSel) FSetSlot(c, p.ib, e.m_plSel->oid, label);
                EdTip(T("The object selected in the 3D view"));
                ImGui::SameLine();
                LO* pt = PloOfOid(n);
                if (pt && ImGui::SmallButton(T("show"))) { e.SelectOnly(pt); e.FrameSelected(); }
            }
            if (p.k == cut::PK_Line || p.k == cut::PK_Text)
            {
                const std::string s = e.StrText(n);
                ImGui::TextWrapped("  \"%s\"%s", s.c_str(), p.k == cut::PK_Line ? (" (" + std::to_string(LineLength(n)).substr(0, 4) + " s)").c_str() : "");
            }
            if (p.k == cut::PK_TaskState) { ImGui::SameLine(); ImGui::TextDisabled("%s", SzTaskState(n)); }
            break;
        }
        }
        if (*p.tip && !fTable) EdTip(T(p.tip));
        if (!fInRecord && p.ib >= 0) { ImGui::SameLine(); ImGui::TextDisabled("%s", T("(set by the game)")); }
        ImGui::PopID();
    }
    if (fTable) ImGui::EndTable();
    ImGui::EndDisabled();
    ImGui::Separator();
    if ((c.kind == 0x27 || c.kind == 0x28) && PloOfOid(c.an[0]))
    {
        LO* pp = PloOfOid(c.an[0]);
        std::vector<glm::vec3> aw;
        const bool fPath = c.kind == 0x27 && FPathPoints(pp, aw);
        if (ImGui::Button(fPath ? T("Edit its points in the view") : T("Show the target")))
        {
            RestoreScene();
            m_fPreview = false;
            e.SelectOnly(pp);
            e.FrameSelected();
            if (fPath) { e.m_fCurveMode = true; e.m_ipoint = int(aw.size()) - 1; }
        }
        EdTip(T("Selects the path: drag its points with the gizmo (the last one is selected: where the character ends)"));
        if (fPath)
        {
            ImGui::SameLine();
            if (ImGui::Button(T("Add a point")) && aw.size() >= 2)
            {
                const glm::vec3 d = aw.back() - aw[aw.size() - 2];
                aw.push_back(aw.back() + (glm::length(d) > 1.0f ? d : glm::vec3(200, 0, 0)));
                FSetPathPoints(pp, aw, std::string(T("Cutscene path")) + " " + std::to_string(pp->oid) + ": " + T("a point added"));
            }
            ImGui::SameLine();
            if (ImGui::Button(T("Remove the last")) && aw.size() > 2)
            {
                aw.pop_back();
                FSetPathPoints(pp, aw, std::string(T("Cutscene path")) + " " + std::to_string(pp->oid) + ": " + T("a point removed"));
            }
            PATHDATA pd;
            bool fCurve = e.FPathRead(pp, pd) && pd.crvk != 0;
            ImGui::SameLine();
            if (ImGui::Checkbox(TL("curved", "pc").c_str(), &fCurve))
                FSetPathPoints(pp, aw, std::string(T("Cutscene path")) + " " + std::to_string(pp->oid) + ": " + (fCurve ? T("curved") : T("straight")), fCurve ? 1 : 0);
            EdTip(T("A smooth curve through its points (off: straight from point to point)"));
        }
    }
    if (c.kind == 0x12 || c.kind == 0x44 || c.kind == 0x53 || FFadeClip(c)) DrawPresentInspector(c);
    if (c.kind == 0x39)
        if (const int ics = IcShotOf(m_isel); ics > 0)
        {
            // (a key: the move to it, its easing and length)
            CSCMD& cs = m_acmd[std::size_t(ics)];
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T("the move to this key"));
            ImGui::SameLine();
            int ease = cs.N(0x44);
            ImGui::SetNextItemWidth(130.0f);
            if (FEaseCombo("##keyease", ease)) FSetSlot(cs, 0x44, ease, std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("camera key easing") + " " + T(cut::SzEase(ease)));
            float g = cs.G(0x40);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::InputFloat(TL("seconds", "keylen").c_str(), &g, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue))
                FSetSlot(cs, 0x40, std::max(c_dtFrame, g), std::string(T("Cutscene")) + " " + std::to_string(m_oid) + ": " + T("camera move") + " " + std::to_string(g).substr(0, 4) + " s");
            EdTip(T("How long the camera takes to reach this key's view (0: the commands' between the move and its end)"));
        }
    if (c.kind == 0x38 || c.kind == 0x39 || c.kind == 0x3F)
    {
        if (ImGui::Button(T("Set from the view"))) FShotFromView(m_isel);
        EdTip(T("The camera's place, angles and field of view from the 3D view (fly there first; the playhead's camera is left)"));
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!fEditable);
    if (ImGui::Button(T("Delete"))) FDeleteCommand(m_isel);
    ImGui::SameLine();
    if (ImGui::Button(T("Add before..."))) ImGui::OpenPopup("##addbefore");
    if (ImGui::BeginPopup("##addbefore")) { DrawAddMenu(m_isel); ImGui::EndPopup(); }
    ImGui::SameLine();
    if (ImGui::Button(T("Add after..."))) ImGui::OpenPopup("##addafter");
    if (ImGui::BeginPopup("##addafter")) { DrawAddMenu(m_isel + 1); ImGui::EndPopup(); }
    ImGui::EndDisabled();
    if (ImGui::Button(T("Play in game from here"))) PlayInGame(c.t0);
    ImGui::EndChild();
}

void CCutsceneEditor::Draw()
{
    if (!fOpen) return;
    if (m_pswList != e.m_psw.get()) BuildList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float top = ImGui::GetFrameHeight();
    const float wRight = std::clamp(size.x * 0.25f, 330.0f, 470.0f);
    const float x = 64, w = std::max(300.0f, size.x - wRight - 12 - x - 6);
    const float h = std::max(260.0f, (size.y - top) * 0.42f);
    ImGui::SetNextWindowPos(ImVec2(x, size.y - 36 - h), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    bool fKeep = true;
    ImGui::Begin(TL("Timeline", "timeline").c_str(), &fKeep, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    DrawToolbar();
    if (m_oid)
    {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float wInsp = std::clamp(avail.x * 0.36f, 260.0f, 480.0f);
        DrawTracks(avail.x - wInsp - 6, avail.y);
        ImGui::SameLine();
        DrawInspector();
    }
    else ImGui::TextDisabled("%s", T("Choose a cutscene above (the world's scripts that play scenes), or make a new one."));
    ImGui::End();
    if (!fKeep) Close();
    DrawGhosts();
    DrawPreviewOverlay();
}

bool CCutsceneEditor::FVerb(const std::string& verb, const std::string& arg)
{
    auto floats = [](const std::string& s, float* a, int n) {
        std::stringstream ss(s);
        std::string t;
        int i = 0;
        while (i < n && std::getline(ss, t, ',')) a[i++] = float(std::atof(t.c_str()));
        return i;
    };
    if (verb == "cutopen") { fOpen = true; BuildList(); m_fTraceWanted = arg.find(":notrace") == std::string::npos; Open(std::atoi(arg.c_str())); }
    else if (verb == "cuttimeline") { fOpen = arg != "0"; if (!fOpen) Close(); else BuildList(); }
    else if (verb == "cutlist")   // the world's cutscenes
    {
        BuildList();
        for (const CSSCENE& s : m_ascene)
            if (s.cLine || s.cShot || s.cPause) std::printf("editor: cutscene %d: %d commands, %d lines, %d shots%s\n", s.oid, s.cCmd, s.cLine, s.cShot, s.cPause ? ", pauses" : "");
    }
    else if (verb == "cutwait")   // until the open cutscene's measurement is in
    {
        if (m_fTracePending || m_fRemeasure) { e.m_iscript--; e.m_waitScript = 10; m_pollTrace = 14; }
        else std::printf("editor: cutscene %d: %s\n", m_oid, m_traceNote.c_str());
    }
    else if (verb == "cutprint")   // the open cutscene's commands (time, track, label, arguments)
    {
        for (const CSCMD& c : m_acmd)
        {
            std::printf("editor: cut %d %3d %-18s %7.2f %7.2f %s%s |", m_oid, c.ic, cut::DescOfKind(c.kind).verb, c.t0, c.t1, c.fMeasured ? "m" : c.fEstimate ? "~" : "e",
                        c.itrack >= 0 ? (" [" + m_atrack[std::size_t(c.itrack)].name + "]").c_str() : "");
            std::printf(" %s |", Label(c).c_str());
            for (const cut::PARDESC& p : cut::DescOfKind(c.kind).apar)
            {
                if (p.ib == -2) std::printf(" %s=%d", p.name, c.b);
                else if (p.k == cut::PK_Float || p.k == cut::PK_Seconds || p.k == cut::PK_Distance || p.k == cut::PK_Speed || p.k == cut::PK_Stick || p.k == cut::PK_Fov)
                    std::printf(" %s=%.3f", p.name, c.G(uint32_t(p.ib)));
                else if (p.k == cut::PK_Pos || p.k == cut::PK_Angles)
                    std::printf(" %s=(%.1f %.1f %.1f)", p.name, c.G(uint32_t(p.ib)), c.G(uint32_t(p.ib + 4)), c.G(uint32_t(p.ib + 8)));
                else std::printf(" %s=%d", p.name, c.N(uint32_t(p.ib)));
            }
            std::printf("\n");
        }
        std::printf("editor: cut %d: %zu tracks, end %.2f s%s\n", m_oid, m_atrack.size(), m_tEnd, m_decodeNote.empty() ? "" : (" (" + m_decodeNote + ")").c_str());
        for (const std::string& s : m_astarts) std::printf("editor: cut %d started by: %s\n", m_oid, s.c_str());
        for (const std::string& s : m_aeffects) std::printf("editor: cut %d does: %s\n", m_oid, s.c_str());
    }
    else if (verb == "cutkinds")   // (the value kinds the worlds store each builder argument type as)
    {
        std::map<std::pair<int, int>, int> mp;
        for (const EDITFIELD& f : e.m_rec.afield)
        {
            const cut::BUILDER* pb = f.fk == EDITFK_OptionArray ? cut::PbuilderOfEopid(f.eopid) : nullptr;
            std::string rel;
            std::size_t ib = 0;
            const std::vector<uint8_t>* pab = nullptr;
            if (!pb || f.ibEnd <= f.ib || !e.FLocate(f.ib, f.ibEnd - f.ib, rel, ib) || !(pab = e.m_proj.partRef(rel))) continue;
            std::size_t i = ib;
            for (int j = 0; j < pb->cArgs && i + 2 <= ib + (f.ibEnd - f.ib); ++j)
            {
                uint16_t w;
                std::memcpy(&w, pab->data() + i, 2);
                i += 2;
                int c = 1;
                if (w & 0x8000) c = (*pab)[i++];
                ++mp[{pb->alay[std::size_t(j)].otyp, w}];
                i += std::size_t(CbOfValueKind(w & 0x7FFF)) * std::size_t(c);
            }
        }
        for (const auto& [k, n] : mp) std::printf("editor: cutkinds otyp 0x%X value kind 0x%X: %d\n", k.first, k.second, n);
    }
    else if (verb == "cutt") { float a[1] = {0}; floats(arg, a, 1); m_t = std::clamp(a[0], 0.0f, m_tEnd); m_fPlaying = false; }   // cutt:SECONDS (the playhead)
    else if (verb == "cutsel") m_isel = std::atoi(arg.c_str());   // cutsel:COMMAND
    else if (verb == "cuttab") m_itabWanted = std::atoi(arg.c_str());   // cuttab:N (the scene inspector's tab: 0 Scene, 1 Start it, 2 Add, 3 Characters)
    else if (verb == "cutset")   // cutset:COMMAND:SLOT(hex)=VALUE (an argument by its slot, -2 its "don't wait")
    {
        const auto c1 = arg.find(':'), eq = arg.find('=');
        const int i = std::atoi(arg.c_str());
        if (c1 != std::string::npos && eq != std::string::npos && i >= 0 && i < int(m_acmd.size()))
        {
            const int slot = int(std::strtol(arg.substr(c1 + 1, eq - c1 - 1).c_str(), nullptr, 16));
            const double v = std::atof(arg.c_str() + eq + 1);
            if (slot == -2 || arg.substr(c1 + 1, eq - c1 - 1) == "-2") { const int iarg = IargOfSlot(m_acmd[std::size_t(i)], -2); FPatchValue(m_acmd[std::size_t(i)], iarg, 0, v, "cutset"); }
            else FSetSlot(m_acmd[std::size_t(i)], slot, v, "Cutscene " + std::to_string(m_oid) + ": command " + std::to_string(i) + " slot " + arg.substr(c1 + 1, eq - c1 - 1));
        }
    }
    else if (verb == "cutmove") { const int i = std::atoi(arg.c_str()); float a[2] = {0, 0}; floats(arg.substr(arg.find(':') + 1), a, 1); if (i > 0 && i < int(m_acmd.size())) FMoveClip(i, a[0]); }   // cutmove:COMMAND:DT
    else if (verb == "cutresize") { const int i = std::atoi(arg.c_str()); float a[1] = {0}; floats(arg.substr(arg.find(':') + 1), a, 1); if (i > 0 && i < int(m_acmd.size())) FResizeClip(i, a[0]); }   // cutresize:COMMAND:SECONDS
    else if (verb == "cutdelete") FDeleteCommand(std::atoi(arg.c_str()));   // cutdelete:COMMAND
    else if (verb == "cutadd")   // cutadd:BEFORE:KIND(hex)[:SLOT=V,...]
    {
        std::stringstream ss(arg);
        std::string a, k, rest;
        std::getline(ss, a, ':');
        std::getline(ss, k, ':');
        std::getline(ss, rest);
        std::vector<std::pair<int, double>> aset;
        std::stringstream sr(rest);
        for (std::string t; std::getline(sr, t, ',');)
            if (const auto eq = t.find('='); eq != std::string::npos) aset.push_back({int(std::strtol(t.substr(0, eq).c_str(), nullptr, 16)), std::atof(t.c_str() + eq + 1)});
        const int kind = int(std::strtol(k.c_str(), nullptr, 16));
        FInsertCommand(std::atoi(a.c_str()), kind, 0, aset, "Cutscene " + std::to_string(m_oid) + ": add " + cut::DescOfKind(kind).name);
    }
    else if (verb == "cutpath")   // cutpath:ACTOR:T:WALK:SPEED:x,y,z|x,y,z|...[:CURVED[:FACEDEG]] (a new path the character follows from T)
    {
        std::vector<std::string> ap;
        std::stringstream ss(arg);
        for (std::string t; std::getline(ss, t, ':');) ap.push_back(t);
        if (ap.size() >= 5)
        {
            std::vector<glm::vec3> aw;
            std::stringstream sp(ap[4]);
            for (std::string t; std::getline(sp, t, '|');) { float a[3] = {0, 0, 0}; if (floats(t, a, 3) == 3) aw.push_back({a[0], a[1], a[2]}); }
            std::string error;
            if (!FNewCharacterPath(std::atoi(ap[0].c_str()), float(std::atof(ap[1].c_str())), aw, ap[2] != "0", float(std::atof(ap[3].c_str())), -1, error,
                                   ap.size() >= 6 && ap[5] == "1", ap.size() >= 7 ? float(std::atof(ap[6].c_str())) : NAN))
                std::printf("editor: cutpath: %s\n", error.c_str());
        }
    }
    else if (verb == "cutpathpoints")   // cutpathpoints:PATH:x,y,z|... (its points replaced)
    {
        const auto c1 = arg.find(':');
        std::vector<glm::vec3> aw;
        std::stringstream sp(arg.substr(c1 + 1));
        for (std::string t; std::getline(sp, t, '|');) { float a[3] = {0, 0, 0}; if (floats(t, a, 3) == 3) aw.push_back({a[0], a[1], a[2]}); }
        if (LO* pp = PloOfOid(std::atoi(arg.c_str()))) FSetPathPoints(pp, aw, "Cutscene path " + std::to_string(pp->oid));
    }
    else if (verb == "cutimport")   // cutimport:FILE:ACTOR:T (a play session made into the character's track)
    {
        const auto c2 = arg.rfind(':'), c1 = arg.rfind(':', c2 - 1);
        std::string error;
        if (c1 == std::string::npos || !FImportSession(arg.substr(0, c1), std::atoi(arg.c_str() + c1 + 1), float(std::atof(arg.c_str() + c2 + 1)), error))
            std::printf("editor: cutimport: %s\n", error.c_str());
    }
    else if (verb == "cutpathinfo")   // cutpathinfo:PATH (its kind, parameters and world points)
    {
        PATHDATA pd;
        LO* pp = PloOfOid(std::atoi(arg.c_str()));
        std::vector<glm::vec3> aw;
        if (pp && e.FPathRead(pp, pd) && FPathPoints(pp, aw))
            for (std::size_t i = 0; i < aw.size(); ++i)
                std::printf("editor: path %d kind %d closed %d point %zu g %.3f at %.1f %.1f %.1f\n", pp->oid, pd.crvk, pd.fClosed ? 1 : 0, i, pd.ag[i], aw[i].x, aw[i].y, aw[i].z);
    }
    else if (verb == "cutrecord") RecordSession();
    else if (verb == "cutrule")   // cutrule:ONCE:TASK:STATES(1+2):SKIP(auto|always|never):LABEL
    {
        std::vector<std::string> ap;
        std::stringstream ss(arg);
        for (std::string t; std::getline(ss, t, ':');) ap.push_back(t);
        cane::mods::CutsceneRule r = RuleOf();
        if (ap.size() >= 5)
        {
            r.once = ap[0] == "1";
            r.task = std::atoi(ap[1].c_str());
            r.states.clear();
            std::stringstream sv(ap[2]);
            for (std::string t; std::getline(sv, t, '+');) if (!t.empty()) r.states.push_back(std::atoi(t.c_str()));
            r.skip = ap[3];
            r.skipToLabel = std::atoi(ap[4].c_str());
            SetRule(r);
        }
        const cane::mods::CutsceneRule q = RuleOf();
        std::printf("editor: cut %d rule: once %d task %d states %zu skip %s label %d\n", m_oid, q.once ? 1 : 0, q.task, q.states.size(), q.skip.c_str(), q.skipToLabel);
    }
    else if (verb == "cutevent")   // cutevent:T:KIND(hex):SLOT=V,... (a command at T on a track of its own)
    {
        std::stringstream ss(arg);
        std::string a, k, rest;
        std::getline(ss, a, ':');
        std::getline(ss, k, ':');
        std::getline(ss, rest);
        std::vector<std::pair<int, double>> aset;
        std::stringstream sr(rest);
        for (std::string t; std::getline(sr, t, ',');)
            if (const auto eq = t.find('='); eq != std::string::npos) aset.push_back({int(std::strtol(t.substr(0, eq).c_str(), nullptr, 16)), std::atof(t.c_str() + eq + 1)});
        FAddAtTime(float(std::atof(a.c_str())), int(std::strtol(k.c_str(), nullptr, 16)), 0, aset, "Cutscene " + std::to_string(m_oid) + ": event");
    }
    else if (verb == "cutstarter")   // cutstarter:TASK:STATE (the scene started from the task's script)
    {
        const int oidTask = std::atoi(arg.c_str());
        const auto c1 = arg.find(':');
        const int state = c1 == std::string::npos ? 1 : std::atoi(arg.c_str() + c1 + 1);
        std::vector<std::string> adiag;
        std::string error;
        const bool fOk = e.FWriteSceneStarter(m_oid, PloOfOid(oidTask), state, adiag, error);
        std::printf("editor: cutstarter %d: %s%s\n", m_oid, fOk ? "written" : "failed: ", error.c_str());
        for (const std::string& d : adiag) std::printf("editor: cutstarter %d: %s\n", m_oid, d.c_str());
    }
    else if (verb == "cutpresent")   // cutpresent:KIND:T:LENGTH[:TEXT or R,G,B] (0 letterbox, 1 title card, 2 caption, 3 fade)
    {
        std::vector<std::string> ap;
        std::stringstream ss(arg);
        for (std::string t; std::getline(ss, t, ':');) ap.push_back(t);
        if (ap.size() >= 3)
        {
            float rgb[3] = {0, 0, 0};
            if (ap.size() >= 4 && ap[0] == "3") floats(ap[3], rgb, 3);
            std::string error;
            if (!FAddPresentation(std::atoi(ap[0].c_str()), float(std::atof(ap[1].c_str())), float(std::atof(ap[2].c_str())), ap.size() >= 4 ? ap[3] : std::string(), rgb, 0.5f, 0.5f, error))
                std::printf("editor: cutpresent: %s\n", error.c_str());
        }
    }
    else if (verb == "cutcustom")   // cutcustom:0|1 (the custom presentation, SafeHouse)
    {
        SetCustomPresent(arg == "1");
        std::printf("editor: cut %d custom presentation %d\n", m_oid, FCustomPresent() ? 1 : 0);
    }
    else if (verb == "cutpresize")   // cutpresize:COMMAND:DT (a presentation clip's end dragged)
    {
        const int i = std::atoi(arg.c_str());
        const auto c1 = arg.find(':');
        if (i > 0 && i < int(m_acmd.size()) && c1 != std::string::npos) FResizePresentation(i, float(std::atof(arg.c_str() + c1 + 1)));
    }
    else if (verb == "cutstart")   // cutstart:KIND:OID:VALUE (0 task state, 1 volume, 2 used, 3 job start, 4 job end, 5 script event, 6 new button zone)
    {
        int a[3] = {0, -1, 1};
        std::stringstream ss(arg);
        std::string t;
        for (int k = 0; k < 3 && std::getline(ss, t, ':'); ++k) a[k] = std::atoi(t.c_str());
        std::vector<std::string> adiag;
        std::string error;
        const bool fOk = FWriteStarter(a[0], a[1], a[2], e.m_cam.pos + e.m_cam.Dir() * 400.0f, adiag, error);
        std::printf("editor: cutstart %d kind %d: %s%s\n", m_oid, a[0], fOk ? "written" : "failed: ", error.c_str());
        // (the Start it tab shows it as if pressed there)
        m_starterKind = a[0], m_oidStarterTask = a[1];
        (a[0] == 5 ? m_starterEvent : m_starterState) = a[2];
        m_starterNote = fOk ? std::string(T("written in its script (readable, with names)")) : error;
        m_astarterDiag = adiag;
        WhoStarts();
        for (const std::string& d : adiag) std::printf("editor: cutstart %d: %s\n", m_oid, d.c_str());
    }
    else if (verb == "cutkey")   // cutkey:T:BLEND:EASE (a camera key at T from the view)
    {
        float a[3] = {0, 1, 3};
        std::string args = arg;
        std::replace(args.begin(), args.end(), ':', ',');
        floats(args, a, 3);
        std::string error;
        if (!FNewCameraKey(a[0], a[1], int(a[2]), error)) std::printf("editor: cutkey: %s\n", error.c_str());
    }
    else if (verb == "cutsub")   // cutsub:COMMAND (an animation's channels and keys printed)
    {
        const int i = std::atoi(arg.c_str());
        if (i > 0 && i < int(m_acmd.size()))
            for (const SUBROW& r : SubrowsOf(m_acmd[std::size_t(i)]))
                std::printf("editor: cut %d sub %d: %s: %zu %s\n", m_oid, i, r.name.c_str(), r.at.size(), r.fEvents ? "events" : "keys");
    }
    else if (verb == "cutkeys")   // cutkeys:COMMAND (an animation's rows and each key's clip time)
    {
        const int i = std::atoi(arg.c_str());
        if (i > 0 && i < int(m_acmd.size()))
        {
            const std::vector<SUBROW> a = SubrowsOf(m_acmd[std::size_t(i)]);
            for (std::size_t r = 0; r < a.size(); ++r)
            {
                std::string ts;
                for (float t : a[r].at) { char sz[16]; std::snprintf(sz, sizeof sz, " %.3f", t); ts += sz; }
                std::printf("editor: cut %d keys %d row %zu %s:%s\n", m_oid, i, r, a[r].name.c_str(), ts.c_str());
            }
            if (ASEG* pa = PasegOfClip(m_acmd[std::size_t(i)]))
                if (const ASEGD* pd = PasegdOf(pa)) std::printf("editor: cut %d keys %d clip %d length %.3f\n", m_oid, i, pa->oid, pd->tMax);
        }
    }
    else if (verb == "cutzoom") { float a[2] = {m_pps, m_tView}; floats(arg, a, 2); m_pps = std::clamp(a[0], 2.0f, 2000.0f); m_tView = a[1]; m_fFitPending = false; }   // cutzoom:PIXELS_PER_SECOND,FIRST_SECOND
    else if (verb == "cutkeysel") { const auto c1 = arg.find(':'); m_keyRow = std::atoi(arg.c_str()); m_keyT = c1 == std::string::npos ? -1.0f : float(std::atof(arg.c_str() + c1 + 1)); }   // cutkeysel:ROW:T (a key selected)
    else if (verb == "cutkeymove" || verb == "cutkeyadd" || verb == "cutkeydel")   // cutkeymove:COMMAND:ROW:T:TO / cutkeyadd:COMMAND:ROW:T / cutkeydel:COMMAND:ROW:T
    {
        std::vector<std::string> ap;
        std::stringstream ss(arg);
        for (std::string t; std::getline(ss, t, ':');) ap.push_back(t);
        const int i = ap.empty() ? -1 : std::atoi(ap[0].c_str());
        if (ap.size() >= 3 && i > 0 && i < int(m_acmd.size()))
        {
            const std::vector<SUBROW> a = SubrowsOf(m_acmd[std::size_t(i)]);
            const int r = std::atoi(ap[1].c_str());
            if (r >= 0 && r < int(a.size()))
            {
                const float t = float(std::atof(ap[2].c_str()));
                if (verb == "cutkeyadd") FEditClipKeys(m_acmd[std::size_t(i)], a[std::size_t(r)], t, 0.0f, true);
                else if (verb == "cutkeydel") FEditClipKeys(m_acmd[std::size_t(i)], a[std::size_t(r)], t, -1.0f, false);
                else if (ap.size() >= 4) FEditClipKeys(m_acmd[std::size_t(i)], a[std::size_t(r)], t, float(std::atof(ap[3].c_str())), false);
            }
        }
    }
    else if (verb == "cutcliplen")   // cutcliplen:COMMAND:SECONDS (the clip retimed)
    {
        const int i = std::atoi(arg.c_str());
        const auto c1 = arg.find(':');
        if (i > 0 && i < int(m_acmd.size()) && c1 != std::string::npos) FEditClipKeys(m_acmd[std::size_t(i)], SUBROW{}, 0.0f, 0.0f, false, float(std::atof(arg.c_str() + c1 + 1)));
    }
    else if (verb == "cutpose")   // cutpose:ACTOR:T:AT (the actor's pose at the scene's time T as a key at AT of a new clip)
    {
        float a[3] = {0, 0, 0};
        std::string args = arg;
        std::replace(args.begin(), args.end(), ':', ',');
        floats(args, a, 3);
        LO* plAseg = nullptr;
        float tClip = 0.0f;
        if (FAnimOn(int(a[0]), a[1], plAseg, tClip)) { m_apose.push_back({int(a[0]), plAseg->oid, tClip, a[2]}); std::printf("editor: cut %d pose %zu: %d by clip %d at %.2f -> %.2f\n", m_oid, m_apose.size(), int(a[0]), plAseg->oid, tClip, a[2]); }
        else std::printf("editor: cutpose: no animation on %d at %.2f\n", int(a[0]), a[1]);
    }
    else if (verb == "cutnewclip")   // cutnewclip:T (the poses made a clip, played from T)
    {
        std::string error;
        const int oidActor = m_apose.empty() ? -1 : m_apose[0].oidActor;
        const int oid = NewClipFromPoses(error);
        std::printf("editor: new animation %d%s\n", oid, error.empty() ? "" : (": " + error).c_str());
        if (oid)
        {
            Decode();
            FAddAtTime(float(std::atof(arg.c_str())), 0x0A, 0, {{0x10, oid}, {0x14, oidActor}, {0x18, 0}, {-2, 0}}, "Cutscene " + std::to_string(m_oid) + ": new animation");
            m_apose.clear();
        }
    }
    else if (verb == "cutshot") FShotFromView(std::atoi(arg.c_str()));   // cutshot:COMMAND (the camera from the view)
    else if (verb == "cutnew") { std::string error; const int oid = CreateCutscene(error); std::printf("editor: new cutscene %d%s\n", oid, error.empty() ? "" : (": " + error).c_str()); }
    else if (verb == "cutplay") { float a[1] = {0}; floats(arg, a, 1); PlayInGame(a[0]); }   // cutplay:SECONDS (the game from there)
    else if (verb == "cutmeasure")   // cutmeasure[:MODE[:TASK]] (0 alone, 1 from its starter)
    {
        if (!arg.empty()) { m_measureMode = std::atoi(arg.c_str()); const auto c1 = arg.find(':'); m_taskMeasure = c1 == std::string::npos ? 0 : std::atoi(arg.c_str() + c1 + 1); }
        StartTrace();
    }
    else if (verb == "cutopts")   // cutopts:camera,scene,ghosts,byrun[,poses] (1 / 0 each)
    {
        float a[5] = {float(m_fLookThrough), float(m_fPreview), float(m_fGhosts), float(m_fByRun), float(m_fPoses)};
        floats(arg, a, 5);
        if (m_fPoses != (a[4] != 0)) RestoreScene();
        m_fLookThrough = a[0] != 0, m_fPreview = a[1] != 0, m_fGhosts = a[2] != 0, m_fPoses = a[4] != 0;
        if (m_fByRun != (a[3] != 0)) { m_fByRun = a[3] != 0; BuildTracks(); }
        m_tApplied = -1.0f;
    }
    else if (verb == "cutsurvey") Survey();   // the preview against what the game drew in view (notes/CUTSCENES)
    else if (verb == "cutscene")   // the scene at the playhead printed (the camera, the moved objects)
    {
        CAMERA cam;
        if (FCameraAt(m_t, cam)) std::printf("editor: cut %d at %.2f: camera %.1f %.1f %.1f fov %.1f\n", m_oid, m_t, cam.pos.x, cam.pos.y, cam.pos.z, cam.fovY);
        for (const CSTRACE::OBJ& o : m_trace.aobj)
        {
            glm::vec3 p;
            glm::quat q;
            bool fIn;
            if (FObjectAt(o, m_t, p, q, fIn) && FCidDerivedFrom(o.cid, EDCID(0x34))) std::printf("editor: cut %d at %.2f: %s at %.1f %.1f %.1f\n", m_oid, m_t, NameOf(o.oid).c_str(), p.x, p.y, p.z);
        }
        // the subtitle the preview shows (the line measured playing at the playhead)
        for (const auto& l : m_trace.aline)
            if (l.t0 <= m_t && m_t < l.t1) std::printf("editor: cut %d at %.2f: subtitle %d \"%s\"\n", m_oid, m_t, l.id, e.StrText(l.id).c_str());
        // the poses (the clip on each animated character at the playhead; its records' spread about its object record)
        for (const auto& k : m_aoidAnimated)
        {
            LO* plo = PloOf(k[0], k[1], k[2]), *plAseg = nullptr;
            float tClip = 0.0f;
            if (!plo || !FIsLoDerivedFrom(plo, EDCID(0x34)) || !FAnimOn(k[0], m_t, plAseg, tClip)) continue;
            const POSE* pp = PposeAt(static_cast<ALO*>(plo), m_t);
            double sum = 0.0;
            if (pp)
                for (const auto& [pxf, rel] : *pp) sum += glm::length(glm::vec3(rel[3]));
            std::printf("editor: cut %d pose at %.2f: %s by clip %d at %.2f: %zu records, spread %.1f\n", m_oid, m_t, NameOf(k[0]).c_str(), plAseg->oid, tClip, pp ? pp->size() : 0, sum);
        }
    }
    else return false;
    return true;
}

// ---- CEditor's side

void CEditor::DrawTimeline()
{
    if (!m_pcut) m_pcut = std::make_shared<CCutsceneEditor>(*this);
    m_pcut->Frame();
    m_pcut->Draw();
}

void CEditor::TimelineMenuItem()
{
    if (!m_pcut) m_pcut = std::make_shared<CCutsceneEditor>(*this);
    bool f = m_pcut->fOpen;
    if (ImGui::MenuItem(T("Cutscene timeline"), nullptr, &f)) { if (f) m_pcut->fOpen = true; else m_pcut->Close(); }
}

bool CEditor::FTimelineOpen() const { return m_pcut && m_pcut->fOpen; }
bool CEditor::FCutsceneLooking() const { return m_pcut && m_pcut->fOpen && m_pcut->FLooking(); }

bool CEditor::FCutsceneVerb(const SCRIPTCMD& c)
{
    if (!m_pcut) m_pcut = std::make_shared<CCutsceneEditor>(*this);
    return m_pcut->FVerb(c.verb, c.arg);
}

}  // namespace edimpl

#endif
