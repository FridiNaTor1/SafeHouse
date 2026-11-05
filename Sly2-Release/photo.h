#pragma once
#include <string>

// Native photo viewer helper (original Sly +0x1D40, 0x201D78..0x203790).
struct ACTOR;
void* PostLoadPhotoPrompt(ACTOR* pa);
int NPhotoPromptState(const void* pv);
bool FPhotoCapturePending();  // render the title frame needed by the forthcoming two-picture capture
void SetPhotoPromptState(void* pv, int state);
void UpdatePhotoPrompt(void* pv);
std::string StrPhotoScript(int id);
