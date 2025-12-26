/*
wavproc01.c

Simple WAV processor: gain and low-pass filter
PCM 16-bit mono only

usage:

./wavproc01 gain in.wav out.wav 0.5
./wavproc01 lpf in.wav out.wav 1000


*/


#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// centralize fatal error handling
static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

// write a 16-bit unsigned value
// we could have done b[0] = (uint8_t)v, but this is less explicit
// so first mask, then cast! This makes intent very clear.
static void write_u16_le(FILE *f, uint16_t v) {
    // allocate a byte buffer
    // 0xFF means 1111 1111
    uint8_t b[2];
    // mask everything but the lowest 8 bits
    // result goes into byte 0 -- the little-endian least-significant byte first!
    b[0] = (uint8_t)(v & 0xFF);
    // extract the high byte by bitshifting
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    if (fwrite(b, 1, 2, f) != 2) die("write_u16_le: fwrite failed");
}

static void write_u32_le(FILE *f, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    if (fwrite(b, 1, 4, f) != 4) die("write_u32_le: fwrite failed");
}

static uint16_t read_u16_le(FILE *f) {
    uint8_t b[2];
    // be careful, if the number of read-in bytes is not exactly 2, we might have a malformed or truncated WAV file.
    if (fread(b, 1, 2, f) != 2) die("read_u16_le: fread failed");
    // b[0] is the LEAST significant byte because WAV files are little-endian. b[1] is shifted left to bits 8-15.
    // bitwise OR will combine something like
    // high:   1011 0101 0000 0000
    // low:    0000 0000 1001 1110
    // result: 1011 0101 1001 1110
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t read_u32_le(FILE *f) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) die("read_u32_le: fread failed");
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static float s16_to_float(int16_t s) {
    /* Map [-32768, 32767] to approximately [-1, 1). */
    if (s == -32768) return -1.0f;
    return (float)s / 32767.0f;
}

static int16_t float_to_s16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    // long round to integer (float version). Rounds a fp number to the nearest int.
    // returns a long int. Don't truncate, this will introduce distortion!
    // don't round to int16_t. Go to long int first, then to int16_t.
    long v = lrintf(x * 32767.0f);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

// wrap all the details of the WAV file into one typedef
// use long because ftell() used long.
typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_bytes;
    long     data_offset;
} wav_info_t;




/* Minimal WAV reader: PCM, 16-bit, mono. */
static wav_info_t read_wav_header(FILE *f) {
    // initialize all fields of the struct to 0, overwrite junk 
    wav_info_t info = {0};

    // 4-byte buffer that will store meaningful chunks in RIFF file
    // RIFF files have no terminator character. This will be exactly
    // 8 bits per element.
    // Each read will simply overwrite the previous contents of this.
    // It is a kind of "label reader". 
    uint8_t id[4];

    // fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
    // size is in bytes, nmemb = 4 (read 4 of them), return value = number of elements read (not bytes)
    // use memcmp, not strcmp. This is not a C string, not null-terminated in the file!
    if (fread(id, 1, 4, f) != 4) die("Not a file?");
    if (memcmp(id, "RIFF", 4) != 0) die("Not RIFF");

    (void)read_u32_le(f); /* riff_size */
    if (fread(id, 1, 4, f) != 4) die("Bad header");
    if (memcmp(id, "WAVE", 4) != 0) die("Not WAVE");

    int got_fmt = 0;
    int got_data = 0;

    while (!got_fmt || !got_data) {
        if (fread(id, 1, 4, f) != 4) die("Unexpected EOF in chunks");
        uint32_t chunk_size = read_u32_le(f);

        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t audio_format = read_u16_le(f);
            info.channels = read_u16_le(f);
            info.sample_rate = read_u32_le(f);
            (void)read_u32_le(f); /* byte_rate */
            (void)read_u16_le(f); /* block_align */
            info.bits_per_sample = read_u16_le(f);

            if (audio_format != 1) die("Only PCM supported");
            if (info.channels != 1) die("Only mono supported");
            if (info.bits_per_sample != 16) die("Only 16-bit supported");

            /* Skip any extra fmt bytes. */
            uint32_t consumed = 16;
            if (chunk_size > consumed) {
                if (fseek(f, (long)(chunk_size - consumed), SEEK_CUR) != 0) die("fseek failed");
            }
            got_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            info.data_bytes = chunk_size;
            info.data_offset = ftell(f);
            if (info.data_offset < 0) die("ftell failed");
            /* Do not skip data now; we will stream it. */
            got_data = 1;
        } else {
            /* Skip unknown chunk. (Chunks are word-aligned; many files pad to even.) */
            long skip = (long)chunk_size;
            if (skip & 1) skip += 1;
            if (fseek(f, skip, SEEK_CUR) != 0) die("fseek skip failed");
        }
    }

    return info;
}

