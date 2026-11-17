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
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <functional>
#include <unordered_map>

bool FResolveWorld(const std::string& w, FK& fk, int& filter, std::string& error);   // sw.cpp
namespace cane::editor::model { struct Scene; }


namespace edimpl
{
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
enum GAMEAREA : int
{
    GA_None, GA_Traversal, GA_Guards, GA_Triggers, GA_Lasers, GA_Warps, GA_Pickups, GA_Cameras, GA_Lights, GA_Missions, GA_Hacking,
    GA_TwoPlayer, GA_Buttons, GA_Gang, GA_Rides, GA_Max
};
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

struct SCRIPTCMD
{
    std::string verb, arg;
};

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
    void PlayTest();
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
    void ExportAnimations(LO* plo, cane::editor::model::Scene& sc, const std::map<const XF*, int>& mpjoint, const glm::mat4& C, const glm::mat4& refInv);
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
    int m_texMode = 0;              // texture import: 0 the texture pack, 1 the game data
    std::function<void(const std::string&)> m_fnFile;
    std::map<std::pair<const void*, const void*>, unsigned> m_mpthumb;
    int m_igrpCopyDest = -1;        // (CopyRecord -> RenumberCopy: the group a copy goes into, its scripts carried there)
    std::string m_noteCopy;   // material thumbnails (GL textures)
};

}  // namespace edimpl

#endif
