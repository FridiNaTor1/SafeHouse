// When an object exists (the Properties panel's "When it exists" section; notes/EDITOR_PLAN.md "Hooks for script
// names"). This version lists the stored facts as they are: the record's world filters (the header's list: the object
// is loaded only in those world variants), the options that add or remove it at the load, and the tasks / scripts that
// name it. The plain-words version (which scripts show or hide it, in which missions it is there) replaces this body.
namespace edimpl
{

void CEditor::DrawExistence(LO* plo, const EDOBJ* po)
{
    (void)po;
    if (!plo) return;
    LO* ptop = PloTopLevel(plo);
    std::vector<std::string> alines;
    // the record header's filters (Sly 2: S8 k, then k S16 world filters; Sly 3: its mode and conditions)
    if (ptop)
        if (const auto it = m_mpobj.find(ptop); it != m_mpobj.end() && !it->second.relRecord.empty())
            if (const std::vector<uint8_t>* pab = m_proj.partRef(it->second.relRecord); pab && it->second.ibRecordFile < pab->size())
            {
                const uint8_t* pb = pab->data() + it->second.ibRecordFile;
                const int k = int8_t(pb[0]);
#if EDGAME_SLY3
                if (k == 1 && it->second.ibRecordFile + 3 <= pab->size())
                    alines.push_back(std::string(T("Record conditions")) + ": " + std::to_string(pb[1]) + " " + T("filters") + ", " + std::to_string(pb[2]) + " " + T("conditions"));
#else
                if (k > 0 && it->second.ibRecordFile + 1 + std::size_t(k) * 2 <= pab->size())
                {
                    std::string s;
                    for (int i = 0; i < k; ++i)
                    {
                        int16_t f;
                        std::memcpy(&f, pb + 1 + i * 2, 2);
                        s += (i ? ", " : "") + std::to_string(f);
                    }
                    alines.push_back(std::string(T("Loaded only in the world variants")) + ": " + s);
                }
                else if (k < 0) alines.push_back(T("A shared group's object: there wherever an instance of its group is placed"));
#endif
            }
    int n = 0;
    if (FOptionInt(plo, EDEOPID(25), n) && n) alines.push_back(T("Added to the world at the load (option 25)"));
    if (FOptionInt(plo, EDEOPID(26), n) && n) alines.push_back(T("Removed from the world at the start (option 26)"));
    if (alines.empty()) alines.push_back(T("Always there (no filters or load options; scripts may still add or remove it)"));
    for (const std::string& s : alines) ImGui::BulletText("%s", s.c_str());
}

}  // namespace edimpl
