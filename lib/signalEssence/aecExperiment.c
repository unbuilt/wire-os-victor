#include "aecExperiment.h"
#include "mmif.h"
#include "se_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int runtimeMode;
static int adaptationDiag = -1;
static int configuredTaps;
static int historyBlocks;
static int validHistoryBlocks;
static int blockPrepared;
static int preparedReferenceComplete;

// The supported vendor reference path has two 100 Hz DC-removal poles at
// nominal 16 kHz (pole < .961). Budget 960 samples for HPF settling before
// filling TD history. The linked vendor test verifies both homogeneous decay
// and sub-PCM16-sample driven rounding error using actual DC-removal code.
// The v009 16 kHz TD updater also has a seven-tap reference FIR, independent
// of the public channel-model length. Keep its six prior samples as well.
enum { ReferenceSettlingSamples = 960, ReferencePrefilterHistorySamples = 6 };

static void Fail(const char* reason)
{
    fprintf(stderr, "AEC_EXPERIMENT ERROR: %s\n", reason);
    abort();
}

int AnkiAecExperimentMode(void)
{
    const char* value = getenv("ANKI_AEC_EXPERIMENT");
    int mode = 0;
    if (value == NULL || strcmp(value, "off") == 0) return 0;
    if (strcmp(value, "reference") == 0) mode = 1;
    else if (strcmp(value, "on") == 0) mode = 2;
    else Fail("ANKI_AEC_EXPERIMENT must be off, reference, or on");
#if !ANKI_AEC_EXPERIMENT_SUPPORTED
    Fail("experiment requires the real VICOS sink and Signal Essence v009");
#endif
    return mode;
}

static int ReadMs(const char* name, int defaultValue, int maximum)
{
    const char* value = getenv(name);
    char* end = NULL;
    long parsed;
    if (value == NULL) return defaultValue;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno || end == value || *end || parsed < 0 || parsed > maximum) Fail(name);
    return (int)parsed;
}

int AnkiAecExperimentMicAgeMs(void) { return ReadMs("ANKI_AEC_MIC_AGE_MS", 10, 200); }
int AnkiAecExperimentDelayMs(void) { return ReadMs("ANKI_AEC_REF_DELAY_MS", 0, 200); }

void AnkiAecExperimentStart(void)
{
    const int mode = AnkiAecExperimentMode();
    runtimeMode = mode;
    adaptationDiag = -1;
    validHistoryBlocks = blockPrepared = preparedReferenceComplete = 0;
    if (!mode) return;
    if (MMIfGetNumChans(PORT_SEND_REFIN) != 1 ||
        MMIfGetSampleRateHz(PORT_SEND_REFIN) != 16000 ||
        MMIfGetBlockSize(PORT_SEND_REFIN) != 160 ||
        MMIfGetNumMicrophones() != 4 ||
        MMIfGetBlockSize(PORT_SEND_SIN) != 160) {
        Fail("unexpected MMIf reference/microphone format");
    }
    if (configuredTaps != 960 || MMIfGetAecLenChanModel() != configuredTaps)
        Fail("unsupported vendor AEC history length");
    historyBlocks = (ReferenceSettlingSamples + ReferencePrefilterHistorySamples + configuredTaps - 1 +
                     MMIfGetBlockSize(PORT_SEND_REFIN) - 1) /
                    MMIfGetBlockSize(PORT_SEND_REFIN);
    adaptationDiag = SEDiagGetIndex("mmfx_aec_adaptation_mode");
    if (adaptationDiag < 0 || SEDiagGetType(adaptationDiag) != SE_DIAG_ENUM ||
        SEDiagGetLen(adaptationDiag) != 1 || SEDiagGetRW(adaptationDiag) != SE_DIAG_RW) {
        Fail("vendor AEC adaptation control unavailable");
    }
    MMIfSetAecChanModelUpdateMode(SE_AF_DISABLE_ADAPTATION);
    MMIfSetAecBypass(1);
    if (!MMIfGetAecBypass() || !AnkiAecExperimentGetUpdateBypass())
        Fail("vendor AEC bypass readback failed");
    fprintf(stderr, "AEC_EXPERIMENT history_blocks=%d ref_settle_samples=%d prior_td_samples=%d prefilter_history=%d\n",
            historyBlocks, ReferenceSettlingSamples, configuredTaps - 1, ReferencePrefilterHistorySamples);
}

