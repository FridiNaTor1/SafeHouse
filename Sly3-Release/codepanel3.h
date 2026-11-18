#pragma once
// codepanel3.h: Sly 3's code-entry panel (class 0x50) and its dials (class 0x79); see codepanel3.cpp.
struct LO;
struct SW;

namespace sly3
{
void ActivateCodePanel3(LO* ploPanel, LO* ploActivator);   // orig3 0x1716F8 (SCPT 0x12 on a 0x50 object, the method 247)
void PostLoadCodePanel3(SW* psw, LO* plo);                          // (update.cpp, every world's object starts)
bool FCodePanelCharacter3(const LO* plo);                  // the panel is a player's character (its mode 2)
void ReleaseCodePanel3(LO* plo);                            // orig3 0x171188's 0x171538(p, -1) (the in-place reset)
}  // namespace sly3
