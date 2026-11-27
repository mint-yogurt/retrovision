"""The retrovision driver end to end: ffmpeg in, rvntsc, ffmpeg out."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DRIVER = ROOT / "retrovision"


def ff(*args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *args], check=True)


def streams(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                          "stream=codec_type,width,height,duration,field_order,r_frame_rate",
                          "-of", "json", str(path)], capture_output=True, text=True, check=True).stdout
    return {s["codec_type"]: s for s in json.loads(out)["streams"]}


def frame(path, at=1.0):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", str(at), "-i", str(path), "-frames:v", "1",
                          "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(480, -1, 3).astype(int)


class Driver(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        # 24 fps 4:3 clip whose audio is shorter than its picture
        cls.clip43 = d / "c43.mp4"
        ff("-f", "lavfi", "-i", "testsrc2=size=640x480:rate=24:duration=3", "-f", "lavfi",
           "-i", "sine=frequency=440:duration=2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
           str(cls.clip43))
        cls.clip169 = d / "c169.mp4"
        ff("-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=2", "-c:v", "libx264",
           "-pix_fmt", "yuv420p", str(cls.clip169))
        cls.still = d / "s.png"
        ff("-f", "lavfi", "-i", "testsrc2=size=800x600", "-frames:v", "1", str(cls.still))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def render(self, src, name, *args):
        out = Path(self.tmp.name) / name
        subprocess.run([str(DRIVER), str(src), str(out), *args], check=True, capture_output=True)
        return out

    def test_post_effect_keeps_running_time_and_sound(self):
        out = self.render(self.clip43, "post.mp4", "--motion", "hold")
        s = streams(out)
        self.assertEqual(s["video"]["r_frame_rate"], "60000/1001")
        self.assertEqual((s["video"]["width"], s["video"]["height"]), (640, 480))
        self.assertAlmostEqual(float(s["video"]["duration"]), 3.0, delta=0.05)
        src_audio = streams(self.clip43)["audio"]
        self.assertEqual(s["audio"]["duration"], src_audio["duration"])
        md5 = lambda f: subprocess.run(["ffmpeg", "-v", "error", "-i", str(f), "-map", "0:a", "-c", "copy",
                                        "-f", "md5", "-"], capture_output=True, text=True, check=True).stdout
        self.assertEqual(md5(out), md5(self.clip43))

    def test_broadcast_motion_keeps_running_time(self):
        s = streams(self.render(self.clip43, "bc.mp4"))
        self.assertAlmostEqual(float(s["video"]["duration"]), 3.0, delta=0.05)

    def test_dvd_is_interlaced_top_field_first(self):
        s = streams(self.render(self.clip43, "dvd.mpg", "--deliver", "dvd", "--motion", "hold"))
        v = s["video"]
        self.assertEqual((v["width"], v["height"], v["field_order"]), (720, 480, "tt"))
        self.assertEqual(v["r_frame_rate"], "30000/1001")

    def test_wide_source_is_letterboxed_or_centre_cut(self):
        pad = frame(self.render(self.clip169, "pad.mp4", "--fit", "pad", "--motion", "hold"))
        crop = frame(self.render(self.clip169, "crop.mp4", "--fit", "crop", "--motion", "hold"))
        # 16:9 inside 4:3 leaves 60 black lines top and bottom
        self.assertLess(pad[:50].mean(), 25)
        self.assertLess(pad[-50:].mean(), 25)
        self.assertGreater(pad[100:380].mean(), 40)
        self.assertGreater(crop[:50].mean(), 40)

    def test_still_gives_one_field_as_png(self):
        out = self.render(self.still, "still.png")
        s = streams(out)["video"]
        self.assertEqual((s["width"], s["height"]), (640, 480))


if __name__ == "__main__":
    unittest.main()
