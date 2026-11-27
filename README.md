# retrovision

Makes video look the way it did coming into a 1980s or 90s television. It simulates, sample by sample, the NTSC composite signal and the TV that decodes it, plus real interlaced field timing. This is a standalone tool: ai_tv uses it, and so can anything else.

**No fake CRT effects:** no scanlines, phosphor masks, glow, curvature or VHS damage. The output is meant to be watched on a real CRT, which supplies all of that itself. retrovision supplies only what the *signal* did to the picture.

> **Analogue hardware output is planned but not scoped yet.** One goal is to emit a real signal: VGA, or genuine RF over coax into a TV and VCR, via an FL2000 USB-VGA dongle with hacktv, or a HackRF. See `docs/RESEARCH.md`. For now, v1 is a software filter: video files in, video files out.

## Use

```sh
make                 # builds bin/rvntsc (C99, gcc; no dependencies beyond libm and pthreads)
make test            # physics checks (needs python3 + numpy)

./retrovision in.mp4 out.mp4                         # 59.94p for PC -> HDMI-to-AV -> CRT
./retrovision in.mp4 out.mpg --deliver dvd           # true 29.97i interlaced MPEG-2
./retrovision in.mp4 out.mp4 --decoder notch         # an 80s set instead of the default comb
./retrovision in.mp4 out.mp4 --motion hold           # fast: no interpolation
./retrovision still.png out.png                      # one field of a still
./retrovision in.mp4 out.mp4 --ntsc sharpness=0.6 --ntsc chroma-bw=0.5
```

Run `./retrovision -h` and `bin/rvntsc --help` for every knob.

**Playback through an HDMI-to-AV converter** (the current setup):
- Play the `hdmi` output fullscreen in mpv at a 59.94/60 Hz desktop mode the converter accepts, with `--video-sync=display-resample`.
- The converter cuts its own 480i fields from successive frames. Each frame already holds one field's moment and subcarrier phase, so the TV ends up with real field-rate motion.
- Don't feed the converter woven interlaced frames. It would scale the combing.
- `0_field_rate_test` in the ai_tv test outputs checks whether a given converter passes all 59.94 fields.

## What it simulates

`bin/rvntsc` (`src/ntsc.c`) runs every field through this chain.

**Encoder (broadcast):**
- The picture becomes Y'IQ straight from gamma-encoded R'G'B', which is what real NTSC did.
- Band limits: Y 4.2 MHz, I 1.3 MHz, Q 0.6 MHz. These are causal Butterworth filters, so they ring after an edge, not before, and their delays are matched as a station's would be.
- A legaliser turns chroma down so the signal stays within -20 to 120 IRE.
- 7.5 IRE setup, -40 IRE sync, and a 9-cycle burst at 180°. Everything is sampled at 4fsc: 14.318 MHz, 910 samples per line.

**Subcarrier phase:**
- Phase is taken from each sample's absolute position in a continuous 525-line stream. Because 910 = 2 (mod 4), the phase flips half a cycle every line and every frame.
- The 4-field colour sequence and its dot crawl therefore come from the arithmetic; nothing draws them.
- Fields map 1:1 to lines: field 1 is lines 23–262 (picture rows 0, 2, 4…) and field 2 is lines 286–525 (rows 1, 3, 5…), top field first.

**TV:**
- The colour PLL locks to the burst (averaged across lines), and automatic colour control (ACC) normalises burst amplitude to 20 IRE.
- Y/C separation, by decoder:

  | Decoder | Set it models | What it does |
  |---|---|---|
  | `notch` | 70s/80s | 3.58 MHz trap: dot crawl and rainbows |
  | `comb1` | late 80s | 1H comb: hanging dots |
  | `comb2` | 90s (default) | 2-line comb |
  | `svideo` | S-Video | separate Y and C: bleed only |

- Demodulation is synchronous against the locked oscillator, followed by a 0.6 MHz low-pass.
- Delay-line peaking (sharpness), then the tint, colour, picture and brightness knobs.

**Fields:**
- Every input frame is the picture at one field's instant (59.94/s). Only that field's lines are used, after a mild vertical filter (`vfilter`), like a camera's field integration.
- `--motion broadcast` (default) interpolates every field to its own moment: the fluid, blurred motion of interlaced broadcast video. `--motion hold` repeats the nearest source frame instead.
- **A post effect:** the output runs exactly as long as the input, and the audio is copied through untouched.

## Speed

On an i5-4690K (4 cores, 2014), the full literal simulation runs at about 200 fields/s, which is 3.4× real time. On one core it is 1.5× real time. A 40 s clip takes about as long as the clip to render, with ffmpeg's x264 encode taking the rest of the CPU.

blargg's precomputed-kernel method (see `docs/RESEARCH.md`) would be much faster, but it isn't needed yet. If it's ever built, `tests/` must show it matching this literal path.

## Layout

| Path | What it is |
|---|---|
| `src/ntsc.h`, `src/ntsc.c` | the simulator library: `rv_create`, `rv_field` |
| `src/rvntsc.c` | CLI: raw RGB24 frames on stdin and stdout |
| `retrovision` | Python 3 driver (stdlib only): ffmpeg, then rvntsc, then ffmpeg |
| `tests/test_ntsc.py` | checks that levels and colours survive; the 4-field sequence; the line-to-line phase flip; that combs cancel cross-colour; that S-Video has none; field and weave order |
| `docs/RESEARCH.md` | what was learned from blargg, NTSC-CRT, ntsc-rs, NTSCRT and others |

## Not done yet

**Next step: RIFE for `--motion broadcast`.**
- Replace ffmpeg's minterpolate with RIFE (rife-ncnn-vulkan, on the GTX 970) to interpolate to 59.94 fields.
- minterpolate runs on the CPU at about 0.1x real time (about 7 minutes per 40 s clip) and can warp AI footage.
- The post-effect rules still apply: same running time, audio untouched.

After that:

- **Analogue output to real hardware** (VGA, RF over coax to a TV or VCR). Planned, not scoped.
- A VCR / tape stage, RF reception (ghosts, snow), and receiver sync faults. NTSCRT's `Receiver.swift` is the reference design.
- A blargg-style compiled fast path, for real-time use.
