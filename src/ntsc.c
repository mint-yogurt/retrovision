/* retrovision: NTSC composite video, simulated literally. See ntsc.h. */

#include "ntsc.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PI 3.14159265358979323846
#define FS_MHZ (315.0 / 88.0 * 4.0) /* 4fsc: 14.31818 MHz */
#define SPL RV_SAMPLES_PER_LINE

/* Line timing from the leading edge of horizontal sync, in 4fsc samples (SMPTE 170M). */
enum {
    SYNC_END = 67,                       /* 4.7 us */
    BURST_START = 76, BURST_LEN = 36,    /* 5.3 us, 9 cycles */
    BURST_READ_START = 80, BURST_READ_LEN = 28, /* the steady middle 7 cycles, read by the TV */
    ACTIVE_START = 135, ACTIVE_END = 889, /* 9.4 us to 62.06 us: 52.66 us of picture */
    ACTIVE_CENTRE = 512,
    EDGE = 3                             /* samples of rise/fall on sync and blanking edges */
};

/* Field 1 (top) carries picture rows 0, 2, 4... on lines 23-262; field 2 carries rows 1, 3, 5...
 * on lines 286-525. Line 286 is drawn between lines 23 and 24, so the rows interleave. */
enum { FIELD1_FIRST = 23, FIELD2_FIRST = 286, FIELD_ROWS = RV_HEIGHT / 2 };
/* Lines generated per field: the 240 picture lines plus two blank ones either side for the comb. */
enum { PAD = 2, GEN_LINES = FIELD_ROWS + 2 * PAD };

enum { IRE_SYNC = -40 };
static const float IRE_SETUP = 7.5f, IRE_SPAN = 92.5f, IRE_BURST = 20.0f;

/* ---- filters ---- */

typedef struct { float b0, b1, b2, a1, a2; } biquad;
typedef struct { biquad s[4]; int n; int delay; } filter;

/* Group delay at DC, in samples, which is what a matched delay line in a real set compensates. */
static double biquad_dc_delay(const biquad *q)
{
    return (q->b1 + 2.0 * q->b2) / (q->b0 + q->b1 + q->b2) - (q->a1 + 2.0 * q->a2) / (1.0 + q->a1 + q->a2);
}

static void filter_finish(filter *f)
{
    double d = 0;
    for (int i = 0; i < f->n; i++)
        d += biquad_dc_delay(&f->s[i]);
    f->delay = (int)lround(d);
}

/* Butterworth low-pass by the bilinear transform. Order is rounded up to even. It is causal like an
 * analogue filter (ringing after an edge, not before) and has a zero at fs/2, so it removes the
 * 2fsc product of demodulation exactly. */
static void butter_lowpass(filter *f, double fc_mhz, int order)
{
    int pairs = (order + 1) / 2;
    double k = tan(PI * fc_mhz / FS_MHZ);
    f->n = pairs;
    for (int i = 0; i < pairs; i++) {
        double q = 1.0 / (2.0 * sin((2 * i + 1) * PI / (4.0 * pairs)));
        double norm = 1.0 / (1.0 + k / q + k * k);
        biquad *s = &f->s[i];
        s->b0 = (float)(k * k * norm);
        s->b1 = 2 * s->b0;
        s->b2 = s->b0;
        s->a1 = (float)(2.0 * (k * k - 1.0) * norm);
        s->a2 = (float)((1.0 - k / q + k * k) * norm);
    }
    filter_finish(f);
}

/* Trap at fsc = fs/4: zeros at z = +-j, poles at +-jr. bw_mhz is its -3 dB width. Unity at DC. */
static void notch_fsc(filter *f, double bw_mhz)
{
    /* first-order allpass notch in z^-2; its -3 dB width in z^-2 is twice the width in z */
    double t = tan(2.0 * PI * bw_mhz / FS_MHZ);
    double r2 = (1.0 - t) / (1.0 + t);
    if (r2 < 0)
        r2 = 0;
    f->n = 1;
    f->s[0] = (biquad){ (float)((1 + r2) / 2), 0, (float)((1 + r2) / 2), 0, (float)r2 };
    filter_finish(f);
}

