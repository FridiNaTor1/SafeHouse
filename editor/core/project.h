#pragma once
// An editor project: a mod package folder (platform/mod_package.h) whose files/ holds the changed world parts. The
// edits work on the parts' bytes in memory (each one undoable); Save writes them, Export copies the package (a folder,
// or a .zip of it) and Install puts it in a Sly 2 mods folder's Packages/.

#include "world.h"

#include "../../platform/mod_package.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cane::editor {

struct PartEdit {
    std::string rel;
    std::optional<std::vector<uint8_t>> before, after;   // nullopt: the part as installed (not in the project)
};

struct Step {
    std::string label;
    bool structural = false;   // objects added / removed (the 3D view loads the world again)
    std::vector<PartEdit> edits;
};

class Project {
public:
    fs::path dir;
    cane::mods::Manifest manifest;
    fs::path installDir;
    std::string game = "sly2";  // the installation's ("sly2", "sly3": its parts' record format)
    std::string historyError;   // (open: the saved history could not be used)

    // A new project in dir for an installation (its region and format in the manifest).
    bool create(const fs::path& dir, const std::string& name, const Installation& inst, std::string& error);
    bool open(const fs::path& dir, const Installation& inst, std::string& error);
    bool save(std::string& error);
    bool dirty() const { return m_dirty; }

    // The part's bytes: the project's (edited or saved), else the installation's.
    bool readPart(const std::string& rel, std::vector<uint8_t>& ab) const;
    bool partChanged(const std::string& rel) const { return m_parts.count(rel) > 0; }
    // The part's bytes without a copy (the project's, else the installation's, read once and kept); null: none.
    const std::vector<uint8_t>* partRef(const std::string& rel) const;
    std::vector<std::string> changedParts() const;

    // Edits (each an undo step; a step may group several, begin/end)
    void begin(const std::string& label, bool structural = false);
    void end();
    bool patch(const std::string& rel, std::size_t ib, const void* pv, std::size_t cb);
    bool setPart(const std::string& rel, std::vector<uint8_t> ab);
    // Top-level object records of an Objects part: removed, or copied with a new oid (appended; *pibNew its offset).
    bool deleteRecord(const std::string& rel, std::size_t ibRecord, std::string& error);
    bool duplicateRecord(const std::string& rel, std::size_t ibRecord, int oidNew, std::size_t* pibNew, std::string& error);
    // Bytes [ib, ib + cbOld) of a part replaced by ab; each S32 at aibSize (the enclosing records' cb fields, before ib)
    // grows by the size change.
    bool spliceBytes(const std::string& rel, std::size_t ib, std::size_t cbOld, const std::vector<uint8_t>& ab,
                     const std::vector<std::size_t>& aibSize);
    // A whole record (S8 k ... cb bytes) appended to an Objects part (*pibNew: its offset).
    bool appendRecord(const std::string& rel, const std::vector<uint8_t>& rec, std::size_t* pibNew, std::string& error);
    // The part back to the installation's.
    bool revertPart(const std::string& rel);

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    const Step* undo();   // the step undone (null: none)
    const Step* redo();
    const std::vector<Step>& history() const { return m_undo; }
    const std::vector<Step>& redoHistory() const { return m_redo; }
    // The undo / redo history kept in the project (editor/history.bin; written at save, read at open). Each edit is
    // stored as the changed byte range of its part (before / after), checked against the part's bytes when read.
    bool saveHistory(std::string& error) const;
    bool loadHistory(std::string& error);

    // The package as a folder (dest/<id>/) or a .zip (dest.zip, mod.json at its root), and into a mods folder.
    bool exportFolder(const fs::path& dest, std::string& error) const;
    bool exportZip(const fs::path& zip, std::string& error) const;
    bool install(const fs::path& modsDir, std::string& error) const;
    // the project's prefabs/ files (name, bytes): exported and installed with the package
    std::vector<std::pair<std::string, std::vector<uint8_t>>> prefabFiles() const;

private:
    void apply(const PartEdit& e, bool forward);
    std::map<std::string, std::vector<uint8_t>> m_parts;   // the project's parts (rel relative to the installation)
    std::vector<Step> m_undo, m_redo;
    std::optional<Step> m_open;
    int m_depth = 0;
    bool m_dirty = false;
    std::vector<std::string> m_removed;   // parts reverted since the last save (their files deleted at save)
    mutable std::map<std::string, std::vector<uint8_t>> m_installed;   // (partRef: installed parts read)
};

// The world parts a project changes, as the manifest's "worlds" (index path + the installed world's hash).
nlohmann::json worldsOfParts(const Installation& inst, const std::vector<std::string>& parts);

}  // namespace cane::editor
