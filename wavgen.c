/*
Simple WAV generator (16-bit PCM, mono).

Build:
  gcc -O2 -Wall -Wextra -std=c11 wavgen.c -lm -o wavgen

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

// suppose v = 0x1234;
// we want to write b[0] = 0x34 and b[1] = 0x12
// so it is little-endian.

static void write_u16_le(FILE *f, uint16_t v) {
  // v is a 16-bit unsigned integer. we want to split
  // it into two 8-bit bytes.
    uint8_t b[2];
    // keep only the lowest 8 bits! This is the least significant byte,
    // written in little-endian.
    b[0] = (uint8_t)(v & 0xFF);
    // shift bytes right by 8 places
    // this moves high byte down to low byte position
    // & 0xFF again keeps only lowest 8 bits
    // this produces the MOST significant byte
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    // write 2 bytes to the disk.
    // specifically, b = pointer to the first byte of the array
    // 1 = size ofeach element (1 byte)
    // 2 = number of elements (2)
    // f = file stream
    // do it this way instead of
    // fwrite(&v, sizeof(v), 1, f);
    // we need little-endian for WAV. This is less portable.
    fwrite(b, 1, 2, f);
}

// same idea as write_u16_le(), but v is 32 bits
// example v = 0x11223344, write 44 33 22 11.
static void write_u32_le(FILE *f, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    fwrite(b, 1, 4, f);
}

// convert normalized floating-point sample to a
// 16-bit PCM sample.
// We have to:
//  Ensure float is in range;
//  Scale it;
//  Round it correctly;
//  Clip it safely;
//  Return a valid int16_t.
// Highly "defensive" programming paradigm...
static int16_t float_to_s16(float x) {
    // input range enforcement: hard clipping
    // remember int16_t ranges from -32768 to +32767
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    // lrintf() rounds to nearest integer. Don't cast
    // because that will truncate, which will add bias
    // and distortion.
    // we use int32_t here so we can clip safetly before
    // narrowing to 16 bits.
    int32_t v = (int32_t)lrintf(x * 32767.0f);
    // final safety clipping, handles edge cases
    // guarentees v will be in [-32768, +32767].
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    // now finally cast, it is safe to do so.
    return (int16_t)v;
}

// produces random number between 0.0 and 1.0. This will be mapped
// to [-1.0, 1.0] in the actual loop. [0,] is more general and can
// be used in other ways.
static float frand_uniform(void) {
    return (float)rand() / (float)RAND_MAX; /* [0,1] */
}


// RIFF stores integersin little-endian byte order so
// the least significant byte goes first.

static void write_wav_header(FILE *f, uint32_t sample_rate, uint32_t num_samples) {
    uint16_t num_channels = 1;
    uint16_t bits_per_sample = 16;
    uint16_t audio_format = 1; /* PCM */
    uint16_t block_align = (uint16_t)(num_channels * (bits_per_sample / 8));
    uint32_t byte_rate = sample_rate * block_align;
    // number of samples must be known in advance of writing the actual samples.
    uint32_t data_bytes = num_samples * block_align;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    write_u32_le(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk -- multiple 16- and 32- byte fields! */
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

// similiar to public static void main(String[] args) { ... }
// argc = number of valid entries in argv. Here, 7:
// argv is an array of null-terminated strings
// argv[0] = "./wavgen"
// argv[1] = "sine"...
// argv[i] = always a string

int main(int argc, char **argv) {
  // "defensive" programming...
  if (argc < 7) {
        usage(argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *outpath = argv[2];
    // convert sample rate to an unsigned long number, NULL termination, base 10
    // int can vary in size across platforms so we use uint32_t to be explicit
    // strtoul() returns an unsigned long, we need to cast as uint32_t.
    // unsigned long is frequently 64 bits!
    // uint32_t is a typedef defined in <stdint.h>
    // better than atoi().
    uint32_t sample_rate = (uint32_t)strtoul(argv[3], NULL, 10);
    double seconds = strtod(argv[4], NULL);
    // convert string to double. No need to specify base as
    // floating-point strings are decimal by definition...
    // better than atof().
    // f1 = start frequency (Hz).
    double f1 = strtod(argv[5], NULL);
    double amp = strtod(argv[6], NULL);
    // f2 = end frequency (Hz). This is only used  in chirp mode and
    // is initialized here so the variable isn't left undefind. If we
    // want to be in chirp mode, the user-inputted parameter gets
    // assigned to f2 in the chirp() function.
    double f2 = 0.0;

    if (sample_rate < 8000 || sample_rate > 192000 || seconds <= 0.0) {
        fprintf(stderr, "Invalid sample_rate or seconds.\n");
        return 1;
    }

    // this would be if (mode.equals("chirp)) { ... } in java.
    // "chirp" is a pointer and == compares addresses, not contents.
    // int strcmp( const char *s1, const char *s2 ); compares two C strings.
    // returns 0 if the strings are equal. It returns and int, not a bool.
    // this is called "lexicographic comparison".
    // this is the standard way to branch in string values!
    // switch (mode) { ... } only works with ints, enums, characters.
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

    // we round to a long long number. Need a integer number of samples.
    // can't have floating point. Casting will truncate, llround() rounds.
    // moreover if you trucate, WAV header may be wrong, buffer length may
    // mismatch, or file may contain an audible click at the end!
    // long long is at least 64 bits. We narrow it to 32 bits.
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

    // WAV file header. The canonical header for a WAV file is 44 bytes
    // for 16-bit PCM mono/stereo. We need:
    // RIFF identifier ("RIFF")
    // Total file size (minus 8 bytes)
    // WAVE identifier ("WAVE")
    // fmtchunk:
    //  Audio format (PCM = 1)
    //  Number of channels (1 = mono)
    //  Sample rate
    //  Byte rate
    //  Block alignment
    //  Bits per sample (16)
    // Data chunk header ("data")
    // Size of the sample data in bytes
    //
    // the fourCCs (RIFF, WAVE, fmt, data) are written as ASCII. RIFF
    // stands for Resource Interchange File Format...
    // Raw PCM has no metadata, isn't self-describing.
    write_wav_header(f, sample_rate, num_samples);

    // seeds C's pseudo-random number generator with the current time,
    // so that calls to rand() produce a different random sequence,
    // each time the program runs. Not a "high quality" noise generator!
    // --- a "stochastic" signal, as opposed to a deterministic one...
    // call srand() only once, not multiple times!
    srand((unsigned)time(NULL));

    // set phase STATE, not a constant. Use double, not float.
    // this will reduce long-term drift. This is for the internal oscillator
    // state, which must be kept precise, as phase accumulates over many
    // samples.
    double phase = 0.0;
    const double two_pi = 2.0 * acos(-1.0);   // use acos(-1.0) instead of M_PI


    // this is a bit wasteful, we are doing string comparisons every time
    // the loop iterates. We could define a typedef enum ( MODE_SINE, ... } mode_t;
    // instead, and switch inside the loop.
    
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
