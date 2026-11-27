"""Behaviour of the NTSC simulator (bin/rvntsc), checked against what real NTSC does."""

import subprocess
import unittest
from pathlib import Path

import numpy as np

BIN = Path(__file__).resolve().parent.parent / "bin" / "rvntsc"
W, H = 764, 480
MID = slice(220, 540)  # columns well inside the picture, away from blanking edges


def run(frames, *args):
    """Feed frames (list of HxWx3 uint8) through rvntsc; return its output frames."""
    data = b"".join(f.astype(np.uint8).tobytes() for f in frames)
    out = subprocess.run([str(BIN), *args], input=data, capture_output=True, check=True).stdout
    return np.frombuffer(out, np.uint8).reshape(-1, H, W, 3).astype(int)


def flat(rgb):
    return np.broadcast_to(np.array(rgb, np.uint8), (H, W, 3)).copy()


def fsc_stripes():
    """Vertical grey stripes two samples wide: luma detail right on the 3.58 MHz subcarrier."""
    f = np.zeros((H, W, 3), np.uint8)
    f[:, (np.arange(W) // 2) % 2 == 0] = 200
    f[:, (np.arange(W) // 2) % 2 == 1] = 60
    return f


def false_colour(frame):
    """How far the middle of a frame strays from grey: cross-colour."""
    m = frame[40:440, MID]
    return np.abs(m - m.mean(axis=2, keepdims=True)).mean()


class Levels(unittest.TestCase):
    def test_flat_greys_come_back_unchanged(self):
        for dec in ("notch", "comb1", "comb2", "svideo"):
            for g in (0, 16, 128, 235, 255):
                out = run([flat((g, g, g))] * 4, f"--decoder={dec}")[-1]
                self.assertLessEqual(np.abs(out[20:460, MID] - g).max(), 1, (dec, g))

    def test_flat_colours_come_back(self):
        # 75% bars stay inside the legal range, so the legaliser leaves them alone
        bars = [(191, 191, 0), (0, 191, 191), (0, 191, 0), (191, 0, 191), (191, 0, 0), (0, 0, 191)]
        for dec in ("notch", "comb2", "svideo"):
            for c in bars:
                out = run([flat(c)] * 4, f"--decoder={dec}")[-1]
                self.assertLessEqual(np.abs(out[20:460, MID] - c).max(), 3, (dec, c))

    def test_saturation_zero_is_monochrome(self):
        out = run([flat((191, 0, 191))] * 4, "--saturation=0")[-1]
        self.assertLessEqual(np.ptp(out[20:460, MID], axis=2).max(), 1)

    def test_legaliser_turns_down_illegal_chroma(self):
        # 100% yellow peaks near 131 IRE: legalised, it comes back less saturated
        legal = run([flat((255, 255, 0))] * 4, "--legalize=1")[-1][240, 380]
        raw = run([flat((255, 255, 0))] * 4, "--legalize=0")[-1][240, 380]
        self.assertGreater(legal[2], raw[2] + 10)


class Subcarrier(unittest.TestCase):
    def test_four_field_sequence(self):
        # the same picture repeats its cross-colour every 4 fields and inverts it 2 fields on
        out = run([fsc_stripes()] * 12, "--decoder=notch")
        a, b, c = out[6], out[8], out[10]
        self.assertEqual(np.abs(a - c).max(), 0)
        self.assertGreater(np.abs(a - b)[40:440, MID].mean(), 5)

    def test_phase_flips_line_to_line(self):
        # luma on the subcarrier decodes to opposite hues on successive lines of a field
        out = run([fsc_stripes()] * 4, "--decoder=notch", "--out=fields")[2]
        row0 = out[100, MID] - out[100, MID].mean(axis=1, keepdims=True)
        row2 = out[102, MID] - out[102, MID].mean(axis=1, keepdims=True)
        corr = (row0 * row2).sum() / np.sqrt((row0 ** 2).sum() * (row2 ** 2).sum())
        self.assertLess(corr, -0.8)

    def test_comb_cancels_cross_colour_on_vertical_detail(self):
        notch = false_colour(run([fsc_stripes()] * 4, "--decoder=notch")[-1])
        comb = false_colour(run([fsc_stripes()] * 4, "--decoder=comb2")[-1])
        self.assertGreater(notch, 10)
        self.assertLess(comb, notch / 10)

    def test_svideo_has_no_cross_colour(self):
        self.assertLess(false_colour(run([fsc_stripes()] * 4, "--decoder=svideo")[-1]), 0.5)

    def test_notch_makes_dot_crawl_on_colour(self):
        # chroma leaking into luma: a flat colour field shows a fine pattern under a notch
        # (most visible at edges) but none through S-Video
        f = flat((0, 0, 0))
        f[:, 380:] = (191, 0, 191)
        def edge_ripple(dec):
            o = run([f] * 4, f"--decoder={dec}")[-1][100:380, 370:400].mean(axis=2)
            return np.abs(np.diff(o, axis=0)).mean()
        self.assertGreater(edge_ripple("notch"), edge_ripple("svideo") + 1)


class Fields(unittest.TestCase):
    def test_weave_pairs_fields_top_first(self):
        top, bottom = flat((200, 200, 200)), flat((40, 40, 40))
        out = run([top, bottom, top, bottom], "--out=weave", "--vfilter=0")
        self.assertEqual(len(out), 2)
        self.assertLessEqual(np.abs(out[1][0:480:2, MID] - 200).max(), 1)
        self.assertLessEqual(np.abs(out[1][1:480:2, MID] - 40).max(), 1)

    def test_fields_mode_takes_each_field_from_its_own_frame(self):
        top, bottom = flat((200, 200, 200)), flat((40, 40, 40))
        out = run([top, bottom], "--out=fields", "--vfilter=0")
        self.assertEqual(len(out), 2)
        self.assertLessEqual(np.abs(out[0][2:478, MID] - 200).max(), 1)
        self.assertLessEqual(np.abs(out[1][2:478, MID] - 40).max(), 1)


if __name__ == "__main__":
    unittest.main()
