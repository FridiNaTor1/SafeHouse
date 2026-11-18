// The map editor for Sly 3: this game's part of the shared shell (editor/game/editor_impl.h, editor_shell.inc) and of
// the shared feature modules (editor/game/editor_*.inc, on for Sly 3 in editor_game.h: options, curves, duplicate /
// delete / create / make unique, missions, scripts; their Sly 3 differences are in the modules under EDGAME_SLY3). The
// features still Sly 2's only (EDFEAT_* 0: models and textures, Sly 3's glob and shader formats; and those turned off)
// answer "not for Sly 3 yet" here and change nothing. Sly2-Release/notes/EDITOR_2026-11-12.md "Phase 5", "Round 5".
#include "editor_impl.h"

#if SLY3_EDITOR

#if EDFEAT_STRUCT && !EDFEAT_MISSIONS
#error "the record copies (duplicate, create) are in the missions module (editor/game/editor_mission.inc)"
#endif

namespace edimpl
{
namespace
{
[[maybe_unused]] constexpr const char* kNotYet = "not for Sly 3 yet";
[[maybe_unused]] bool FNotYet(std::string& error)
{
    error = kNotYet;
    return false;
}
}  // namespace

#if !EDFEAT_MISSIONS
LO* CEditor::PloOid(int oid, int cid) const
{
    LO* p = nullptr;
    for (const auto& up : m_psw->aploAll)
        if (up->oid == oid && (cid < 0 || up->Cid() == cid))
            if (!p || (FHasMatrix(up.get()) && !FHasMatrix(p))) p = up.get();
    return p;
}
int CEditor::IobjOfLo(LO* plo) const
{
    const auto it = m_mpobj.find(plo);
    if (it == m_mpobj.end() || it->second.relRecord.empty()) return -1;
    return it->second.iobjrec;
}
bool CEditor::FTaskLinks(const LO*, TASKLINKS&) const { return false; }
bool CEditor::FSetTaskLinks(LO*, const std::vector<int>&, const std::vector<int>&, const std::string&) { Status(std::string("task links: ") + kNotYet); return false; }
void CEditor::DrawMissions() {}
int CEditor::CreateJob(int, std::string& error) { error = kNotYet; return 0; }
int CEditor::CopyIntoJob(int, int, std::string& error) { error = kNotYet; return 0; }
void CEditor::BumpProxiesIn(int, int) {}
#endif

#if !EDFEAT_DIALOGUE
void CEditor::ReloadText()
{
    m_atext.clear();
    std::vector<uint8_t> ab;
    std::string error;
    if (!m_index.textPart.empty() && m_proj.readPart(m_index.textPart, ab)) ce::readTextStrings(ab, m_atext, m_textLanguage, error);
}
bool CEditor::FSetText(int, const std::string&, std::string& error) { return FNotYet(error); }
void CEditor::DrawDialogue() {}
bool CEditor::FParseDialogue(std::vector<DLGLINE>&, std::vector<std::string>&, std::string& error) const { return FNotYet(error); }
bool CEditor::FSetDialogueEvent(const DLGLINE&, int, int, int, int32_t, std::string& error) { return FNotYet(error); }
#endif

#if !EDFEAT_STRUCT
int CEditor::LevelGroup() const
{
    for (const ce::GroupInfo& g : m_index.groups)
        if (g.level) return g.index;
    return -1;
}
int CEditor::CopyGroup() const { return LevelGroup(); }
void CEditor::DrawCreate() {}
int CEditor::CopyRecord(int, int, const glm::vec3*, bool, std::string& error) { error = kNotYet; return 0; }
bool CEditor::FMakeUnique(LO*, std::string& error) { return FNotYet(error); }
bool CEditor::FDeleteNested(int, std::string& error) { return FNotYet(error); }
int CEditor::DuplicateNested(int, std::string& error) { error = kNotYet; return 0; }
#endif

#if !EDFEAT_CURVES
bool CEditor::FPathRead(const LO*, PATHDATA&) const { return false; }
bool CEditor::FPathLive(const LO*, PATHDATA&) const { return false; }
bool CEditor::FPathWrite(LO*, const PATHDATA&, const std::string&) { Status(std::string("curve: ") + kNotYet); return false; }
void CEditor::PathLive(LO*, const PATHDATA&) {}
glm::mat4 CEditor::MatPathSpace(const LO* plo) const { return MatWorld(plo); }
glm::vec3 CEditor::PathPivot(const LO*, const PATHDATA&) const { return glm::vec3(0.0f); }
void CEditor::DrawPaths() {}
void CEditor::DrawPathInspector(LO*) {}
bool CEditor::FSnapDown(const glm::vec3&, glm::vec3&) { return false; }
bool CEditor::FPathTransform(LO*, const glm::mat4&, const std::string&) { Status(std::string("curve: ") + kNotYet); return false; }
bool CEditor::FPathSnap(LO*, int, const std::string&) { Status(std::string("curve: ") + kNotYet); return false; }
void CEditor::ReapplyPathsFromParts() {}
#endif

#if !EDFEAT_SCRIPTS
void CEditor::DrawScripts() {}
void CEditor::LoadScriptText(int) {}
bool CEditor::FCheckScript(const std::string&, std::string& error, int& line, std::vector<PROBLEM>*, int)
{
    line = 0;
    return FNotYet(error);
}
bool CEditor::FApplyScript(int, const std::string&, std::string& error) { return FNotYet(error); }
int CEditor::AppendScriptModule(int, const std::string&, std::string& error) { error = kNotYet; return -1; }
#endif

#if !EDFEAT_MODELS
// ---- models and textures (Sly 3's glob and shader formats differ from Sly 2's)
void CEditor::PrintModelInfo(LO*) { Status(std::string("model: ") + kNotYet); }
bool CEditor::FImportModel(const std::string&, LO*, bool, std::string& error) { return FNotYet(error); }
bool CEditor::FImportTexture(const std::string&, LO*, int, int, std::string& error) { return FNotYet(error); }
bool CEditor::FSetMaterial(LO*, int, int, int, const uint32_t[3], int, std::string& error) { return FNotYet(error); }
bool CEditor::FExportGlb(LO*, int, const std::string&, std::string& error) { return FNotYet(error); }
bool CEditor::FPoseDump(LO*, int, float, bool, const std::string&, std::string& error) { return FNotYet(error); }
void CEditor::DrawModels() {}
void CEditor::DrawModelSection(LO*) {}
void CEditor::ApplyProjectTextures() {}
void CEditor::OpenFileDialog(const char*, const char*, const std::string&, std::function<void(const std::string&)>) { Status(std::string("files: ") + kNotYet); }
void CEditor::DrawFileDialogs() {}
void CEditor::ForgetThumbnails() {}

// ---- help (the shortcuts that apply here)
void CEditor::DrawHelp()
{
    if (!m_fShowHelp && !m_fShowKeys) return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.4f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool fOpen = true;
    if (ImGui::Begin("Getting started (Sly 3)", &fOpen, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::TextWrapped("Fly: hold the right mouse button and use W A S D, Q / E; Shift is faster, the wheel sets the speed.");
        ImGui::TextWrapped("Pick: click an object in the view or in the Scene list. W / E / R: move / rotate / scale with the gizmo, or type "
                           "in the Inspector. An instance of a shared prop moves as a whole (its proxy); Make unique turns it into the level's own.");
        ImGui::TextWrapped("The Inspector edits an object's options (by name where Sly 3's are known) and a curve's points; Duplicate / Delete "
                           "and the Create tab add and remove objects; Missions makes a new job from an existing one; Scripts edits a group's "
                           "splice scripts.");
        ImGui::TextWrapped("Ctrl+Z / Ctrl+Y undo / redo, Ctrl+S saves the project (a mod package for Sly 3), Project > Play test starts Sly 3 "
                           "in this world with it.");
        ImGui::TextColored(st::vec(st::muted), "Models and textures are Sly 2's only for now.");
        ImGui::Checkbox("Show at start", &m_fHelpAtStart);
    }
    ImGui::End();
    if (!fOpen) m_fShowHelp = m_fShowKeys = false;
}
#endif

}  // namespace edimpl

#endif
