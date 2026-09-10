// Replay Vector's actual SVad on mono PCM16LE, 16 kHz. No robot-state overrides.
#include "svad.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
    FILE* input;
    SVadConfig_t config;
    SVadObject_t vad;
    unsigned char bytes[320];
    int16_t samples[160];
    size_t length, i, blocks = 0;
    if (argc != 2) {
        fprintf(stderr, "usage: vector-vad-replay MONO_16000HZ_PCM16LE.raw\n");
        return 2;
    }
    input = fopen(argv[1], "rb");
    if (!input) { perror(argv[1]); return 1; }
    SVadSetDefaultConfig(&config, 160, 16000);
    SVadInit(&vad, &config);
    while ((length = fread(bytes, 1, sizeof(bytes), input)) != 0) {
        if (length % 2 != 0) {
            fprintf(stderr, "Partial PCM16 sample\n");
            fclose(input);
            return 1;
        }
        memset(samples, 0, sizeof(samples));
        for (i = 0; i < length / 2; ++i) {
            const unsigned value = bytes[i * 2] | ((unsigned)bytes[i * 2 + 1] << 8);
            samples[i] = (int16_t)(value < 32768 ? (int)value : (int)value - 65536);
        }
        printf("%d\n", DoSVad(&vad, 1.0f, samples) != 0);
        ++blocks;
    }
    if (ferror(input) || blocks == 0) {
        fprintf(stderr, "Empty or unreadable PCM input\n");
        fclose(input);
        return 1;
    }
    if (fclose(input) != 0 || fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "VAD replay I/O failed\n");
        return 1;
    }
    return 0;
}
