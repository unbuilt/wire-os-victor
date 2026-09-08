// Cross-link with the production signal_essence and vendor mmfx archives, then run
// under qemu-arm or on a robot. This is digital smoke coverage, not an acoustic test.
#include "aecExperiment.h"
#include "mmif.h"
#include "se_diag.h"
#include "dcremove_f32.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { Taps = 960, Mics = 4, HistoryBlocks = 13 };

static void CheckReferenceSettling(void)
{
    int signs, sample, pattern;
    float worst = 0, worstDriven = 0;
    // Bound all four independent state errors of the actual two cascaded
    // vendor poles, generously above any difference between PCM16 histories.
    for (signs = 0; signs < 16; ++signs) {
        DCRmvF32_t first, second;
        InitDcRemovalFilter_f32(&first, 100, 16);
        InitDcRemovalFilter_f32(&second, 100, 16);
        first.State = (signs & 1 ? 1 : -1) * 1048576.0f;
        first.XPrevState = (signs & 2 ? 1 : -1) * 1048576.0f;
        second.State = (signs & 4 ? 1 : -1) * 1048576.0f;
        second.XPrevState = (signs & 8 ? 1 : -1) * 1048576.0f;
        for (sample = 0; sample < 1920; ++sample) {
            float zero = 0, intermediate, output;
            DcRemovalFilter_f32(&first, &zero, &intermediate, 1);
            DcRemovalFilter_f32(&second, &intermediate, &output, 1);
            if (sample >= 959) {
                if (fabsf(output) > worst) worst = fabsf(output);
                if (!isfinite(output) || fabsf(output) >= 1e-6f) abort();
            }
        }
    }
    printf("reference_hpf_settling samples=960 state_error_bound=1048576 worst_pcm_error=%g PASS\n", worst);
    // Also compare full-scale driven histories, where floating-point rounding
    // does not obey exact linear superposition. Keep residual below one PCM16
    // quantization step, not a claim of bit-identical infinite IIR histories.
    for (pattern = 0; pattern < 3; ++pattern) {
        for (signs = 0; signs < 16; ++signs) {
            DCRmvF32_t first, second, cleanFirst, cleanSecond;
            uint32_t random = 1;
            InitDcRemovalFilter_f32(&first, 100, 16);
            InitDcRemovalFilter_f32(&second, 100, 16);
            InitDcRemovalFilter_f32(&cleanFirst, 100, 16);
            InitDcRemovalFilter_f32(&cleanSecond, 100, 16);
            first.State = (signs & 1 ? 1 : -1) * 1048576.0f;
            first.XPrevState = (signs & 2 ? 1 : -1) * 1048576.0f;
            second.State = (signs & 4 ? 1 : -1) * 1048576.0f;
            second.XPrevState = (signs & 8 ? 1 : -1) * 1048576.0f;
            for (sample = 0; sample < 4096; ++sample) {
                float input, intermediate, output, cleanIntermediate, cleanOutput, error;
                random = random * 1664525u + 1013904223u;
                input = pattern == 0 ? 32767 : pattern == 1 ?
                    (sample % 2 ? -32768 : 32767) : (int)(random >> 16) - 32768;
                DcRemovalFilter_f32(&first, &input, &intermediate, 1);
                DcRemovalFilter_f32(&second, &intermediate, &output, 1);
                DcRemovalFilter_f32(&cleanFirst, &input, &cleanIntermediate, 1);
                DcRemovalFilter_f32(&cleanSecond, &cleanIntermediate, &cleanOutput, 1);
                error = fabsf(output - cleanOutput);
                if (sample >= 959) {
                    if (error > worstDriven) worstDriven = error;
                    if (!isfinite(output) || error >= 1.0f) abort();
                }
            }
        }
    }
    printf("reference_hpf_driven_settling samples=960 patterns=DC,alternating,random worst_pcm_error=%g PASS\n",
           worstDriven);
}

static void Snapshot(int16_t models[Mics][2][Taps])
{
    int mic;
    for (mic = 0; mic < Mics; ++mic) {
        char name[64];
        int index;
        memcpy(models[mic][0], MMIfGetAecChanModelCoef(mic), sizeof(models[mic][0]));
        snprintf(name, sizeof(name), "multiaec_filter_coefs_backup_%02d", mic);
        index = SEDiagGetIndex(name);
        if (index < 0 || SEDiagGetLen(index) != Taps) abort();
        memcpy(models[mic][1], SEDiagGetInt16Array(index), sizeof(models[mic][1]));
    }
}

