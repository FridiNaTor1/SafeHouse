#pragma once
// The map editor's internals, shared by Sly 2 and Sly 3 (each game's editor.cpp compiles editor_shell.inc with its own
// headers: the includes below are the game's; editor_game.h says which game and what it offers): the CEditor class.
// The shared feature modules (editor/game/editor_*.inc, compiled by each game whose editor_game.h turns the feature on:
// EDFEAT_*) and each game's own modules (Sly2-Release/editor_*.cpp, Sly3-Release/editor_*3.cpp) implement the members;
// Sly 3's editor_game3.cpp answers "not yet" for the features it does not have. Sly2-Release/notes/EDITOR_2026-11-12.md.
#include "editor.h"
#include "editor_game.h"

#if CANE_EDITOR_3D

#include "alo.h"
#include "brx.h"
#include "cls.h"
#include "disc.h"
#include "display.h"
#include "editrec.h"
#include "gl.h"
#include "light.h"
#include "lightsw.h"
#include "lo.h"
#include "render.h"
#include "scene.h"
#include "scenesw.h"
#include "shd.h"
#include "sw.h"
#include "worldaccess.h"
#include "worldparts.h"
#include "so.h"
#include "bsp.h"
#include "crv.h"

#include "core/project.h"
#include "core/spawn.h"
#include "core/style.h"

#include "../platform/platform.h"
#include "../platform/mod_worlds.h"

#include <imgui/imgui.h>
#include <imgui/imgui_impl_glfw.h>
#include <imgui/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <stb/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <climits>
#include <array>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <functional>
#include <unordered_map>

bool FResolveWorld(const std::string& w, FK& fk, int& filter, std::string& error);   // sw.cpp
namespace cane::editor::model { struct Scene; }
struct CMDLIST;   // (Sly 2's command lists: editor_guard.inc)


namespace edimpl
{
class CCutsceneEditor;   // (editor_cutscene.inc, Sly 2: the cutscene editor and its Timeline dock)
namespace ce = cane::editor;
namespace st = cane::editor::style;

// An option's bytes by its kind (brx.cpp ReadOptionValue)
struct OPTTYPE
{
    int cb = 0;        // 0: not editable here (arrays, references without a value)
    int cf = 0;        // floats
    int ci = 0;        // integers (cb / ci bytes each)
    const char* name = "";
};
inline OPTTYPE OptType(int k)
{
    switch (k < 24 ? k : 24)
    {
    case 0: return {1, 0, 1, "byte"};
    case 5: case 6: return {4, 0, 1, "int"};
    case 1: return {4, 1, 0, "float"};
    case 10: return {8, 2, 0, "2 floats"};
    case 2: case 9: case 11: return {12, 3, 0, "vector"};
    case 4: return {12, 3, 0, "euler"};
    case 3: case 12: return {16, 4, 0, "4 floats"};
    case 13: return {4, 0, 1, "colour"};
    case 22: return {0, 0, 0, "list"};
    case 8: case 18: case 24:
    {
        const int cls = (k & ~0x8000) >> 12;
        if (cls == 1) return {4, 0, 1, "word"};
        if (cls == 2) return {4, 0, 1, "object id"};
        return {0, 0, 0, "reference"};
    }
    default: return {2, 0, 1, "short"};
    }
}

struct EDOBJ
{
    LO* plo = nullptr;
    int iobjrec = -1;          // EDITRECORDER::aobj
    int imat = -1;             // its matrix field (EDITRECORDER::afield)
    std::vector<int> aiopt;    // its option fields
    std::string relRecord;     // a top-level record: its Objects part and offset there
    std::size_t ibRecordFile = 0;
    int ipath = -1;            // a PATH's curve (EDITFK_Path)
    int ilinks = -1;           // a TASK's links (EDITFK_TaskLinks)
    std::vector<int> aiarr;    // its method / list options (EDITFK_OptionArray: shown read only)
    int iglobset = -1;         // an ALO's glob set (EDITFK_Globset: its model)
    int isoes = -1;            // an ALO's collision entries (EDITFK_Soes)
};

// editor_data.cpp / editor_data3.cpp: names the port knows (the executables have none): classes, options, gameplay areas
const char* SzOptionName(int eopid);      // null: not known
bool FOidOption(int eopid);               // an integer option whose value is an oid (its kind does not say so)
std::string StrOption(int eopid);         // "1264 job" / "1264"
}  // namespace edimpl
#include "editor_objtypes.h"   // (GAMEAREA; the object types and vis groups: shared with the games' debug overlays)
namespace edimpl
{
GAMEAREA AreaOfCid(int cid);
const char* SzArea(GAMEAREA ga);
// the key options shown first for an area (the rest under "All options")
const std::vector<int>& KeyOptionsOfArea(GAMEAREA ga);

// A PATH's curve as stored (EDITFK_Path), in the path's own space
struct PATHDATA
{
    std::string rel;
    std::size_t ib = 0;          // its S8 kind in the part
    int crvk = 0;
    bool fClosed = false;
    std::vector<float> ag;
    std::vector<glm::vec3> apos, avIn, avOut;
};

// A task's links (EDITFK_TaskLinks)
struct TASKLINKS
{
    std::string rel;
    std::size_t ib = 0, cb = 0;    // the two lists in the part (U8 c, S32 oids, U8 c, S32 oids)
    std::vector<int> aoidPre, aoidPost;
};

// A dialogue line (World/Sound.bin's dialog table, sound.cpp LoadMusicFromBrx)
struct DLGLINE
{
    int id = 0, oidSpeaker = 0;
    std::size_t ibId = 0;                     // its S16 id in the part
    std::vector<std::size_t> aibLang;         // per language: its 64-byte file key (then S8 no-headset, F32 length)
    std::vector<float> atLength;
    std::vector<bool> afNoHeadset;
    int cevent = 0;
    std::size_t ibEvents = 0;                 // its events (8 bytes each) in the part
};

// A record copied with edits (editor_mission.cpp FBuildCopy)
struct RECEDIT
{
    std::map<int, int> mpRemap;          // oid values (options, list options) moved to new ones
    std::set<int> setRemove;             // the top object's options dropped
    std::map<int, int> mpSet;            // the top object's options set
    bool fLinks = false;                 // a task: its links replaced
    std::vector<int> aoidPre, aoidPost;
    int nModule = INT_MIN;               // the script module (INT_MIN: kept)
    bool fDropScripts = false;           // nested records' modules dropped (a copy into another group)
    std::set<int> setTaskDone;           // list options' (task, state) pairs naming these tasks: state 2 (done)
    std::map<int, std::vector<uint8_t>> mpAdd;   // options added to the top object (eopid: its value bytes)
};

// A value of a list option (EDITFK_OptionArray) in its part
struct LISTEL
{
    std::size_t ib = 0;
    int cb = 0;
    bool fFloat = false;
};

// A problem found before a save
struct PROBLEM
{
    int oid = 0;          // the object it is about (0: none)
    std::string text;
};

const char* SzClass(int cid);
// a tooltip for the last item (shown after a short hover)
inline void EdTip(const char* sz)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", sz);
}
inline const char* SzClassRaw(int cid) { return cid >= 0 && cid < CID_Max && g_acls[cid].name && *g_acls[cid].name ? g_acls[cid].name : "?"; }