/* Eight lines at once, one per vector lane: each lane does filter_run's exact operations in the
 * same order (no contraction under -std=c99), so the output is bit for bit the same. */
#define LANES 8
typedef float v8 __attribute__((vector_size(32)));

static void filter_run8(const filter *f, v8 *buf, int len)
{
    static __thread v8 tmp[SPL + 8];
    const v8 *src = buf;
    for (int i = 0; i < f->n; i++) {
        const biquad *q = &f->s[i];
        v8 x1 = src[0], x2 = src[0];
        float dc = (q->b0 + q->b1 + q->b2) / (1 + q->a1 + q->a2);
        v8 y1 = src[0] * dc, y2 = y1;
        for (int n = 0; n < len; n++) {
            v8 x = src[n];
            v8 y = q->b0 * x + q->b1 * x1 + q->b2 * x2 - q->a1 * y1 - q->a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            tmp[n] = y;
        }
        src = tmp;
    }
    int d = f->delay;
    for (int n = 0; n < len; n++)
        buf[n] = tmp[n + d < len ? n + d : len - 1];
}

/* ---- the simulator ---- */

typedef struct {
    float dsin[4], dcos[4]; /* decoder reference, per subcarrier phase, hue and gain folded in */
} line_ref;

struct rv_ntsc {
    rv_params p;
    int width, x0;
    filter enc_y, enc_i, enc_q, notch, dec_c;
    float c33[4], s33[4], bsin[4];   /* encoder carrier per phase: cos/sin(theta + 33 deg), burst */
    float edge_ramp[EDGE];
    float w_sync[SPL], w_burst[SPL], w_act[SPL];   /* window() per sample, worked out once */
    float *comp, *luma, *chroma;     /* GEN_LINES x SPL each */
    line_ref ref[GEN_LINES];
    double burst_re, burst_im;       /* the set's colour reference, averaged across lines */
    int locked;

    /* per-call state for the workers */
    const uint8_t *src;
    uint8_t *dst;
    long frame;
    int parity;
};

void rv_defaults(rv_params *p)
{
    p->luma_bw_mhz = 4.2;
    p->i_bw_mhz = 1.3;
    p->q_bw_mhz = 0.6;
    p->legalize = 1;
    p->vfilter = 0.5;
    p->decoder = RV_DECODER_NOTCH;
    p->notch_bw_mhz = 1.5;
    p->chroma_bw_mhz = 0.6;
    p->sharpness = 0.3;
    p->hue_deg = 0;
    p->saturation = 1;
    p->contrast = 1;
    p->brightness = 0;
    p->threads = 0;
}

static const char *const decoder_names[RV_DECODER_COUNT] = { "notch", "comb1", "comb2", "svideo" };

const char *rv_decoder_name(rv_decoder d)
{
    return (unsigned)d < RV_DECODER_COUNT ? decoder_names[d] : "?";
}

int rv_decoder_from_name(const char *name)
{
    for (int i = 0; i < RV_DECODER_COUNT; i++)
        if (!strcmp(name, decoder_names[i]))
            return i;
    return -1;
}

void rv_free(rv_ntsc *n)
{
    if (!n)
        return;
    free(n->comp);
    free(n->luma);
    free(n->chroma);
    free(n);
}

/* Subcarrier phase (0-3, quarter cycles) of sample x on absolute line a. 910 = 2 mod 4, so the phase
 * steps half a cycle per line, and 525 lines step it half a cycle per frame. */
static inline int phase_of(long long a, int x)
{
    return (int)((a * 2 + x) & 3);
}

static long long abs_line(const rv_ntsc *n, int k)
{
    int first = n->parity ? FIELD2_FIRST : FIELD1_FIRST;
    return n->frame * RV_LINES_PER_FRAME + (first + k - 1);
}

/* Weight of the blanked/unblanked window at sample x: 0 outside [start, end), 1 inside, with a short
 * raised-cosine rise and fall like a real blanking switch. */
