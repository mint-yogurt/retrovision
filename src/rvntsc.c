/* rvntsc: raw RGB24 frames in on stdin, NTSC-simulated frames out on stdout.
 *
 * Every input frame is the picture at one field's instant (59.94 per second); the first is field 1.
 *   --out=fields  one output frame per field (59.94p): the field's lines, with the lines between
 *                 them filled from their neighbours. For progressive screens and HDMI converters,
 *                 which make their own fields from successive frames.
 *   --out=weave   one output frame per field pair (29.97i, top field first): a true interlaced
 *                 frame, for interlaced encoding (DVD, hardware).
 */

#include "ntsc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(void)
{
    rv_params d;
    rv_defaults(&d);
    fprintf(stderr,
            "usage: rvntsc [options] < rgb24 frames > rgb24 frames\n"
            "  --width=N          samples per picture line (default %d; height is always %d)\n"
            "  --out=fields|weave 59.94p frames, or woven 29.97i frames (default fields)\n"
            "  --decoder=NAME     notch, comb1, comb2, svideo (default %s)\n"
            "  --luma-bw=MHZ      encoder luma band (%.1f)     --i-bw=MHZ (%.1f)  --q-bw=MHZ (%.1f)\n"
            "  --legalize=0|1     keep chroma within -20..120 IRE (%d)\n"
            "  --vfilter=F        vertical anti-twitter before field sampling, 0..1 (%.2f)\n"
            "  --notch-bw=MHZ     trap width (%.1f)    --chroma-bw=MHZ  demodulator low-pass (%.1f)\n"
            "  --sharpness=S      peaking (%.2f)   --hue=DEG (%.0f)   --saturation=S (%.1f)\n"
            "  --contrast=C (%.1f)   --brightness=B (%.1f)   --threads=N (0 = all CPUs)\n"
            "  --first-field=N    field index of the first input frame (default 0)\n"
            "  --stats            print speed to stderr at the end\n",
            RV_DEFAULT_WIDTH, RV_HEIGHT, rv_decoder_name(d.decoder), d.luma_bw_mhz, d.i_bw_mhz,
            d.q_bw_mhz, d.legalize, d.vfilter, d.notch_bw_mhz, d.chroma_bw_mhz, d.sharpness,
            d.hue_deg, d.saturation, d.contrast, d.brightness);
    exit(2);
}

static int opt(const char *arg, const char *name, const char **val)
{
    size_t l = strlen(name);
    if (strncmp(arg, name, l) || arg[l] != '=')
        return 0;
    *val = arg + l + 1;
    return 1;
}

static int read_full(void *buf, size_t len)
{
    size_t got = fread(buf, 1, len, stdin);
    if (got == len)
        return 1;
    if (got)
        fprintf(stderr, "rvntsc: ignoring a partial frame at the end (%zu of %zu bytes)\n", got, len);
    return 0;
}

int main(int argc, char **argv)
{
    rv_params p;
    rv_defaults(&p);
    int width = RV_DEFAULT_WIDTH, weave = 0, stats = 0;
    long field = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v;
        if (opt(a, "--width", &v)) width = atoi(v);
        else if (opt(a, "--out", &v)) {
            if (!strcmp(v, "weave")) weave = 1;
            else if (strcmp(v, "fields")) usage();
        } else if (opt(a, "--decoder", &v)) {
            int d = rv_decoder_from_name(v);
            if (d < 0) usage();
            p.decoder = (rv_decoder)d;
        }
        else if (opt(a, "--luma-bw", &v)) p.luma_bw_mhz = atof(v);
        else if (opt(a, "--i-bw", &v)) p.i_bw_mhz = atof(v);
        else if (opt(a, "--q-bw", &v)) p.q_bw_mhz = atof(v);
        else if (opt(a, "--legalize", &v)) p.legalize = atoi(v);
        else if (opt(a, "--vfilter", &v)) p.vfilter = atof(v);
        else if (opt(a, "--notch-bw", &v)) p.notch_bw_mhz = atof(v);
        else if (opt(a, "--chroma-bw", &v)) p.chroma_bw_mhz = atof(v);
        else if (opt(a, "--sharpness", &v)) p.sharpness = atof(v);
        else if (opt(a, "--hue", &v)) p.hue_deg = atof(v);
        else if (opt(a, "--saturation", &v)) p.saturation = atof(v);
        else if (opt(a, "--contrast", &v)) p.contrast = atof(v);
        else if (opt(a, "--brightness", &v)) p.brightness = atof(v);
        else if (opt(a, "--threads", &v)) p.threads = atoi(v);
        else if (opt(a, "--first-field", &v)) field = atol(v);
        else if (!strcmp(a, "--stats")) stats = 1;
        else usage();
    }

    rv_ntsc *n = rv_create(&p, width);
    if (!n) {
        fprintf(stderr, "rvntsc: bad settings (width must be 16-%d)\n", RV_MAX_WIDTH);
        return 2;
    }
    size_t row = (size_t)width * 3, frame = row * RV_HEIGHT;
    uint8_t *in = malloc(frame), *out = calloc(1, frame);
    if (!in || !out)
        return 1;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long fields = 0;
    while (read_full(in, frame)) {
        int parity = (int)(field & 1);
        rv_field(n, in, out, field);
        if (!weave) {
            /* the lines between this field's lines: a spot tall enough to leave no gaps, which is
             * how the field looks on a CRT whose scanlines aren't visible */
            for (int r = 1 - parity; r < RV_HEIGHT; r += 2) {
                const uint8_t *a = out + (size_t)(r > 0 ? r - 1 : r + 1) * row;
                const uint8_t *b = out + (size_t)(r < RV_HEIGHT - 1 ? r + 1 : r - 1) * row;
                uint8_t *o = out + (size_t)r * row;
                for (size_t x = 0; x < row; x++)
                    o[x] = (uint8_t)((a[x] + b[x] + 1) >> 1);
            }
        }
        if (!weave || parity == 1)
            if (fwrite(out, 1, frame, stdout) != frame)
                return 1;
        field++;
        fields++;
    }
    fflush(stdout);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (stats) {
        double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr, "rvntsc: %ld fields in %.2f s = %.1f fields/s (%.2fx real time at 59.94)\n",
                fields, s, fields / s, fields / s / 59.94);
    }
    rv_free(n);
    free(in);
    free(out);
    return 0;
}