// The 12 floats of a stored matrix (bis.cpp ReadMatrix4: the 3 x 3, then the translation)
inline glm::mat4 MatFromBytes(const uint8_t* pb)
{
    float a[12];
    std::memcpy(a, pb, 48);
    glm::mat4 m(1.0f);
    for (int i = 0; i < 3; ++i) m[i] = glm::vec4(a[3 * i], a[3 * i + 1], a[3 * i + 2], 0.0f);
    m[3] = glm::vec4(a[9], a[10], a[11], 1.0f);
    return m;
}
inline bool FBytesFromMat(const glm::mat4& m, float a[12])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) a[3 * i + j] = m[i][j];
    for (int j = 0; j < 3; ++j) a[9 + j] = m[3][j];
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(a[i])) return false;
    return true;
}

// A top-level record's header: Sly 2's S8 k [k S16 filters]; Sly 3's S8 mode (1: U8 n1, U8 n2, n1 S16 filters, n2 S32
// conditions); then S16 cid, S32 oid, S16 n (its script module), S32 cb. The bytes before the cid:
inline std::size_t CbRecordPrefix(const uint8_t* pb)
{
#if EDGAME_SLY3
    return int8_t(pb[0]) == 1 ? 3 + std::size_t(pb[1]) * 2 + std::size_t(pb[2]) * 4 : 1;
#else
    const int k = int8_t(pb[0]);
    return 1 + std::size_t(k > 0 ? k : 0) * 2;
#endif
}
inline std::size_t IbOidOfRecord(const uint8_t* pb) { return CbRecordPrefix(pb) + 2; }   // (then S16 n, S32 cb)
// a shared group's record (Sly 2 k -1 / -2, Sly 3 mode 3 / 2); fOnce: loaded once for every instance (k -2, mode 2)
inline bool FSharedRecord(const uint8_t* pb, bool fOnce = false)
{
    const int k = int8_t(pb[0]);
#if EDGAME_SLY3
    return fOnce ? k == 2 : (k == 2 || k == 3);
#else
    return fOnce ? k == -2 : k < 0;
#endif
}
// a shared group's record made a plain one (the level's own)
inline void MakeOwnRecord(uint8_t* pb)
{
    if (FSharedRecord(pb)) pb[0] = 0;
}

// What moves an object: its own stored matrix, or (an instance of a shared group) the proxy record that places it
struct PLACE
{
    LO* ploLive = nullptr;     // the object moved in the view (the instance's root for a proxy)
    std::size_t ibMat = 0;     // the matrix in the world file
    bool fProxy = false;
    int oidProxy = 0;
    int iobjrec = -1;          // the record deleted / duplicated (the proxy's, or the object's top-level one)
};

// A convex piece of collision (a brush's): its points and its faces (counter-clockwise seen from outside)
struct CONVEX
{
    std::vector<glm::vec3> apos;
    std::vector<std::vector<int>> aface;
};

// A brush's extra material slot (a mesh brush's faces: another object's texture or a grid of a colour)
struct BRUSHMAT
{
    int texFrom = 0;
    uint32_t rgb = 0xB8B8B8;
};

// A brush (editor_brush.inc): a shape the editor makes into an object's model and collision, kept in the project
// (editor/brushes.json) so it can be changed and made again
struct BRUSH
{
    int shape = 0;                 // 0 block, 1 ramp, 2 stairs, 3 cylinder, 4 floor
    glm::vec3 size{256, 256, 256}; // width (x), depth (y), height (z)
    int steps = 8, segments = 16;
    float texScale = 256.0f;       // units per texture repeat
    int texFrom = 0;               // the object whose first material's texture it takes (0: a grid in rgb)
    uint32_t rgb = 0xB8B8B8;
    int oid = 0;                   // the object it became
    // shape 6 (a mesh brush, editor_brushedit.inc): its convex pieces, each face's material slot (0: the brush's own
    // texFrom / rgb, i: amat[i - 1])
    std::vector<CONVEX> apiece;
    std::vector<std::vector<int>> afaceMat;
    std::vector<BRUSHMAT> amat;
};
struct BRUSHSEL
{
    int ip = -1, iface = -1, v0 = -1, v1 = -1;   // the piece, its face, a point (v0) or an edge (v0, v1)
};

struct SCRIPTCMD
{
    std::string verb, arg;
};

// The editor's own strings in the user's language (editor_lang.inc: editor/lang/<code>.json keyed by the English text;
// English when the language has no entry). Every new UI string goes through it.
const char* T(const char* szEnglish);
// "Text###id": a translated label with a fixed ImGui id
std::string TL(const char* szEnglish, const char* id);
// printf into a string (its format already translated with T())
std::string Fmt(const char* fmt, ...);
bool FSetEditorLanguage(const std::string& code);   // (editor_lang.inc) "": SAFEHOUSE_LANGUAGE, else English
const std::string& EditorLanguage();
void AddEditorLanguageFonts(ImGuiIO& io, float pixels);
// (editor_dialogue.inc) the game's text as shown (Sly 2: the disc's 0x80..0x9F bytes as the Windows-1252 marks they
// are, e.g. ’ and …) and as stored back (typed marks as those bytes)
std::string GameTextShown(const std::string& s);
std::string GameTextStored(const std::string& s);

// ---- object types and vis groups: editor_objtypes.h (included above), editor_types.inc
// a stable key for an object across reloads (its oid and the proxy that placed its instance)
inline uint64_t KeyOfLo(const LO* plo) { return (uint64_t(uint32_t(plo->oid)) << 32) | uint32_t(plo->oidInstance); }