static void write_wav_header_pcm16_mono(FILE *f, uint32_t sample_rate, uint32_t data_bytes) {
    uint16_t channels = 1;
    uint16_t bits_per_sample = 16;
    uint16_t block_align = (uint16_t)(channels * (bits_per_sample / 8));
    uint32_t byte_rate = sample_rate * (uint32_t)block_align;
    uint32_t riff_size = 36 + data_bytes;

    if (fwrite("RIFF", 1, 4, f) != 4) die("write RIFF failed");
    write_u32_le(f, riff_size);
    if (fwrite("WAVE", 1, 4, f) != 4) die("write WAVE failed");

    if (fwrite("fmt ", 1, 4, f) != 4) die("write fmt failed");
    write_u32_le(f, 16);
    write_u16_le(f, 1); /* PCM */
    write_u16_le(f, channels);
    write_u32_le(f, sample_rate);
    write_u32_le(f, byte_rate);
    write_u16_le(f, block_align);
    write_u16_le(f, bits_per_sample);

    if (fwrite("data", 1, 4, f) != 4) die("write data failed");
    write_u32_le(f, data_bytes);
}

static void usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  wavproc gain <in.wav> <out.wav> <gain>\n"
        "  wavproc lpf  <in.wav> <out.wav> <cutoff_hz>\n"
        "\n"
        "Notes: PCM 16-bit mono only.\n");
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 2) usage();

    const char *mode = argv[1];
    if (!(strcmp(mode, "gain") == 0 || strcmp(mode, "lpf") == 0)) usage();
    if (argc != 5) usage();

    const char *inpath  = argv[2];
    const char *outpath = argv[3];

    FILE *fin = fopen(inpath, "rb");
    if (!fin) die("Could not open input file");
    wav_info_t in = read_wav_header(fin);

    FILE *fout = fopen(outpath, "wb");
    if (!fout) die("Could not open output file");

    /* Same format out as in (PCM16 mono), same data length. */
    write_wav_header_pcm16_mono(fout, in.sample_rate, in.data_bytes);

    if (fseek(fin, in.data_offset, SEEK_SET) != 0) die("fseek to data failed");

    uint32_t total_samples = in.data_bytes / 2;

    if (strcmp(mode, "gain") == 0) {
        float g = (float)strtod(argv[4], NULL);

        for (uint32_t n = 0; n < total_samples; n++) {
            int16_t s = (int16_t)read_u16_le(fin); /* read as unsigned then reinterpret */
            float x = s16_to_float(s);
            x *= g;
            int16_t y = float_to_s16(x);
            write_u16_le(fout, (uint16_t)y);
        }
    } else if (strcmp(mode, "lpf") == 0) {
        double cutoff = strtod(argv[4], NULL);
        if (cutoff <= 0.0) die("cutoff_hz must be > 0");

        /* One-pole low-pass: y[n] = y[n-1] + a*(x[n] - y[n-1]) */
        const double two_pi = 2.0 * acos(-1.0);
        double dt = 1.0 / (double)in.sample_rate;
        double rc = 1.0 / (two_pi * cutoff);
        float a = (float)(dt / (rc + dt));

        float y1 = 0.0f;
        for (uint32_t n = 0; n < total_samples; n++) {
            int16_t s = (int16_t)read_u16_le(fin);
            float x = s16_to_float(s);
            y1 = y1 + a * (x - y1);
            int16_t y = float_to_s16(y1);
            write_u16_le(fout, (uint16_t)y);
        }
    }

    fclose(fin);
    fclose(fout);
    return 0;
}
