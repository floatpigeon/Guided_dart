"""Host checks for calibration output, stop handling, and failed startup cleanup."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "tools/canmv_tune.py"


class CalibrationTest(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("canmv_calibration_test", SCRIPT)
        self.script = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.script)
        self.script.DETECTION_ENABLED = False
        self.events = []
        self.pwms = []
        self.results = iter([0, 100, KeyboardInterrupt()])
        self.frame_pixels = bytes(640 * 480)
        self.fail_channel = None
        self.clock = 0
        self.script.os = SimpleNamespace(exitpoint=lambda: None)
        self.script.time = SimpleNamespace(
            ticks_ms=self.tick, ticks_diff=lambda a, b: a - b, sleep_ms=lambda _: None
        )
        self.script.MAX_CONSECUTIVE_TIMEOUTS = 2
        owner = self

        class PWM:
            def __init__(self, channel):
                if channel == owner.fail_channel:
                    raise RuntimeError("PWM startup failed")
                self.channel = channel
                self.hz = 0
                self.ns = 0
                self.stopped = False
                owner.pwms.append(self)

            def freq(self, value=None):
                if value is not None:
                    self.hz = value
                return self.hz

            def duty_ns(self, value=None):
                if value is not None:
                    self.ns = value
                    owner.events.append(("pulse", self.channel, value))
                return self.ns

            def deinit(self):
                self.stopped = True
                owner.events.append(("pwm_close", self.channel))

        class FPIOA:
            def set_function(self, pin, function, **_):
                owner.events.append(("route", pin, function))

        for channel in (1, 2, 3, 5):
            setattr(FPIOA, "PWM%d" % channel, "pwm%d" % channel)
        for pin in (61, 7, 8, 59):
            setattr(FPIOA, "GPIO%d" % pin, "gpio%d" % pin)

        class Camera:
            def __init__(self, *args):
                owner.events.append(("camera_open", args))
                self.sequence = 0

            def snapshot_into(self, buffer, timeout):
                owner.assertEqual(len(buffer), 640 * 480)
                owner.assertEqual(timeout, owner.script.ACQUIRE_TIMEOUT_MS)
                result = next(owner.results)
                if isinstance(result, BaseException):
                    raise result
                if result is not None:
                    self.sequence += 1
                    buffer[:] = owner.frame_pixels
                return result

            def frame_info(self):
                return (self.sequence, self.sequence * 10000, 0, 1)

            def close(self):
                owner.events.append(("camera_close",))

        class Display:
            VIRT = "virtual"

            @staticmethod
            def init(*_, **__):
                owner.events.append(("display_open",))

            @staticmethod
            def show_image(_):
                owner.events.append(("display_frame",))

            @staticmethod
            def deinit():
                owner.events.append(("display_close",))

        def image(width, height, format, **__):
            pixels = bytearray(width * height * (2 if format == 2 else 1))
            def draw(kind, *args, **kwargs):
                owner.events.append((kind, args, kwargs))
                pixels[0] = 128  # Make an accidental detect-after-overlay visible.
            def draw_image(other, *args):
                owner.assertIsNot(pixels, other.bytearray())
                pixels[0] = other.bytearray()[0]
                owner.events.append(("preview_copy",))
            return SimpleNamespace(
                bytearray=lambda: pixels,
                draw_image=draw_image,
                draw_rectangle=lambda *a, **kw: draw("rectangle", *a, **kw),
                draw_cross=lambda *a, **kw: draw("cross", *a, **kw),
                draw_string=lambda *a, **kw: draw("text", *a, **kw),
            )

        class Detector:
            def __init__(self, config):
                owner.events.append(("detector_open",))
                owner.assertEqual(config["threshold"], owner.script.GRAY_THRESHOLD)
                owner.assertEqual(config["workspace.min_contrast"], 12.0)

            def detect(self, pixels, metadata):
                owner.assertEqual(bytes(pixels), owner.frame_pixels,
                                  "grayscale pixels must reach detection unchanged")
                owner.events.append(("detect", metadata[0]))
                return {"kind": "none", "target": None, "light": None, "armor": None,
                        "roi": (0, 0, 640, 480), "full_frame": True, "rescanned": False,
                        "candidates": 0, "missed_frames": 1, "run_high_water": 0,
                        "rejected_contrast": 0, "rejected_snr": 0, "rejected_stability": 0,
                        "phase": "detect", "full_validation": True, "stability_checks": 0}

            def close(self):
                owner.events.append(("detector_close",))

        image_module = SimpleNamespace(Image=image, GRAYSCALE=1, RGB565=2, ALLOC_HEAP=0)
        pin = lambda *args, **kwargs: None
        pin.OUT = 1
        self.modules = {
            "machine": SimpleNamespace(PWM=PWM, FPIOA=FPIOA, Pin=pin),
            "image": image_module,
            "_dart_raw": SimpleNamespace(Camera=Camera, PIXEL_FORMAT="gray8"),
            "_dart_detect": SimpleNamespace(Detector=Detector, GRAY_FEATURE_VERSION=1, TRACK_FEATURE_VERSION=1),
            "media": SimpleNamespace(),
            "media.display": SimpleNamespace(Display=Display),
        }

    def tick(self):
        self.clock += 100
        return self.clock

    def run_script(self):
        with patch.dict(sys.modules, self.modules), contextlib.redirect_stdout(io.StringIO()):
            self.script.main()

    def assert_cleaned(self, count):
        self.assertEqual(len(self.pwms), count)
        self.assertTrue(all(pwm.stopped for pwm in self.pwms))
        self.assertIn(("camera_close",), self.events)
        self.assertIn(("display_close",), self.events)
        last_pwm_close = max(i for i, item in enumerate(self.events) if item[0] == "pwm_close")
        self.assertLess(last_pwm_close, self.events.index(("camera_close",)))

    def test_old_binary_interpreter_requires_update(self):
        del self.modules["_dart_raw"].PIXEL_FORMAT
        with self.assertRaisesRegex(RuntimeError, "updated tuning interpreter"):
            self.run_script()
        self.assertFalse(self.pwms)
        self.assertFalse(any(item[0] == "camera_open" for item in self.events))

    def test_black_frame_is_valid_and_interrupt_closes_all_outputs(self):
        self.run_script()
        self.assertEqual(self.events.count(("display_frame",)), 2)
        self.assert_cleaned(4)

    def test_fractional_microseconds_and_routes_only_after_all_pulses_written(self):
        self.script.SERVO_INITIAL_US = (1512.125, 1530.5, 1499.9996, 1520.0)
        self.run_script()
        self.assertEqual([pwm.ns for pwm in self.pwms], [1512125, 1530500, 1500000, 1520000])
        last_write = max(i for i, item in enumerate(self.events) if item[0] == "pulse")
        for pin, channel in zip((61, 7, 8, 59), (1, 2, 3, 5)):
            routes = [i for i, item in enumerate(self.events) if item == ("route", pin, "pwm%d" % channel)]
            self.assertGreater(routes[-1], last_write)

    def test_camera_timeouts_close_pwm_and_camera(self):
        self.results = iter([None, None])
        with self.assertRaisesRegex(RuntimeError, "consecutive frame timeouts"):
            self.run_script()
        self.assert_cleaned(4)

    def test_partial_pwm_startup_closes_preceding_channels_and_camera(self):
        self.fail_channel = 3
        with self.assertRaisesRegex(RuntimeError, "PWM startup failed"):
            self.run_script()
        self.assert_cleaned(2)

    def test_detection_uses_raw_pixels_and_never_advances_on_timeout(self):
        self.script.DETECTION_ENABLED = True
        self.frame_pixels = bytes((0, 20, 59, 60, 61, 180, 254, 255)) * (640 * 480 // 8)
        self.results = iter([None, 0, None, 100, KeyboardInterrupt()])
        self.run_script()
        self.assertEqual([item for item in self.events if item[0] == "detect"],
                         [("detect", 1), ("detect", 2)])
        self.assertEqual(self.events.count(("display_frame",)), 2)
        self.assertEqual(self.events.count(("preview_copy",)), 2)
        self.assertIn(("detector_close",), self.events)
        self.assert_cleaned(4)

    def test_light_and_armor_crosses_keep_separate_colors(self):
        events = []
        preview = SimpleNamespace(
            draw_rectangle=lambda *args, **kwargs: None,
            draw_string=lambda *args, **kwargs: None,
            draw_cross=lambda *args, **kwargs: events.append((args, kwargs)),
        )
        light = {"center": (100.5, 120.5), "bounds": (96, 116, 10, 10)}
        armor = {"center": (102.5, 80.5), "bounds": (92, 70, 20, 20)}
        self.script._draw_detection(preview, {
            "kind": "armor", "light": light, "armor": armor, "target": armor,
            "roi": (80, 60, 50, 90), "candidates": 3,
        })
        self.assertIn(((101, 121), {"color": (0, 255, 0), "size": 10, "thickness": 2}), events)
        self.assertIn(((103, 81), {"color": (255, 0, 0), "size": 10, "thickness": 2}), events)
        self.assertEqual(events[-1][1]["color"], (255, 0, 0))

    def test_detector_failure_closes_camera_and_pwm(self):
        self.script.DETECTION_ENABLED = True
        def fail(*_):
            raise RuntimeError("Detector detect failed: 6")
        with patch.object(self.modules["_dart_detect"].Detector, "detect", fail):
            with self.assertRaisesRegex(RuntimeError, "Detector detect failed: 6"):
                self.run_script()
        self.assertIn(("detector_close",), self.events)
        self.assert_cleaned(4)

    def test_old_grayscale_interpreter_fails_before_camera_or_pwm_start(self):
        self.script.DETECTION_ENABLED = True
        self.modules["_dart_detect"].GRAY_FEATURE_VERSION = 0
        with self.assertRaisesRegex(RuntimeError, "Grayscale feature checks require"):
            self.run_script()
        self.assertFalse(self.pwms)
        self.assertNotIn(("detector_open",), self.events)

    def test_old_tracking_interpreter_fails_before_outputs_start(self):
        self.script.DETECTION_ENABLED = True
        self.modules["_dart_detect"].TRACK_FEATURE_VERSION = 0
        with self.assertRaisesRegex(RuntimeError, "Detect/track requires"):
            self.run_script()
        self.assertFalse(self.pwms)
        self.assertNotIn(("detector_open",), self.events)


if __name__ == "__main__":
    unittest.main()