static inline float window(const rv_ntsc *n, int x, int start, int end)
{
    if (x < start || x >= end)
        return 0;
    if (x < start + EDGE)
        return n->edge_ramp[x - start];
    if (x >= end - EDGE)
        return n->edge_ramp[end - 1 - x];
    return 1;
}

rv_ntsc *rv_create(const rv_params *p, int width)
{
    if (width < 16 || width > RV_MAX_WIDTH || (unsigned)p->decoder >= RV_DECODER_COUNT)
        return NULL;
    rv_ntsc *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->p = *p;
    n->width = width;
    n->x0 = ACTIVE_CENTRE - width / 2;
    butter_lowpass(&n->enc_y, p->luma_bw_mhz, 4);
    butter_lowpass(&n->enc_i, p->i_bw_mhz, 2);
    butter_lowpass(&n->enc_q, p->q_bw_mhz, 2);
    notch_fsc(&n->notch, p->notch_bw_mhz);
    butter_lowpass(&n->dec_c, p->chroma_bw_mhz, 2);
    for (int q = 0; q < 4; q++) {
        double th = q * PI / 2;
        n->c33[q] = (float)cos(th + 33 * PI / 180);
        n->s33[q] = (float)sin(th + 33 * PI / 180);
        n->bsin[q] = (float)(-IRE_BURST * sin(th)); /* burst sits at 180 degrees on the B-Y axis */
    }
    for (int i = 0; i < EDGE; i++)
        n->edge_ramp[i] = (float)(0.5 - 0.5 * cos(PI * (i + 1) / (EDGE + 1)));
    for (int x = 0; x < SPL; x++) {
        n->w_sync[x] = window(n, x, 0, SYNC_END);
        n->w_burst[x] = window(n, x, BURST_START, BURST_START + BURST_LEN);
        n->w_act[x] = window(n, x, ACTIVE_START, ACTIVE_END);
    }
    size_t sz = (size_t)GEN_LINES * SPL;
    n->comp = malloc(sz * sizeof(float));
    n->luma = malloc(sz * sizeof(float));
    n->chroma = malloc(sz * sizeof(float));
    if (!n->comp || !n->luma || !n->chroma) {
        rv_free(n);
        return NULL;
    }
    return n;
}

static void encode_fill(rv_ntsc *n, int gi, float *y, float *i, float *q)
{
    int k = gi - PAD;
    int row = 2 * k + n->parity;
    memset(y, 0, SPL * sizeof *y);
    memset(i, 0, SPL * sizeof *i);
    memset(q, 0, SPL * sizeof *q);

    if (k >= 0 && k < FIELD_ROWS) {
        /* the field takes its line from the frame, softened vertically like a camera's field
         * integration so fine horizontal detail doesn't twitter at 30 Hz */
        int up = row > 0 ? row - 1 : row, dn = row < RV_HEIGHT - 1 ? row + 1 : row;
        float wc = (float)(1.0 - n->p.vfilter), wn = (float)(n->p.vfilter / 2);
        const uint8_t *r0 = n->src + (size_t)row * n->width * 3;
        const uint8_t *ru = n->src + (size_t)up * n->width * 3;
        const uint8_t *rd = n->src + (size_t)dn * n->width * 3;
        for (int x = 0; x < n->width; x++) {
            float c[3];
            for (int ch = 0; ch < 3; ch++)
                c[ch] = (wc * r0[x * 3 + ch] + wn * (ru[x * 3 + ch] + rd[x * 3 + ch])) * (1.0f / 255);
            int s = n->x0 + x;
            y[s] = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
            i[s] = 0.5959f * c[0] - 0.2746f * c[1] - 0.3213f * c[2];
            q[s] = 0.2115f * c[0] - 0.5227f * c[1] + 0.3112f * c[2];
        }
    }
}

static void lanes_in(v8 *dst, const float *src, int lane)
{
    for (int x = 0; x < SPL; x++)
        dst[x][lane] = src[x];
}

typedef int v8i __attribute__((vector_size(32)));

