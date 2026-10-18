#!/usr/bin/env sh
set -e

echo "Compiling Pomodoro TUI..."
gcc -O2 -Wall -Wextra -pedantic -std=c99 pomodoro.c -lm -o pomodoro

if [ ! -f "sounds/bell.wav" ]; then
    echo "Creating default sound asset (sounds/bell.wav)..."
    mkdir -p sounds
    gcc -O2 -std=c99 -lm -x c - -o /tmp/gen_bell << 'EOF'
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#define SAMPLE_RATE 44100
int main() {
    FILE *f = fopen("sounds/bell.wav", "wb");
    if (!f) return 1;
    double duration = 1.6;
    int num_samples = (int)(SAMPLE_RATE * duration);
    int data_size = num_samples * sizeof(int16_t);
    fwrite("RIFF", 1, 4, f);
    uint32_t chunk_size = 36 + data_size;
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t subchunk1_size = 16, sample_rate = SAMPLE_RATE, byte_rate = SAMPLE_RATE * 2;
    uint16_t audio_format = 1, num_channels = 1, block_align = 2, bits_per_sample = 16;
    fwrite(&subchunk1_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    for (int i = 0; i < num_samples; i++) {
        double t = (double)i / SAMPLE_RATE;
        double env1 = exp(-t * 3.2);
        double s1 = (0.6 * sin(2.0 * M_PI * 783.99 * t) +
                     0.25 * sin(2.0 * M_PI * 1567.98 * t) +
                     0.1 * sin(2.0 * M_PI * 2351.97 * t)) * env1;
        double s2 = 0.0;
        if (t > 0.22) {
            double t2 = t - 0.22;
            double env2 = exp(-t2 * 2.8);
            s2 = (0.7 * sin(2.0 * M_PI * 1046.50 * t2) +
                  0.25 * sin(2.0 * M_PI * 2093.00 * t2) +
                  0.1 * sin(2.0 * M_PI * 3139.50 * t2)) * env2;
        }
        double sample = s1 + s2;
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        int16_t val = (int16_t)(sample * 30000.0);
        fwrite(&val, 2, 1, f);
    }
    fclose(f);
    return 0;
}
EOF
    /tmp/gen_bell && rm -f /tmp/gen_bell
fi

echo "Build complete! Run './pomodoro' or './pomodoro -s /path/to/sound.wav'"