class CEditor
{
public:
    explicit CEditor(const EDITOROPTS& eo) : m_eo(eo) {}
    int Run();

private:
    // the world
    bool FLoad(std::string& error);
    bool FLocate(std::size_t ibWorld, std::size_t cb, std::string& rel, std::size_t& ibFile) const;
    void IndexObjects();
    // transforms
    bool FHasMatrix(const LO* plo) const;
    glm::mat4 MatLocal(const LO* plo) const;
    glm::mat4 MatWorld(const LO* plo) const;
    glm::vec3 PosWorld(const LO* plo) const { return glm::vec3(MatWorld(plo)[3]); }
    void SetLocal(LO* plo, const glm::mat4& m);
    bool FRecordLocal(LO* plo, const std::string& label);
    void SetWorld(LO* plo, const glm::mat4& w);
    void ReapplyFromParts();
    // edits
    LO* PloTopLevel(LO* plo) const;
    bool FPlace(LO* plo, PLACE& pl) const;
    glm::mat4 MatPlace(const PLACE& pl) const;
    void SetPlace(const PLACE& pl, const glm::mat4& m);
    void SetPlaceWorld(const PLACE& pl, const glm::mat4& w);
    bool FRecordPlace(const PLACE& pl, const std::string& label);
    void Duplicate();
    void Delete();
    void Undo();
    void Redo();
    bool FSave();
    void PlayTest() { PlayTestAt(-1, nullptr, 0.0f); }
    // the game with the project: at a task (its job started there: --world W:TASK) and / or Sly at a point (--start-pos)
    void PlayTestAt(int oidTask, const glm::vec3* ppos, float heading);
    std::string WorldArg(int record) const;
    glm::vec3 DirPlace() const;   // the direction new objects are placed along (the visible view's middle)
    const char* ClassFriendlyName(int cid) const;
    void PlayFromView();                 // Sly on the ground below the view's middle, facing the view's way
    int TaskToPlay(LO* plo) const;       // a job's first task, a task's own oid (0: none)
    void DrawPlayMenu();
    bool m_fPlayDry = false, m_fOpenPlayMenu = false;             // (automation: the command printed, not started)
    void Reload(int oidSelect);
    // UI
    void DrawMenu();
    void DrawTree();
    void DrawTreeNode(LO* plo, int depth);
    void DrawInspector();
    void DrawWorldPanel();
    void DrawGizmo();
    void DrawMarkers();
    void Pick(float x, float y);
    void CameraInput(float dt);
    void FrameSelected();
    bool FProject(const glm::vec3& p, ImVec2& s) const;
    glm::vec3 RayDir(float x, float y) const;
    void Select(LO* plo) { if (plo != m_plSel) { m_ipoint = -1; m_fCurveMode = false; } m_plSel = plo; m_fScrollTree = true; }
    bool FRunScript();
    void Status(const std::string& s) { m_status = s; std::printf("editor: %s\n", s.c_str()); }

    // ---- editor_types.inc: object types, friendly names, visibility, the outliner
    OBJTYPE TypeOfLo(const LO* plo) const;
    std::string NameOfObject(const LO* plo) const;     // (hook: a better name source plugs in here)
    std::string GroupNameOf(const LO* plo) const;      // its group's name (an instance: the shared group's)
    bool FHidden(const LO* plo) const;                 // hidden by its type, vis group, own flag or the isolation
    void InvalidateVis() { ++m_visVersion; }
    void ApplyVisibility();                            // every frame, before the render
    void DrawOutliner();
    void DrawOutlinerRow(LO* plo, const char* idPrefix);
    void OutlinerContextMenu(LO* plo);
    void HideSelection(bool fHide);
    void IsolateSelection();
    void ShowAll();
    void SaveViewState();                              // the project's editor/view.json: hidden types / groups, snapping
    void LoadViewState();
    // ---- selection (several objects; m_plSel is the primary: the last clicked, shown in Properties)
    bool FSelected(const LO* plo) const { return std::find(m_asel.begin(), m_asel.end(), plo) != m_asel.end(); }
    void SelectOnly(LO* plo) { Select(plo); m_asel.clear(); if (plo) m_asel.push_back(plo); }
    void ToggleSelected(LO* plo);
    void AddSelected(LO* plo);
    void ClearSelection() { m_asel.clear(); Select(nullptr); }
    void SyncSelection();                              // m_asel follows a direct change of m_plSel (the older code's)
    void SelectWhere(const std::function<bool(LO*)>& f, bool fAdd);
    bool FSelectable(LO* plo) const;                   // movable as a whole (its placement is its own) and shown
    std::vector<PLACE> PlacesOfSelection() const;      // each placement once (an instance's proxy, an own matrix)
    void BoxSelect(const ImVec2& a, const ImVec2& b, bool fAdd);
    void DuplicateSelection();
    void DeleteSelection();
    void CopySelection();
    void Paste(bool fInPlace);
    std::vector<uint64_t> KeysOfSelection() const;
    void SelectKeys(const std::vector<uint64_t>& akey);
    LO* PloOfKey(uint64_t key) const;
    // ---- snapping
    float SnapMove(float v) const { return m_fGridSnap && m_gridSize > 0 ? std::round(v / m_gridSize) * m_gridSize : v; }
    float SnapAngle(float deg) const { return m_fAngleSnap && m_angleSnap > 0 ? std::round(deg / m_angleSnap) * m_angleSnap : deg; }
    float SnapScale(float s) const { return m_fScaleSnap && m_scaleSnap > 0 ? std::max(m_scaleSnap, std::round(s / m_scaleSnap) * m_scaleSnap) : s; }
    void DrawToolStrip();
    void DrawStatusBar();
    // ---- editor_exist.inc: when an object exists (hook: the script names' plain words replace the body)
    void DrawExistence(LO* plo, const EDOBJ* po);
    void Screenshot(const std::string& path);