/* a where m is set, else b (m from a vector comparison: all ones or all zeros per lane) */
static inline v8 sel(v8i m, v8 a, v8 b)
{
    return (v8)(((v8i)a & m) | ((v8i)b & ~m));
}

/* Per lane, a 4-entry table by subcarrier phase: out[m][l] = tab[(2a_l + m) & 3], so that
 * out[x & 3] is phase_of(a_l, x)'s entry for every lane at sample x. */
static void phase_table(const long long *a, const float *tab, size_t stride, v8 out[4])
{
    for (int m = 0; m < 4; m++)
        for (int l = 0; l < LANES; l++)
            out[m][l] = *(const float *)((const char *)tab + stride * (size_t)l + sizeof(float) * (size_t)((a[l] * 2 + m) & 3));
}

static void encode_group(rv_ntsc *n, int g)
{
    static __thread v8 Y[SPL], I[SPL], Q[SPL];
    float y[SPL], i[SPL], q[SPL];
    int first = g * LANES, count = GEN_LINES - first < LANES ? GEN_LINES - first : LANES;
    long long a[LANES] = { 0 };
    if (count < LANES) {
        memset(Y, 0, sizeof Y);
        memset(I, 0, sizeof I);
        memset(Q, 0, sizeof Q);
    }
    for (int l = 0; l < count; l++) {
        encode_fill(n, first + l, y, i, q);
        lanes_in(Y, y, l);
        lanes_in(I, i, l);
        lanes_in(Q, q, l);
        a[l] = abs_line(n, first + l - PAD);
    }
    filter_run8(&n->enc_y, Y, SPL);
    filter_run8(&n->enc_i, I, SPL);
    filter_run8(&n->enc_q, Q, SPL);

    v8 c33[4], s33[4], bs[4];
    phase_table(a, n->c33, 0, c33);
    phase_table(a, n->s33, 0, s33);
    phase_table(a, n->bsin, 0, bs);
    float *comp[LANES], *luma[LANES], *chroma[LANES];
    for (int l = 0; l < count; l++) {
        comp[l] = n->comp + (size_t)(first + l) * SPL;
        luma[l] = n->luma + (size_t)(first + l) * SPL;
        chroma[l] = n->chroma + (size_t)(first + l) * SPL;
    }
    const float hi = (120.0f - IRE_SETUP) / IRE_SPAN, lo = (-20.0f - IRE_SETUP) / IRE_SPAN;
    const v8 zero = { 0 }, one = zero + 1;
    for (int x = 0; x < SPL; x++) {
        int m = x & 3;
        float sync = IRE_SYNC * n->w_sync[x];
        v8 burst = bs[m] * n->w_burst[x];
        float act = n->w_act[x];
        v8 yy = Y[x], ii = I[x], qq = Q[x];
        if (n->p.legalize) {
            /* a broadcast legaliser turns chroma down where the composite peaks would leave
             * -20..120 IRE, rather than clipping (which would shift the hue) */
            v8 amp = zero;
            for (int l = 0; l < LANES; l++)
                amp[l] = sqrtf(ii[l] * ii[l] + qq[l] * qq[l]);
            v8 g1 = sel(yy + amp > hi, sel(yy < hi, (hi - yy) / amp, zero), one);
            v8 g2 = sel(yy > lo, (yy - lo) / amp, zero);
            v8 g = sel(yy - amp < lo, sel(g2 < g1, g2, g1), g1);
            ii *= g;
            qq *= g;
        }
        v8 lu = sync + act * (IRE_SETUP + IRE_SPAN * yy);
        v8 ch = burst + act * IRE_SPAN * (ii * c33[m] + qq * s33[m]);
        v8 co = lu + ch;
        for (int l = 0; l < count; l++) {
            luma[l][x] = lu[l];
            chroma[l][x] = ch[l];
            comp[l][x] = co[l];
        }
    }
}

/* The set's colour oscillator: correlate each line's burst against the free-running 3.58 MHz
 * reference and average across lines, as the colour PLL does. Gives the demodulation phase (plus
 * the tint knob) and the ACC gain (burst amplitude normalised to 20 IRE). */
