import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "capture_display", Path(__file__).resolve().parents[1] / "tools" / "capture_display.py"
)
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CaptureSummaryTests(unittest.TestCase):
    def test_empty(self):
        self.assertEqual(capture.summarize(""), (0, []))

    def test_errors_and_ansi_statistics(self):
        log = (
            "E lcd.dsi.dpi: underrun happens\r\r\n"
            "E lcd.dsi.dpi: underrun happens\r\r\n"
            "\x1b[0;32mI (100) tab5_lcd: frames sent=42\x1b[0m\r\r\n"
        )
        self.assertEqual(capture.summarize(log),
                         (2, ["I (100) tab5_lcd: frames sent=42"]))

    def test_new_stats_format(self):
        line = "I (5000) tab5_lcd: frames sent=100 window=5000ms fps=20.00"
        self.assertEqual(capture.summarize("unrelated\n" + line), (0, [line]))

    def test_interleaved_errors(self):
        self.assertEqual(capture.summarize("underrun happensunderrun happens"), (2, []))

    def test_display_diagnostic(self):
        lines = [
            "I (6000) tab5_lcd: Display diagnostic: TEST 2: STATIC HOLD; T advances mode",
            "I (11000) tab5_lcd: frames sent=0 cpu_scale=0 ppa_fail=0 diag=2",
        ]
        self.assertEqual(capture.summarize("\n".join(lines)), (0, lines))


if __name__ == "__main__":
    unittest.main()