    // ---- editor_data.cpp: options, validation
    bool FFieldBytes(int ifield, int cb, std::string& rel, std::size_t& ib, const std::vector<uint8_t>*& pab) const;
    bool FPathLive(const LO* plo, PATHDATA& pd) const;   // the live curve (drawing: no part read)
    bool FSetOption(int ifield, const void* pv, int cb, const std::string& label);
    void DrawOptions(LO* plo, const EDOBJ* po);
    bool FOptionInt(LO* plo, int eopid, int& n) const;           // a recorded option's integer value
    int IfieldOption(LO* plo, int eopid) const;                   // its field (-1: none)
    std::vector<PROBLEM> Validate();
    int OidNew();                                                 // a free oid (every recorded oid, nested ones too)
    std::vector<std::size_t> EnclosingSizeFields(const std::string& rel, std::size_t ibFile) const;
    bool FRecordRange(int iobjrec, std::string& rel, std::size_t& ib, std::size_t& cb) const;
    void DrawProblems();
    bool FListElements(int ifield, std::string& rel, std::vector<LISTEL>& a) const;
    GAMEAREA AreaOfLo(LO* plo) const;                             // its gameplay area (by its class; Sly 3's uses by who may use them)
    std::string StrLo(LO* plo) const;                             // "USE 15351 (Murray)"
    // links: the options that make an object act on others (a use's task states, state machines, script)
    bool FAddOption(LO* plo, int eopid, const std::vector<uint8_t>& abValue, const std::string& label);   // structural
    bool FRemoveOption(int ifield, const std::string& label);                                            // structural
    bool FAddLink(LO* plo, int kind, const int an[3], std::string& error);   // kind: 0 a task's state, 1 a script, 2 a state machine
    void DrawLinks(LO* plo, const EDOBJ* po);
    bool FSetListElement(int ifield, int iel, double v, std::string& error);
    // ---- editor_path.cpp: curves
    bool FPathRead(const LO* plo, PATHDATA& pd) const;
    bool FPathWrite(LO* plo, const PATHDATA& pd, const std::string& label);
    void PathLive(LO* plo, const PATHDATA& pd);
    glm::mat4 MatPathSpace(const LO* plo) const;                  // path space -> world
    glm::vec3 PathPivot(const LO* plo, const PATHDATA& pd) const; // in path space
    void DrawPaths();
    void DrawPathInspector(LO* plo);
    bool FSnapDown(const glm::vec3& w, glm::vec3& hit);
    bool FPathTransform(LO* plo, const glm::mat4& mWorld, const std::string& label);   // the whole curve about its pivot
    bool FPathSnap(LO* plo, int ipoint, const std::string& label);                     // -1: every point
    void ReapplyPathsFromParts();
    // ---- editor_mission.cpp: jobs, tasks, markers
    bool FTaskLinks(const LO* plo, TASKLINKS& tl) const;
    bool FSetTaskLinks(LO* ploTask, const std::vector<int>& aoidPre, const std::vector<int>& aoidPost, const std::string& label);
    void DrawMissions();
    int CreateJob(int oidJobSrc, std::string& error);             // the new job's oid (0: failed)
    int CopyIntoJob(int oidSrc, int oidJob, std::string& error);  // an object copied with its task references moved to oidJob's
    LO* PloOid(int oid, int cid = -1) const;
    int OidFreeSmall(int oidFrom);
    bool FBuildCopy(int iobjrec, int oidNew, const RECEDIT& re, std::vector<uint8_t>& rec, std::string& error);
    int IobjOfLo(LO* plo) const;
    bool FAppendCopy(int iobjrecSrc, const std::vector<uint8_t>& rec, std::string& error);
    bool FBumpInstances(int igrp, int d);
    int IgrpOfProxy(int oidProxy) const;
    void BumpProxiesIn(int iobjrec, int d);
    std::string StrText(int id) const;
    // ---- editor_script.cpp: scripts, dialogue, text
    void DrawScripts();
    void DrawDialogue();
    void LoadScriptText(int igroup);
    bool FCheckScript(const std::string& text, std::string& error, int& line, std::vector<PROBLEM>* papb, int igroup = -1);
    bool FApplyScript(int igroup, const std::string& text, std::string& error);
    int AppendScriptModule(int igroup, const std::string& body, std::string& error);   // its index (-1: failed)
    bool FWriteSceneStarter(int oidScene, LO* ptask, int state, std::vector<std::string>& adiag, std::string& error);   // (Sly 2: editor_scriptdiag.inc)
    bool FWriteObjectHandler(LO* pobj, const std::string& handler, const std::vector<std::string>& aparam, const std::string& form,
                             const std::vector<std::pair<std::string, int>>& anames, std::vector<std::string>& adiag, std::string& error, bool fMakeModule);   // (Sly 2)
    // ---- editor_scriptnames.inc (Sly 2): the Scripts tab's names (kept in the part when Names is on: the game
    // compiles them), "when it exists" in plain words
    bool m_fScriptReadable = true;
    std::shared_ptr<struct EXISTCACHE> m_pexist;
    const SW* m_pswExist = nullptr;
    std::string ScriptObjectName(int oid) const;
    std::string ScriptReadable(int igroup, const std::string& raw, std::string* perror = nullptr);
    bool FScriptToRaw(const std::string& text, std::string& raw, std::string& error, std::vector<std::string>* panew = nullptr);
    void BuildExistCache();
    void EnsureExistCache();
    void ExistenceFacts(LO* plo, std::vector<std::string>& alines);
    // ---- editor_scriptdiag.inc (Sly 2): the Scripts tab's diagnostics (underlines, the problems list, quick fixes)
    std::shared_ptr<struct SCRIPTDIAG> m_pscriptdiag;
    void RunScriptDiagnostics();                               // the shown text now
    void ScriptDiagnosticsTick();                              // each frame: again a moment after an edit
    float ScriptDiagPanelHeight() const;                       // the problems list's height (0: none)
    void DrawScriptDiagUnderlines();                           // right after the text input: its squiggles, hover text
    void DrawScriptDiagPanel();                                // the problems list (below the text)
    std::string ScriptDiagSummary() const;                     // "2 errors, 1 warning"
    // (both games) a scripts part's converter text: a readable one (Sly 2's names) as the converter prints it
    std::string ScriptTextRaw(const std::string& text);
    bool FParseDialogue(std::vector<DLGLINE>& a, std::vector<std::string>& alang, std::string& error) const;
    bool FSetText(int id, const std::string& text, std::string& error);
    bool FSetDialogueEvent(const DLGLINE& l, int k, int dt60, int itarget, int32_t value, std::string& error);
    void ReloadText();
    // ---- editor_create.cpp: palette, copies across groups, make unique, nested records
    void DrawCreate();
    int CopyRecord(int iobjrec, int igrpDest, const glm::vec3* ppos, bool fSelect, std::string& error);
    bool FMakeUnique(LO* plo, std::string& error);
    bool FDeleteNested(int iobjrec, std::string& error);
    int DuplicateNested(int iobjrec, std::string& error);
    void RenumberCopy(std::vector<uint8_t>& rec, int iobjrecSrc, std::size_t ibSrc, int oidTop, bool fDropScripts, std::map<int, int>* pmpoid);
    int LevelGroup() const;
    int CopyGroup() const;      // the group copies, creations and made-unique objects go into
    // ---- editor_model.cpp: models, textures, materials, GLB / FBX import, GLB export
    bool FFieldRange(LO* plo, EDITFK fk, std::string& rel, std::size_t& ib, std::size_t& cb) const;
    std::string RelGraphicsOf(const std::string& relObjects) const;
    void PrintModelInfo(LO* plo);
    bool FImportModel(const std::string& path, LO* plo, bool fNew, std::string& error);
    bool FImportTexture(const std::string& png, LO* plo, int imat, int mode, std::string& error);
    bool FSetMaterial(LO* plo, int imat, int shdk, int grfshd, const uint32_t argba[3], int grftex, std::string& error);
    bool FExportGlb(LO* plo, int kind, const std::string& path, std::string& error);
    struct EXPORTRIG;               // (editor_model.cpp: a character's skeleton as the export builds it)
    void ExportAnimations(LO* plo, cane::editor::model::Scene& sc, const EXPORTRIG& rig);
    bool FPoseDump(LO* plo, int oidAseg, float t, bool fCycle, const std::string& path, std::string& error);
    bool FPoseCharacter(LO* plo, LO* plAseg, float tClip, std::vector<std::pair<struct XF*, glm::mat4>>& arel);   // (Sly 2: the cutscene editor's poses)
    bool FPoseCharacter(LO* plo, const std::vector<std::pair<LO*, float>>& aclip, std::vector<std::pair<struct XF*, glm::mat4>>& arel);   // (layered clips)
    bool FPoseCharacter(LO* plo, const std::vector<std::tuple<LO*, LO*, float>>& aclip, std::vector<std::pair<struct XF*, glm::mat4>>& arel);   // (on its parts too)
    void UnbindPosed(LO* plo);
    bool FSetCollision(LO* plo, int kind, std::string& error);
    void DrawModels();
    void DrawModelSection(LO* plo);
    void ApplyProjectTextures();
    void OpenFileDialog(const char* title, const char* filters, const std::string& start, std::function<void(const std::string&)> fn);
    void DrawFileDialogs();
    void DrawHelp();
    void ForgetThumbnails();
    unsigned ThumbOf(const SHD* pshd, int* pdx, int* pdy);