static void burst_lock(rv_ntsc *n)
{
    for (int gi = 0; gi < GEN_LINES; gi++) {
        long long a = abs_line(n, gi - PAD);
        const float *c = n->p.decoder == RV_DECODER_SVIDEO ? n->chroma + (size_t)gi * SPL
                                                            : n->comp + (size_t)gi * SPL;
        double re = 0, im = 0;
        for (int x = BURST_READ_START; x < BURST_READ_START + BURST_READ_LEN; x++) {
            int ph = phase_of(a, x);
            static const int cs[4] = { 1, 0, -1, 0 }, sn[4] = { 0, 1, 0, -1 };
            re += c[x] * cs[ph];
            im += c[x] * sn[ph];
        }
        if (!n->locked) {
            n->burst_re = re;
            n->burst_im = im;
            n->locked = 1;
        } else {
            n->burst_re += 0.1 * (re - n->burst_re);
            n->burst_im += 0.1 * (im - n->burst_im);
        }
        /* burst = -20 sin(theta + phi): re = -10N sin(phi), im = -10N cos(phi) */
        double phi = atan2(-n->burst_re, -n->burst_im) + n->p.hue_deg * PI / 180;
        double amp = 2 * hypot(n->burst_re, n->burst_im) / BURST_READ_LEN;
        double gain = amp > 1e-3 ? IRE_BURST / amp : 0; /* no burst: the colour killer */
        gain *= 2.0 * n->p.saturation * n->p.contrast / IRE_SPAN;
        for (int ph = 0; ph < 4; ph++) {
            double th = ph * PI / 2 + phi;
            n->ref[gi].dsin[ph] = (float)(sin(th) * gain);
            n->ref[gi].dcos[ph] = (float)(cos(th) * gain);
        }
    }
}

static inline v8i to_u8v(v8 v)
{
    const v8 zero = { 0 }, top = zero + 255;
    v = v * 255.0f + 0.5f;
    v = sel(v < 0, zero, v);
    v = sel(v > 255, top, v);
    return __builtin_convertvector(v, v8i);
}