void AnkiAecExperimentPrepareBlock(int referenceComplete)
{
    int bypass;
    if (!runtimeMode) return; // Preserve the original off-mode configuration.
    if (blockPrepared) Fail("AEC block prepared without processing completion");
    blockPrepared = 1;
    preparedReferenceComplete = referenceComplete;
    if (!referenceComplete) validHistoryBlocks = 0;
    bypass = runtimeMode != 2 || !referenceComplete || validHistoryBlocks < historyBlocks;
    // The global cancel bypass explicitly does NOT stop channel-model updates.
    // Use the public update-mode API: it freezes adaptation across all microphones.
    MMIfSetAecChanModelUpdateMode(bypass ? SE_AF_DISABLE_ADAPTATION : SE_AF_NORMAL_ADAPTATION);
    MMIfSetAecBypass(bypass);
}

void AnkiAecExperimentCompleteBlock(void)
{
    if (!runtimeMode) return;
    if (!blockPrepared) Fail("AEC block completed without preparation");
    if (preparedReferenceComplete && validHistoryBlocks < historyBlocks)
        ++validHistoryBlocks;
    blockPrepared = 0;
}

int AnkiAecExperimentHistoryBlocksRemaining(void)
{
    return runtimeMode ? historyBlocks - validHistoryBlocks : 0;
}

int AnkiAecExperimentGetUpdateBypass(void)
{
    if (!runtimeMode) return 1;
    return SEDiagGetEnumAsInt(adaptationDiag) == SE_AF_DISABLE_ADAPTATION;
}

void AnkiAecExperimentConfigure(MMFxConfig_t* config)
{
    const int mode = AnkiAecExperimentMode();
    MultiAecConfig_t* aec;
    int mic;
    if (!mode) return;
    aec = MMFxGetMultiAecConfig(config);
    // Keep the vendor's implementation and callbacks together. The shipped default is
    // time-domain LMS with a nonzero 60 ms channel model, despite ConfigAec's zero argument.
    if (aec->MetaAecConfig.aecType != META_AEC_TD ||
        aec->MetaAecConfig.aecConfigUnion.td.lenChanModel != 960 ||
        aec->MetaAecConfig.aecConfigUnion.td.blockSize != 160 ||
        aec->MetaAecConfig.aecConfigUnion.td.sampleRate_Hz != 16000 ||
        aec->BlockSize != 160 || aec->SampleRateHz != 16000 ||
        config->RefProcConfig.blockSize != 160 ||
        config->RefProcConfig.sampleRateHz != 16000 ||
        config->RefProcConfig.numChans != 1 ||
        config->RefProcConfig.hpfCutoffHz != 100 ||
        config->RefProcConfig.numCutoffPoles != 2 ||
        config->RefProcConfig.numDelaySamples != 0) {
        Fail("unsupported vendor AEC reference filter/delay/history configuration");
    }
    configuredTaps = aec->MetaAecConfig.aecConfigUnion.td.lenChanModel;
    for (mic = 0; mic < config->NumMics; ++mic) {
        aec->pBypassCancelPerAec[mic] = (mode != 2);
        aec->pBypassUpdatePerAec[mic] = (mode != 2);
    }
    fprintf(stderr, "AEC_EXPERIMENT mode=%d type=TD taps=%d mics=%d cancel=%d adapt=%d mic_age_ms=%d ref_delay_ms=%d\n",
            mode, (int)aec->MetaAecConfig.aecConfigUnion.td.lenChanModel,
            (int)config->NumMics, mode == 2, mode == 2,
            AnkiAecExperimentMicAgeMs(), AnkiAecExperimentDelayMs());
}
