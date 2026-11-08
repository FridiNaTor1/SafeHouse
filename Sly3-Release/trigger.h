#pragma once

struct LO;

// Native representation of the shared embedded state's notification fields.
// Messages 6/7 carry this state after their enter/leave actions (0x1487A0 / 0x148860).
struct TRIGGERSTATE
{
    LO* owner = nullptr;
    int state = -1;
    float tState = 0.0f;
    LO* ploFound = nullptr;
};

// orig: 0x001488D8: the authored animation list of an embedded trigger state.
// List 0 enters, list 1 leaves; shared by touch pads and volume triggers.
void RunTriggerAsegList(LO* plo, int i);

// orig: 0x001488D8 -> 0x0020FF70: the state-machine goals follow the ASEG list.
void RunTriggerRsmgList(LO* plo, int i);

// Complete shared action sequence: ASEGs, state machines, then enter script/tasks.
void RunEmbeddedTriggerList(LO* plo, int i, LO* found);

// orig: 0x001A6730 copies the embedded RSMG/task action arrays with the object.
void CloneEmbeddedTriggerActions(const LO* source, LO* target);
