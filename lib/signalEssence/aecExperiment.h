#pragma once

#include "mmfxpub.h"
#ifdef __cplusplus
extern "C" {
#endif
// Startup-only controls. Invalid/unsupported opt-in is fatal, never silently "enabled".
int AnkiAecExperimentMode(void); // 0=off, 1=reference only, 2=cancel+adapt
int AnkiAecExperimentMicAgeMs(void);
int AnkiAecExperimentDelayMs(void);
void AnkiAecExperimentConfigure(MMFxConfig_t* config);
void AnkiAecExperimentStart(void);
// After Start, pair Prepare/Complete around each MMIfProcessMicrophones call on
// its thread. Complete means fully covered reference AND healthy/ready timing.
// Invalid blocks reset history; the current block never counts as prior history.
void AnkiAecExperimentPrepareBlock(int referenceComplete);
void AnkiAecExperimentCompleteBlock(void);
int AnkiAecExperimentHistoryBlocksRemaining(void);
// Read the vendor adaptation control on that same thread (not the cancellation bypass).
int AnkiAecExperimentGetUpdateBypass(void);
#ifdef __cplusplus
}
#endif