static void decode_group(rv_ntsc *n, int g)
{
    static __thread v8 C[SPL], P[SPL], N[SPL], F[SPL], Yv[SPL], U[SPL], V[SPL];
    int first = g * LANES, count = FIELD_ROWS - first < LANES ? FIELD_ROWS - first : LANES;
    rv_decoder dec = n->p.decoder;
    long long a[LANES] = { 0 };
    if (count < LANES) {
        memset(C, 0, sizeof C);
        memset(P, 0, sizeof P);
        memset(N, 0, sizeof N);
    }
    for (int l = 0; l < count; l++) {
        int gi = first + l + PAD;
        a[l] = abs_line(n, first + l);
        if (dec == RV_DECODER_SVIDEO) {
            lanes_in(P, n->luma + (size_t)gi * SPL, l);     /* S-Video: P is Y, N is C */
            lanes_in(N, n->chroma + (size_t)gi * SPL, l);
            continue;
        }
        lanes_in(C, n->comp + (size_t)gi * SPL, l);
        if (dec != RV_DECODER_NOTCH) {
            lanes_in(P, n->comp + (size_t)(gi - 1) * SPL, l);
            lanes_in(N, n->comp + (size_t)(gi + 1) * SPL, l);
        }
    }
    v8 ds[4], dc[4];
    phase_table(a, n->ref[first + PAD].dsin, sizeof(line_ref), ds);
    phase_table(a, n->ref[first + PAD].dcos, sizeof(line_ref), dc);

    switch (dec) {
    case RV_DECODER_NOTCH:
        memcpy(F, C, sizeof F);
        filter_run8(&n->notch, F, SPL);
        for (int x = 0; x < SPL; x++) {
            v8 c = C[x] - F[x];
            Yv[x] = F[x];
            U[x] = c * ds[x & 3];
            V[x] = c * dc[x & 3];
        }
        break;
    case RV_DECODER_COMB1:
    case RV_DECODER_COMB2:
        /* lines a field apart carry the subcarrier in opposite phase: their difference is chroma
         * (plus any vertical luma detail, which is what leaves hanging dots), band-passed around
         * fsc as the complement of the trap */
        for (int x = 0; x < SPL; x++)
            F[x] = dec == RV_DECODER_COMB1 ? 0.5f * (C[x] - P[x]) : 0.25f * (2 * C[x] - P[x] - N[x]);
        memcpy(U, F, sizeof U);   /* the comb's difference, kept */
        filter_run8(&n->notch, F, SPL);
        for (int x = 0; x < SPL; x++) {
            v8 c = U[x] - F[x];
            Yv[x] = C[x] - c;
            U[x] = c * ds[x & 3];
            V[x] = c * dc[x & 3];
        }
        break;
    default: /* S-Video */
        for (int x = 0; x < SPL; x++) {
            Yv[x] = P[x];
            U[x] = N[x] * ds[x & 3];
            V[x] = N[x] * dc[x & 3];
        }
        break;
    }
    filter_run8(&n->dec_c, U, SPL);
    filter_run8(&n->dec_c, V, SPL);

    /* peaking: a symmetric delay-line aperture corrector, 1 + s(1 - cos 3w), strongest at fs/6 */
    float s = (float)n->p.sharpness, ct = (float)n->p.contrast, br = (float)n->p.brightness;
    uint8_t *out[LANES];
    for (int l = 0; l < count; l++)
        out[l] = n->dst + (size_t)(2 * (first + l) + n->parity) * n->width * 3;
    for (int x = 0; x < n->width; x++) {
        int sx = n->x0 + x;
        v8 yy = Yv[sx] + s * (Yv[sx] - 0.5f * (Yv[sx - 3] + Yv[sx + 3]));
        yy = (yy - IRE_SETUP) / IRE_SPAN * ct + br;
        v8 uu = U[sx], vv = V[sx];
        v8i r = to_u8v(yy + 1.13983f * vv);
        v8i gr = to_u8v(yy - 0.39465f * uu - 0.58060f * vv);
        v8i b = to_u8v(yy + 2.03211f * uu);
        for (int l = 0; l < count; l++) {
            out[l][x * 3 + 0] = (uint8_t)r[l];
            out[l][x * 3 + 1] = (uint8_t)gr[l];
            out[l][x * 3 + 2] = (uint8_t)b[l];
        }
    }
}

/* ---- threads ---- */

typedef struct {
    rv_ntsc *n;
    void (*fn)(rv_ntsc *, int);
    int from, to;
} job;

static void *job_run(void *arg)
{
    job *j = arg;
    for (int i = j->from; i < j->to; i++)
        j->fn(j->n, i);
    return NULL;
}

static void parallel(rv_ntsc *n, void (*fn)(rv_ntsc *, int), int count)
{
    int t = n->p.threads;
    if (t <= 0) {
        long c = sysconf(_SC_NPROCESSORS_ONLN);
        t = c > 0 ? (int)c : 1;
    }
    if (t > 32)
        t = 32;
    if (t == 1) {
        for (int i = 0; i < count; i++)
            fn(n, i);
        return;
    }
    pthread_t th[32];
    job jobs[32];
    for (int w = 0; w < t; w++) {
        jobs[w] = (job){ n, fn, count * w / t, count * (w + 1) / t };
        if (pthread_create(&th[w], NULL, job_run, &jobs[w]))
            job_run(&jobs[w]), th[w] = 0;
    }
    for (int w = 0; w < t; w++)
        if (th[w])
            pthread_join(th[w], NULL);
}

void rv_field(rv_ntsc *n, const uint8_t *src, uint8_t *dst, long field_index)
{
    n->src = src;
    n->dst = dst;
    n->frame = field_index / 2;
    n->parity = (int)(field_index & 1);
    parallel(n, encode_group, (GEN_LINES + LANES - 1) / LANES);
    burst_lock(n);
    parallel(n, decode_group, (FIELD_ROWS + LANES - 1) / LANES);
}
