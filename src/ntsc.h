/* retrovision: NTSC composite video, simulated literally.
 *
 * A picture is encoded into a composite signal sampled at 4fsc (14.318 MHz, 910 samples per
 * line), the way a broadcast encoder would: Y'IQ from gamma-encoded R'G'B', band limits in MHz,
 * 7.5 IRE setup, sync, colour burst. Then a TV decodes it: burst lock, Y/C separation (notch,
 * comb or S-Video), demodulation, peaking, back to R'G'B'.
 *
 * The subcarrier phase comes from each sample's absolute position in a continuous 525-line,
 * 910-sample-per-line stream, so the 180 degree flip from line to line, the frame-to-frame flip
 * and the four-field dot crawl sequence all fall out of the arithmetic rather than being drawn.
 *
 * One call processes one field. The picture handed in is the whole 480-line frame as it looks at
 * that field's instant; only that field's 240 lines are taken from it.
 */

#ifndef RV_NTSC_H
#define RV_NTSC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RV_SAMPLES_PER_LINE = 910, RV_LINES_PER_FRAME = 525, RV_HEIGHT = 480 };

/* Samples the picture may span: the active line is samples 135-888 (52.66 us). 764 samples is
 * Rec. 601's 720 pixels (53.33 us), whose 4:3 picture is about 747 samples wide. */
enum { RV_DEFAULT_WIDTH = 764, RV_MAX_WIDTH = 780 };

typedef enum {
    RV_DECODER_NOTCH,   /* trap filter at 3.58 MHz: a 70s/80s set; most dot crawl and rainbows */
    RV_DECODER_COMB1,   /* one-line (1H) comb: late-80s sets; hanging dots on colour edges */
    RV_DECODER_COMB2,   /* two-line (3-tap) comb: 90s sets */
    RV_DECODER_SVIDEO,  /* separate Y and C: no crosstalk, only the bandwidth limits */
    RV_DECODER_COUNT
} rv_decoder;

typedef struct {
    /* the broadcast encoder */
    double luma_bw_mhz;    /* 4.2 */
    double i_bw_mhz;       /* 1.3 */
    double q_bw_mhz;       /* 0.6 */
    int legalize;          /* 1: scale chroma so the signal stays within -20..120 IRE */
    double vfilter;        /* 0..1: vertical low-pass before taking a field's lines (anti-twitter) */

    /* the TV */
    rv_decoder decoder;
    double notch_bw_mhz;   /* width of the 3.58 MHz trap (and of the comb's chroma band) */
    double chroma_bw_mhz;  /* low-pass after demodulation; consumer sets were about 0.6 */
    double sharpness;      /* luma peaking around 2.4 MHz; 0 = none */
    double hue_deg;        /* tint knob */
    double saturation;     /* colour knob, 1 = nominal */
    double contrast;       /* picture knob, 1 = nominal */
    double brightness;     /* added to Y', 0 = nominal */

    int threads;           /* 0 = one per CPU */
} rv_params;

void rv_defaults(rv_params *p);
const char *rv_decoder_name(rv_decoder d);
int rv_decoder_from_name(const char *name); /* -1 if unknown */

typedef struct rv_ntsc rv_ntsc;

/* width = samples the picture spans, centred on the active line (RV_DEFAULT_WIDTH for 720-pixel
 * Rec. 601 lines). Height is always RV_HEIGHT. Returns NULL on bad arguments. */
rv_ntsc *rv_create(const rv_params *p, int width);
void rv_free(rv_ntsc *n);

/* Encode and decode one field. field_index counts fields from 0; even fields are field 1, the top
 * field (even picture rows), odd fields are field 2 (odd rows). Consecutive calls must pass
 * consecutive indices for the burst lock to behave like a real set's.
 * src, dst: packed RGB24, width x RV_HEIGHT. Only this field's rows of dst are written. */
void rv_field(rv_ntsc *n, const uint8_t *src, uint8_t *dst, long field_index);

#ifdef __cplusplus
}
#endif

#endif