    EDITOROPTS m_eo;
    ce::Installation m_inst;
    ce::WorldIndex m_index;
    ce::Project m_proj;
    std::vector<std::unique_ptr<SW>> m_apswOld;   // (worlds loaded before a reload: kept, the loader has no unload)
    std::unique_ptr<SW> m_psw;
    SCENE m_scene;
    std::vector<ALO*> m_apalo;
    std::vector<RAVELSEG> m_aseg;
    EDITRECORDER m_rec;
    std::unordered_map<const LO*, EDOBJ> m_mpobj;
    std::map<int, std::vector<LO*>> m_mpgroupRoots;
    std::unordered_map<const LO*, std::vector<LO*>> m_mpchildren;
    std::unordered_map<int, int> m_mpoidProxy;              // a proxy record's oid -> EDITRECORDER::aobj
    std::map<std::size_t, glm::mat4> m_mpProxyApplied;      // a proxy matrix (world file offset) as the view shows it
    LO* m_plSel = nullptr;
    bool m_fScrollTree = false;
    CAMERA m_cam;
    RENDEROPTS m_opts;
    float m_speed = 1000.0f;
    int m_gizmo = 0;   // 0 move, 1 rotate, 2 scale
    bool m_fLocalAxes = false;
    int m_axisDrag = -1;
    glm::mat4 m_matDragStart{1.0f};
    ImVec2 m_mouseDragStart{};
    bool m_fShowMarkers = true;
    char m_filter[64] = "";
    std::string m_status;
    std::vector<SCRIPTCMD> m_ascript;
    bool m_fScripted = false;     // (--editor-do steps still running)
    std::size_t m_iscript = 0;
    int m_waitScript = 2;
    std::string m_shotPending;
    bool m_fQuit = false;
    std::vector<ce::TextString> m_atext;
    std::string m_textLanguage;
    int m_igroupScript = -1;
    std::string m_scriptText;
    int m_tab = 0;
    // phase 3
    int m_ipoint = -1;            // the selected path point (-1: the whole curve)
    int m_pivot = 0;              // 0 the curve's middle, 1 its first point, 2 the object's origin, 3 the selected point
    bool m_fCurveMode = false;    // the gizmo transforms the selected path's points (not the object)
    bool m_fShowPaths = true;
    bool m_fShowAllPaths = false;   // the curves near the camera, not only the selected one
    bool m_fSnapCollision = false;
    bool m_fPhysReady = false;
    std::vector<SOE*> m_apsoeLevel;
    std::string m_scriptEdit;     // the Scripts tab's text being edited
    std::string m_scriptError;
    int m_scriptErrorLine = 0;
    bool m_fScriptDirty = false;
    int m_ojobActive = 0;         // the Missions tab's job
    std::vector<PROBLEM> m_aproblem;
    bool m_fProblemsOpen = false;
    bool m_fSaveAnyway = false;
    int m_igrpCreate = -1;
    char m_filterCreate[64] = "";
    std::string m_note;           // the last edit's note (live or after reload)
    std::vector<int> m_aoidIssued;
    std::map<int, std::map<int, int>> m_mpjobRemap;   // a job made this session: the old oids -> its copies'
    std::map<int, int> m_mpjobLast;                   // its last task
    int m_linkTask = 0;
    char m_linkPre[256] = "", m_linkPost[256] = "";
    int m_tabRight = 0;
    int m_iobjPalette = -1;
    glm::mat4 m_matDragCur{1.0f};
    PATHDATA m_pdDrag;
    int m_tabCur = 0;             // the World panel's tab shown (wider for scripts / dialogue)
    // phase 4
    char m_importPath[512] = "";
    char m_exportPath[512] = "";
    char m_texPath[512] = "";
    float m_importScale = 100.0f;   // file units (glTF: metres) -> game units
    int m_importMaxTex = 256;       // the game copy's largest side (the full image goes into the package's Textures)
    bool m_importLit = true;        // lit like the replaced model's shaders (else unlit, flat)
    int m_importCollision = 0;      // 0 kept as it is, 1 none, 2 a box around the model
    bool m_importOutline = true;    // cel borders made for the imported parts
    int m_imat = 0;                 // the Models tab's material
    std::string m_lastExport;
    bool m_fShowHelp = false;       // the getting-started panel
    bool m_fShowKeys = false;       // the shortcuts list
    bool m_fHelpAtStart = true;     // (the user's choice, kept in <user dir>/Editor/editor.ini)
    int m_exportKind = 0;           // 0 the object, 1 its group, 2 a character
    int m_exportAnim = 0;           // a character's animations: 0 as the game shows them (limbs solved, baked), 1 the clips' channels only
    int m_exportFps = 60;           // the baked keys a second (60: every game step; 30)
    bool m_exportReduce = true;     // keys dropped where the motion stays within a small error
    std::string m_exportAnimInfo;   // the last character export's animations (shown in its status line)
    int m_texMode = 0;              // texture import: 0 the texture pack, 1 the game data
    std::function<void(const std::string&)> m_fnFile;
    std::map<std::pair<const void*, const void*>, unsigned> m_mpthumb;
    int m_igrpCopyDest = -1;        // (CopyRecord -> RenumberCopy: the group a copy goes into, its scripts carried there)
    std::string m_noteCopy;   // material thumbnails (GL textures)
    // ---- editor 2 (notes/EDITOR_PLAN.md)
    std::vector<LO*> m_asel;                        // the selection (m_plSel: its primary)
    mutable std::unordered_map<const LO*, OBJTYPE> m_mptype;
    bool m_afTypeHidden[OT_Max] = {};
    bool m_afVgHidden[VG_Max] = {};
    std::set<uint64_t> m_setHidden;                 // objects hidden one by one (KeyOfLo)
    std::set<uint64_t> m_setIsolate;                // isolated: only these (and their parts) shown
    int m_visVersion = 1, m_visApplied = 0;
    mutable std::unordered_map<const LO*, bool> m_mphidden;   // (FHidden's answers for m_visVersion)
    mutable int m_visCached = 0;
    std::vector<int> m_aiHiddenEntries;             // scene entries hidden at the last ApplyVisibility
    std::vector<int> m_aiSceneHidden;               // (Sly 2's cutscene preview) entries the game has out of the world at the playhead
    char m_outFilter[96] = "";
    int m_outType = -1;                             // the outliner's type filter (-1: all)
    int m_outMode = 0;                              // 0 by type, 1 by group (hierarchy), 2 flat list
    bool m_fOutScroll = false;
    bool m_fBoxDrag = false, m_fClickPending = false;
    ImVec2 m_mouseDown{};
    std::vector<std::pair<PLACE, glm::mat4>> m_adragSel;   // the selection's placements at a drag's start
    glm::mat4 m_matDragPivot{1.0f};
    float m_gridSize = 64.0f, m_angleSnap = 15.0f, m_scaleSnap = 0.1f;
    bool m_fGridSnap = true, m_fAngleSnap = true, m_fScaleSnap = false;
    struct CLIP { uint64_t key; glm::vec3 pos; };
    std::vector<CLIP> m_aclip;
    bool m_fDockOpen = true;                        // the bottom dock (the world's tabs)
    int m_tool = 1;                                 // the tool strip: 0 select (no gizmo), 1 the gizmo (m_gizmo)
    std::string m_viewStateLoaded;
    // objects whose live matrix is their stored one under another object's (attached at the load: a GROUPER's
    // effects): the stored matrix is P^-1 x live, and they follow their parent live
    struct LOADPARENT { LO* ploParent = nullptr; glm::mat4 P{1.0f}; };
    std::unordered_map<const LO*, LOADPARENT> m_mpLoadParent;
    std::unordered_map<const LO*, std::vector<LO*>> m_mpLoadKids;
    void FindLoadParents();
    // ---- editor_build.inc: the traversal / interactive palette, who uses what
    int IobjExampleOf(int cid) const;
    void LayOutCurve(LO* plo, float length);
    int BuildFromPalette(int cid2, bool fCurve, std::string& error);
    void DrawBuildPalette();
    std::string StrWhoUses(LO* plo) const;
    void DrawBuildSection(LO* plo);
    float m_buildLength = 800.0f;
    bool m_fBuildVertical = false;
    // (step 7-8 part 2) each player's own rules: Sly 1, Bentley 2, Murray 4 (apszWhy[3]: why / why not, each)
    uint32_t GrfPlayersOf(LO* plo, std::string* apszWhy, int* peopidMask, bool* pfKnown) const;
    bool FSetPlayers(LO* plo, uint32_t grf);
    int BuildCrawlSpace(std::string& error);           // a crawl space: its entries and a low ceiling (brushes)
    int ImportRecordOfClass(int cid, const glm::vec3& pos, std::string& from, std::string& error);
    int BuildKey(LO* pguard, std::string& error, int mode = 0);   // a key (a prize with its own task) in a guard's pocket (a shared guard: 0 refused, 1 made unique, 2 every one's)
    bool FKeyLock(int oidTask, const std::vector<LO*>& alock, std::string& error);
    void KeyButton(LO* pguard);
    void DrawKeyUniquePopup();
    int m_oidKeyLast = 0, m_oidKeyTask = 0, m_oidKeyGuard = 0;
    bool m_fKeyUniqueAsk = false, m_fKeyProxy = false;
    int m_cKeyShared = 0;
    std::vector<std::string> PocketWords(LO* plo) const;
    std::string StrExitTo(LO* plo) const;              // an exit's destination in words
    bool FSetExitTo(LO* plo, LO* pother);              // an exit made to lead where another one leads
    void DrawInteractSection(LO* plo);                 // pockets, exits, vaults, prizes (the Properties)
    bool FBuildVerb(const std::string& verb, const std::string& arg);
    float m_crawlHeight = 100.0f, m_crawlWidth = 192.0f;
    float m_dockScroll = -1.0f;                        // (automation: the Create tab scrolled there)
    // ---- editor_states.inc: map states (step 9)
    struct STATEROW
    {
        std::string id, name, trigger, kind, from;
        int oidTaskStart = -1;
        std::vector<std::pair<int, int>> atask;   // (task, state)
    };
    void BuildStates();
    void ShowState(int istate);                         // -1: every object
    void PollState();
    bool FStateHides(const LO* plo) const;
    void DrawStates();
    void DrawStateEditor();
    void PlayInState(int istate);
    bool FSaveStates(std::string& error);
    std::string StrStateWhen(const cane::mods::MapState& s) const;
    bool FStatesVerb(const std::string& verb, const std::string& arg);
    std::vector<STATEROW> m_astate;
    std::vector<std::string> m_astateWhy;
    std::vector<std::pair<int, int>> m_astateTask;
    std::set<int> m_setStateOut;
    std::string m_statePending, m_playMapState;
    int m_istateShown = -1, m_istateEdit = -1, m_statePoll = 0;
    bool m_fStatesBuilt = false, m_fStateHide = true, m_fStateNewOpen = false;
    cane::mods::MapState m_stateNew;
    // the shown state's poses (its state machines' records, as the game left them: Paris's fallen sign), applied to the
    // live objects' records while it is shown (restored for a drag, again after a reload, gone when hidden)
    struct STATEPOSE { int oid = 0, order = 0, ixf = 0; glm::mat4 mat{1.0f}; };
    std::vector<STATEPOSE> m_astatePose;
    std::vector<std::array<int, 3>> m_astateSm;          // (object, state machine, state) in the shown state
    std::map<XF*, std::pair<ALO*, glm::mat4>> m_mpPoseOrig;
    const SW* m_pswPosed = nullptr;
    bool m_fStatePose = true;
    void ApplyStatePoses();
    void RestoreStatePoses();
    // ---- editor_brushedit.inc: brush faces / edges / points (step 13)
    bool FBrushMakeEditable(BRUSH& b, std::string& error);
    void BrushFixMats(BRUSH& b);
    bool FBrushPick(float x, float y);
    bool FBrushApply(BRUSH& b, const std::string& label);
    bool FBrushOp(int op, const glm::vec3& v, float g);
    void DrawBrushEdit();
    void DrawBrushEditTools(BRUSH& b);
    bool FBrushEditVerb(const std::string& verb, const std::string& arg);
    bool m_fBrushEdit = false;
    int m_brushMode = 0, m_brushOther = 0;
    float m_brushDist = 64.0f;
    glm::vec3 m_brushMove{0.0f, 0.0f, 32.0f};
    BRUSHSEL m_bsel;
    // ---- editor_everything.inc: every option addable / removable (step 10)
    bool FAddAnyOption(LO* plo, int eopid, const std::vector<double>& av, std::string& error);
    void DrawOptionTools(LO* plo, const EDOBJ* po);
    bool FEverythingVerb(const std::string& verb, const std::string& arg);
    char m_szOptFind[64] = "";
    int m_eopidAdd = -1, m_ifieldRemove = -1;
    double m_avAdd[4] = {0, 0, 0, 0};
    // ---- editor_newlevel.inc: brand-new levels (step 11)
    bool FCreateLevel(std::string& error);
    void OpenLevel(int record);
    bool FSaveManifestOnly(std::string& error);
    void DrawEntries();
    glm::vec3 PosViewGround();
    void DrawLevelTab();
    void DrawNewLevelDialog();
    bool FLevelVerb(const std::string& verb, const std::string& arg);
    std::string m_nlName = "My Level";
    int m_nlTemplate = 16, m_nlEntryWorld = -1, m_nlMadeRecord = -1;
    bool m_nlKeepLevel = false, m_nlReturn = true, m_fNewLevelOpen = false, m_nlFloor = true, m_nlLights = true;
    float m_nlSpawn[4] = {0, 0, 200, 0}, m_nlEntryAt[4] = {0, 0, 0, 120}, m_nlReturnAt[4] = {0, 1500, 100, 150};
    // ---- editor_guests.inc: characters from other levels with their behaviour (mod.json "guests")
    std::string GuestWorldName() const;
    std::vector<cane::mods::Guest> GuestsHere() const;
    void PutGuests(const std::vector<cane::mods::Guest>& aHere);
    bool FAddGuest(int record, const std::string& group, std::string& error);
    void DrawGuests();
    void DrawGuestsPanel();
    bool FGuestVerb(const std::string& verb, const std::string& arg);
    int m_iguest = -1;
    // ---- editor_prefab.inc: prefabs (several objects saved as one piece, placed in any level)
    struct PREFAB { std::string name, path, from, where, thumb; int c = 0; };
    std::string PrefabLibraryDir() const;
    bool FSavePrefab(const std::string& name, std::string& path, std::string& error);
    bool FModuleInner(const std::string& text, int n, std::string& inner) const;
    int PlacePrefab(const std::string& path, const glm::vec3& pos, std::string& error);
    void ListPrefabs();
    void DrawPrefabs(float tile, int ccol, const std::function<bool(const std::string&, const std::string&)>& match);
    bool FPrefabVerb(const std::string& verb, const std::string& arg);
    std::vector<PREFAB> m_aprefab;
    bool m_fPrefabsListed = false;
    char m_szPrefabName[96] = "My prefab";
    // ---- editor_lighting.inc: lights, fog and skies (the Lighting tab)
#if EDGAME_SLY2
    const EDITFIELD* PworldOpt(int iList, int eopid) const;
    bool FWorldOptFloat(int eopid, float& g, int iList = 1, int iFloat = 0) const;
    bool FSetWorldOpt(int eopid, const void* pv, std::size_t cb, std::string& error, int iList = 1);
    void ReloadWorldOpts();
    void ApplyFogPreview();
    void SetLightOption(LO* pl, int eopid, const void* pv, std::size_t cb);
    void RelightView();
    void MoveLightTo(LO* pl, const glm::vec3& pos);
    int AddLight(const glm::vec3& pos);
#endif
    bool FLightValues(LO* plo, glm::vec3& hsv, float& strength, glm::vec2& falloff) const;
    void DrawLightingTab();
    bool FLightingVerb(const std::string& verb, const std::string& arg);
    bool m_fWorldOptsRead = false, m_fFogPreview = false;
    float m_fogNear = 500.0f, m_fogFar = 20000.0f, m_fogMax = 0.5f;
    uint32_t m_rgbaFog = 0x80303030u;
    // ---- editor_sound.inc: ambient sounds and music (the Sound tab)
    struct MUSICTRACK { int id = -1; std::string name, path; float seconds = 0; };
#if EDGAME_SLY2
    bool FPlayPreview(const std::string& path);
    void ReadSoundManifest();
    std::string SampleOfSound(int sfxid);
    bool FMusicEntries(const std::vector<uint8_t>& ab, std::vector<std::size_t>& aib) const;
    std::string SoundPart() const;
    bool FMusicKey(int id, uint8_t key[64]);
    void ReadMusicKeys();
    int TrackOfMusicEntry(int i);
    bool FReplaceMusic(int i, int idNew, std::string& error);
    std::vector<int> MusicOfLevel();
    std::string MusicFile(int id);
    void MoveObjectTo(LO* plo, const glm::vec3& pos);
    int AddSoundEmitter(const glm::vec3& pos);
#endif
    void StopPreview();
    void DrawSoundTab();
    bool FSoundVerb(const std::string& verb, const std::string& arg);
    bool m_fSoundManifest = false;
    std::vector<MUSICTRACK> m_amusicAll;
    std::map<int, std::string> m_mpsoundSample;
    std::map<int, std::vector<uint8_t>> m_mpMusicKey;   // (a track's file key, from the installed levels)
    bool m_fMusicKeys = false;   // (this level's bank sound -> its first sample's WAV)
    // ---- editor_check.inc: Check level (problems, warnings, jump to, quick fixes)
    enum { CF_None, CF_RemoveOption, CF_DropLinks, CF_FloorUnder, CF_SpawnDown, CF_RemoveEntry, CF_RemoveGuest, CF_GuestNoPatrol, CF_MoveToView };
    struct CHECK { int sev = 1; std::string cat, text; int oid = 0, fix = 0, ifield = -1, arg = 0; glm::vec3 pos{0.0f}; };
    bool FGroundBelow(const glm::vec3& w, glm::vec3& hit);
    void CheckLevel();
    std::vector<PLACE> PlacesOfAll();
    bool FApplyCheckFix(int i);
    void DrawCheckPanel();
    bool FCheckVerb(const std::string& verb, const std::string& arg);
    std::vector<CHECK> m_acheck;
    std::vector<SOE*> m_apsoeAll;
    bool m_fCheckSoes = false, m_fCheckOpen = false;
    // ---- editor_guard.inc: guards' patrols
    std::string StrCommand(const void* pcmd, LO** pploRef) const;
    LO* PatrolOf(LO* plo) const;                    // its patrol's command list (null: none)
    std::vector<LO*> PatrolPaths(LO* plo) const;   // the paths its patrol walks / goes to
    void DrawCurve(LO* plo, uint32_t rgb, float thick);
    void DrawPatrols();
    std::vector<std::string> PatrolWords(LO* plo) const;
    void DrawGuardSection(LO* plo);
    bool m_fAllPatrols = false;
#if EDGAME_SLY2
    const ::CMDLIST* PcmdlistOfGuard(LO* plo) const;
#endif
    // ---- editor_assets.inc: the asset browser
    struct ASSET
    {
        std::string name, group;
        int igrp = -1;
        std::set<OBJTYPE> atype;
        std::vector<int> aoidProxy;   // its instances' proxies in the map
        bool fModel = false, fWanted = false, fTried = false;
        unsigned tex = 0;
    };
    struct OTHERASSET { std::string group, name, types, thumb; int instances = 0; bool fCharacter = false; };
    std::vector<int> EntriesOfGroup(int igrp) const;
    bool FRenderThumb(const std::vector<int>& aentry, unsigned& tex, std::vector<uint8_t>* prgba);
    void BuildAssets();
    void RenderPendingThumbs();
    void WriteThumbs(const std::string& dir);
    void PlaceAsset(const ASSET& a);
    std::string AssetsDirOf(int record) const;
    void MarkOtherCharacters();
    void StartOtherLevelEditor(int record, const std::string& steps);
    void LoadOtherLevel(int record);
    void PollOtherLevel();
    unsigned TexOfFile(const std::string& path);
    void DrawAssets();
    void ImportFromOtherLevel(const std::string& group);
    void ForgetAssets();
    std::vector<ASSET> m_aasset;
    std::vector<OTHERASSET> m_aother;
    int m_iasset = -1, m_assetSrc = 0, m_otherRecord = -1, m_pollFrame = 0;
    bool m_fAssetsBuilt = false, m_fOtherPending = false;
    char m_assetFilter[96] = "";
    unsigned m_fboThumb = 0;
    std::string m_importPending;
    std::map<std::string, unsigned> m_mpfiletex;
    // ---- editor_brush.inc: brushes (blocks, ramps, stairs, cylinders, floors) made into objects
    std::vector<CONVEX> m_aconvexImport;   // (FImportModel: collision mode 3 writes these pieces)
    std::vector<BRUSH> m_abrush;
    BRUSH m_brushNew;
    bool FBuildBrush(const BRUSH& b, cane::editor::model::Scene& sc, std::vector<CONVEX>& acv, std::string& error);
    int CreateBrush(const BRUSH& b, const glm::vec3* ppos, std::string& error);   // the new object's oid (0: failed)
    bool FRebuildBrush(BRUSH& b, std::string& error);
    BRUSH* PbrushOf(const LO* plo);
    void LoadBrushes();
    void SaveBrushes();
    void DrawBrushTools();          // the Create tab's brush section
    void DrawBrushProperties(LO* plo);
    // ---- editor_camera.inc: camera objects (preview, look through, pilot) and camera zones' names
    const char* SzOptionOf(const LO* plo, int eopid) const;   // an option's name for this object (camera zones by field)
    bool FOptionFloat(LO* plo, int eopid, float& g) const;
    bool FCameraView(LO* plo, CAMERA& cam) const;
    void RenderCameraPreview();
    void DrawCameraFrusta();
    void DrawCameraPanel();
    void UpdateLookThrough();
    void StorePilot();
    bool m_fPreview = true, m_fPreviewShown = false, m_fLookThrough = false, m_fPilot = false, m_fPilotMoved = false;
    CAMERA m_camBeforeLook;
    unsigned m_fboPreview = 0, m_texPreview = 0;
    int m_dxPreview = 0, m_dyPreview = 0;
    float m_fovEdit = 57.3f, m_fovCameraDefault = 57.29578f;   // (CM's own: 1 radian)
    // ---- editor_cutscene.inc (Sly 2): the cutscene editor, its Timeline dock (in the bottom dock's place while open)
    friend class CCutsceneEditor;
    std::shared_ptr<CCutsceneEditor> m_pcut;
    void DrawTimeline();                       // every frame (the playhead's scene applied, the dock drawn when open)
    void TimelineMenuItem();                   // View > Cutscene timeline
    bool FTimelineOpen() const;
    bool FCutsceneLooking() const;             // the timeline looks through the scene's camera (its picture: no markers)
    bool FCutsceneVerb(const SCRIPTCMD& c);    // --editor-do cut...
};

}  // namespace edimpl

#endif
