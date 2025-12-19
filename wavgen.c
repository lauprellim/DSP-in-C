/*
Simple WAV generator (16-bit PCM, mono).

Build:
  cc -O2 -Wall -Wextra -std=c11 wavgen.c -lm -o wavgen

Examples:
  ./wavgen sine out.wav 44100 2.0 440 0.8
  ./wavgen noise out.wav 48000 3.0 0 0.4
  ./wavgen impulse out.wav 44100 1.0 0 0.9
  ./wavgen silence out.wav 44100 2.0 0 0
  ./wavgen chirp out.wav 44100 3.0 200 0.8 2000

Args:
  mode out.wav sample_rate seconds f1 amplitude [f2]

Modes:
  sine     : f1 = frequency (Hz)
  noise    : f1 ignored
  impulse  : f1 ignored (impulse at sample 0)
  silence  : amplitude ignored
  chirp    : f1 = start Hz, f2 = end Hz (required)
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static void write_u16_le(FILE *f, uint16_t v) {
    uint8_t b[2];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    fwrite(b, 1, 2, f);
}

static void write_u32_le(FILE *f, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    fwrite(b, 1, 4, f);
}

static int16_t float_to_s16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    int32_t v = (int32_t)lrintf(x * 32767.0f);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static float frand_uniform(void) {
    return (float)rand() / (float)RAND_MAX; /* [0,1] */
}

static void write_wav_header(FILE *f, uint32_t sample_rate, uint32_t num_samples) {
    uint16_t num_channels = 1;
    uint16_t bits_per_sample = 16;
    uint16_t audio_format = 1; /* PCM */
    uint16_t block_align = (uint16_t)(num_channels * (bits_per_sample / 8));
    uint32_t byte_rate = sample_rate * block_align;
    uint32_t data_bytes = num_samples * block_align;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    write_u32_le(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    write_u32_le(f, 16);
    write_u16_le(f, audio_format);
    write_u16_le(f, num_channels);
    write_u32_le(f, sample_rate);
    write_u32_le(f, byte_rate);
    write_u16_le(f, block_align);
    write_u16_le(f, bits_per_sample);

    /* data chunk */
    fwrite("data", 1, 4, f);
    write_u32_le(f, data_bytes);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s mode out.wav sample_rate seconds f1 amplitude [f2]\n"
        "Modes: sine, noise, impulse, silence, chirp\n"
        "Examples:\n"
        "  %s sine out.wav 44100 2.0 440 0.8\n"
        "  %s noise out.wav 48000 3.0 0 0.4\n"
        "  %s chirp out.wav 44100 3.0 200 0.8 2000\n",
        prog, prog, prog, prog
    );
}

int main(int argc, char **argv) {
    if (argc < 7) {
        usage(argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *outpath = argv[2];
    uint32_t sample_rate = (uint32_t)strtoul(argv[3], NULL, 10);
    double seconds = strtod(argv[4], NULL);
    double f1 = strtod(argv[5], NULL);
    double amp = strtod(argv[6], NULL);
    double f2 = 0.0;

    if (sample_rate < 8000 || sample_rate > 192000 || seconds <= 0.0) {
        fprintf(stderr, "Invalid sample_rate or seconds.\n");
        return 1;
    }

    if (!strcmp(mode, "chirp")) {
        if (argc < 8) {
            fprintf(stderr, "chirp mode requires f2.\n");
            return 1;
        }
        f2 = strtod(argv[7], NULL);
        if (f1 <= 0.0 || f2 <= 0.0) {
            fprintf(stderr, "chirp frequencies must be > 0.\n");
            return 1;
        }
    }

    uint32_t num_samples = (uint32_t)llround(seconds * (double)sample_rate);
    if (num_samples == 0) {
        fprintf(stderr, "Duration too short.\n");
        return 1;
    }

    FILE *f = fopen(outpath, "wb");
    if (!f) {
        perror("fopen");
        return 1;
    }

    write_wav_header(f, sample_rate, num_samples);

    srand((unsigned)time(NULL));

    double phase = 0.0;
    double two_pi = 2.0 * acos(-1.0);  // M_PI unavailable?

    for (uint32_t n = 0; n < num_samples; n++) {
        float x = 0.0f;

        if (!strcmp(mode, "sine")) {
            double inc = two_pi * f1 / (double)sample_rate;
            x = (float)(amp * sin(phase));
            phase += inc;
            if (phase >= two_pi) phase -= two_pi;
        } else if (!strcmp(mode, "noise")) {
            /* white noise in [-1,1] */
            float r = 2.0f * frand_uniform() - 1.0f;
            x = (float)(amp * r);
        } else if (!strcmp(mode, "impulse")) {
            x = (n == 0) ? (float)amp : 0.0f;
        } else if (!strcmp(mode, "silence")) {
            x = 0.0f;
        } else if (!strcmp(mode, "chirp")) {
            /* linear chirp in frequency over time: f(t) = f1 + (f2-f1)*t/T */
            double t = (double)n / (double)sample_rate;
            double T = seconds;
            double ft = f1 + (f2 - f1) * (t / T);
            double inc = two_pi * ft / (double)sample_rate;
            x = (float)(amp * sin(phase));
            phase += inc;
            if (phase >= two_pi) phase -= two_pi;
        } else {
            fprintf(stderr, "Unknown mode: %s\n", mode);
            fclose(f);
            return 1;
        }

        int16_t s = float_to_s16(x);
        write_u16_le(f, (uint16_t)s);
    }

    fclose(f);
    return 0;
}