int main(int argc, char** argv)
{
    int16_t reference[160], microphones[640], output[160];
    int16_t history[1024] = {0};
    uint32_t random = 1;
    int block, sample, mic, cursor = 0, completedValid = 0;
    int mode = AnkiAecExperimentMode();
    int16_t trained[Mics][2][Taps], current[Mics][2][Taps];
    int16_t previous[Mics][2][Taps] = {{{0}}};
    const int cancelOnlyControl = argc == 2 && strcmp(argv[1], "--cancel-only-control") == 0;
    const int noHistoryControl = argc == 2 && strcmp(argv[1], "--no-history-control") == 0;
    unsigned changed = 0, resumedMics = 0, refillFrozen = 0;
    CheckReferenceSettling();
    MMIfInit(argc == 2 && strcmp(argv[1], "--unsupported-ref-delay") == 0 ? 0.01f : 0, NULL);
    AnkiAecExperimentStart();
    printf("smoke mode=%d bypass=%u channel_taps=%u\n", mode,
           MMIfGetAecBypass(), MMIfGetAecLenChanModel());
    if (MMIfGetAecLenChanModel() != Taps || (mode && !MMIfGetAecBypass())) return 2;
    if (mode && AnkiAecExperimentHistoryBlocksRemaining() != HistoryBlocks) return 7;
    for (block = 0; block < 1200; ++block) {
        // Long gaps, a transient gap, interrupted refill, then fully covered but
        // timing-invalid input. All reset the same production history guard.
        const int incomplete = (block >= 500 && block < 700) || block == 900 || block == 906;
        const int timingValid = !(block >= 930 && block < 933);
        const int valid = !incomplete && timingValid;
        int expectedBypass;
        if (!valid) completedValid = 0;
        expectedBypass = mode != 2 || !valid || completedValid < HistoryBlocks;
        for (sample = 0; sample < 160; ++sample) {
            random = random * 1664525u + 1013904223u;
            reference[sample] = (int16_t)((int)(random >> 20) - 2048);
            history[cursor] = reference[sample];
            for (mic = 0; mic < 4; ++mic) {
                microphones[mic * 160 + sample] = history[(cursor + 1024 - 32 - mic) % 1024] / 2;
                // A final second of near-end energy exercises double-talk without
                // claiming intelligibility or a validated double-talk detector.
                if (block >= 400 && block < 500) microphones[mic * 160 + sample] += (sample % 32 < 16 ? 500 : -500);
            }
            cursor = (cursor + 1) % 1024;
        }
        if (incomplete) {
            // Keep most reference samples nonzero initially, then remove all.
            memset(reference, 0, (block < 600 ? 1 : 160) * sizeof(reference[0]));
        }
        AnkiAecExperimentPrepareBlock(valid);
        if (mode && (MMIfGetAecBypass() != expectedBypass ||
                     AnkiAecExperimentGetUpdateBypass() != expectedBypass ||
                     AnkiAecExperimentHistoryBlocksRemaining() != HistoryBlocks - completedValid))
            return 4;
        if (cancelOnlyControl && incomplete) {
            MMIfSetAecChanModelUpdateMode(SE_AF_NORMAL_ADAPTATION);
            MMIfSetAecBypass(1);
        }
        if (noHistoryControl && block >= 700 && valid) {
            MMIfSetAecChanModelUpdateMode(SE_AF_NORMAL_ADAPTATION);
            MMIfSetAecBypass(0);
        }
        MMIfProcessMicrophones(reference, microphones, output);
        AnkiAecExperimentCompleteBlock();
        if (valid && completedValid < HistoryBlocks) ++completedValid;
        if (mode && AnkiAecExperimentHistoryBlocksRemaining() != HistoryBlocks - completedValid)
            return 7;
        // Vendor coefficient diagnostics are mirrors refreshed only on cancellation
        // (AecTdPublicObject_t.enableCopyWeightedFilterOnCancelRW). A frozen mirror
        // alone is not proof of frozen filters. Refresh it with adaptation disabled.
        // This extra diagnostic pass is NOT credited as a production block;
        // it only makes the guard more conservative than the vendor history.
        if (mode == 2) {
            MMIfSetAecChanModelUpdateMode(SE_AF_DISABLE_ADAPTATION);
            MMIfSetAecBypass(0);
            MMIfProcessMicrophones(reference, microphones, output);
        }
        Snapshot(current);
        if (block == 499) {
            memcpy(trained, current, sizeof(trained));
            for (mic = 0; mic < Mics; ++mic) {
                unsigned nonzero = 0;
                for (sample = 0; sample < Taps; ++sample)
                    nonzero += trained[mic][0][sample] != 0;
                printf("trained mic=%d nonzero_filter_taps=%u\n", mic, nonzero);
                if ((mode == 2) != (nonzero != 0)) return 3;
                changed += nonzero;
            }
        }
        if (expectedBypass && memcmp(previous, current, sizeof(previous))) {
            fprintf(stderr, "ERROR: trained coefficients changed while frozen block=%d valid=%d history_remaining=%d\n",
                    block, valid, HistoryBlocks - completedValid);
            return 5;
        }
        if (block >= 500 && mode == 2 && expectedBypass && valid) ++refillFrozen;
        if (block == 713 || block == 920 || block == 946) {
            if (mode == 2 && expectedBypass) return 8;
            printf("history_refilled block=%d prior_valid_blocks=%d cancel_adapt=%d\n",
                   block, completedValid, !expectedBypass);
        }
        if (mode != 2) {
            for (mic = 0; mic < Mics; ++mic)
                for (sample = 0; sample < Taps; ++sample)
                    if (current[mic][0][sample] || current[mic][1][sample]) return 6;
        }
        memcpy(previous, current, sizeof(previous));
    }
    for (mic = 0; mic < Mics; ++mic)
        resumedMics += memcmp(trained[mic], current[mic], sizeof(trained[mic])) != 0;
    printf("gap_freeze blocks=202 timing_invalid=3 covered_refill_frozen=%u active_and_backup_mics=4 resumed_mics=%u\n",
           refillFrozen, resumedMics);
    if (mode == 2 && refillFrozen != 44) return 9;
    MMIfDestroy();
    if ((mode == 2 && (changed == 0 || resumedMics != Mics)) || (mode != 2 && changed != 0)) {
        fprintf(stderr, "ERROR: AEC filter adaptation did not match requested mode\n");
        return 3;
    }
    return 0;
}
