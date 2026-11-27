# Research notes (2026-11-12)

These are the sources read before writing retrovision, and what each one taught us.

## blargg's snes_ntsc / nes_ntsc

LGPL C89. Source: <http://slack.net/~ant/libs/ntsc.html>; the zips are on archive.org now.

- **Superposition.** Everything from R'G'B' to decoded R'G'B' is linear for a fixed burst phase: the encoder, its filters, demodulation and the matrix. Gamma and the final clamp are the only non-linear steps.
  - `snes_ntsc_init` therefore runs every palette colour through the full model once, at every burst phase and pixel alignment.
  - It stores each colour's contribution to its neighbours as packed RGB: three 10-bit fields in one 32-bit int, with guard bits (`snes_ntsc_rgb_builder`).
  - Blitting is then about six table lookups, adds, and a branch-free bit-trick clamp (`SNES_NTSC_CLAMP_`) per output pixel. That was 8% of a 2 GHz Athlon at 60 fps.
- **"Artifacts" and "fringing".** The signal is encoded into two composite signals so that chroma-to-luma artifacts and luma-to-chroma fringing can be scaled separately. This algorithm is credited to NewRisingSun.
- **Filters.** Luma uses a sinc kernel with roll-off (sharpness), under a Blackman window. Chroma uses a Gaussian kernel whose even and odd phases are normalised separately. There is an 8:7 rescale for the SNES pixel aspect.
- **Limits for our use.**
  - The table is indexed by palette colour (15-bit), so 24-bit video would need per-channel tables. Linearity makes that possible: out = Σ T_R[r] + T_G[g] + T_B[b].
  - The carrier is tied to console pixel clocks (3 burst phases), not broadcast timing (2 phases per line, 227.5 cycles).
  - `merge_fields` averages phases to hide dot crawl, which is the opposite of what we want.

## NTSC-CRT (LMP88959)

C89, integer-only, free licence.

- **The most literal of the existing code:**
  - 910 samples per line at 4fsc.
  - Real sync, burst, blanking and vsync.
  - A decoder that integrates the signal to find sync and locks hue from the burst with a leaky accumulator.
- **The flaw:** `CRT_CC_VPER 1`, and the phase is set once per field from `inv_phase = (field == frame)` (`crt_ntsc.c:199`). There is no 180° flip from line to line, so the dot pattern is wrong and comb filtering cannot work.
- The fields are 262 lines, not 262.5.

## ntsc-rs (and its lineage)

The lineage: composite-video-simulator (joncampbell123), then zhuker/ntsc and ntscqt, then ntsc-rs. ntsc-rs's core crate is MIT/ISC/Apache-2.0.

- **Phase:** it gets the phase pattern right with `((field + y) & 2) + offset`.
- **No real line timing:** each input pixel is treated as one 4fsc sample, so the dot pitch scales with the input width.
- **Decoders:** the best set of the lot. Box, notch (IIR at fs/4), 1-line and 2-line comb, a 3×3 2D filter, and gradient-adaptive. It uses SIMD and per-row threads.
- **Tricks:**
  - The carrier at 4fsc is `{1,0,-1,0}`, so modulation needs no multiplies.
  - A 4-tap box filter is an exact null at fsc.
  - `cascade_self` merges serial one-pole filters.

## NTSCRT (finnmckenty)

A Swift/Metal macOS app that wraps ntsc-rs and RetroArch shaders (librashader).

- Its own new part is `Receiver.swift`, a TV's sync circuits simulated after xscreensaver's analogtv:
  - a horizontal AFC with a PI loop and a sinusoidal phase detector (Adler-equation cycle slipping);
  - a vertical relaxation oscillator with injection locking;
  - a slicing sync separator.
- Its GPU stage adds:
  - a 3.58 MHz colour PLL with ACC and a colour killer;
  - ghosts whose hue rotates by 2π·fsc·τ;
  - snow shaped by the decoder.
- Noise is hash-seeded by position, so any frame can be re-rendered exactly.
- This is the reference to use if retrovision ever gains RF and sync faults.

## Interlacing and getting a real signal out

**Interlacing.**
- Field-rate motion means sampling each field at its own instant: 59.94 per second.
- 3:2 pulldown is the authentic way to show 24 fps material.
- Interpolate first, then scale to 480 lines, then interlace. Any scaling after the weave mixes the fields. For interlaced 4:2:0, chroma must be subsampled per field (`scale=...:interl=1`).
- Deliver interlaced as MPEG-2 with `-flags +ilme+ildct -top 1`, at 720x480 with SAR 10:11 (704 active).

**Future hardware:**
- `hacktv` (in Debian) is a complete software NTSC encoder. `-o fl2k` drives an FL2000 USB 3 VGA dongle as a DAC: composite comes out of the red pin. `-i` samples an image per field.
- A HackRF can transmit channel 3 (`-o hackrf -m m -f 61250000`); cable it directly and attenuate it.
- Sound cards and Pi PWM are far too slow for a 3.58 MHz carrier.
- Once retrovision drives real composite, the TV does the decoding. retrovision's encoder half then becomes the useful part, and the decoder half is only for previews.
