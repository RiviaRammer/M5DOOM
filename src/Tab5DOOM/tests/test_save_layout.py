"""Configuration regressions for onboard saves and removal of the T test."""
from pathlib import Path
import csv
import unittest

ROOT = Path(__file__).resolve().parents[1]


def size(value):
    value = value.strip()
    return int(value[:-1], 0) * 1024 * 1024 if value.endswith("M") else (
        int(value[:-1], 0) * 1024 if value.endswith("K") else int(value, 0))


class SaveLayoutTests(unittest.TestCase):
    def test_partition_bounds_and_overlap(self):
        rows = csv.reader(line for line in (ROOT / "partitions.csv").read_text().splitlines()
                          if line.strip() and not line.lstrip().startswith("#"))
        partitions = [(r[0].strip(), size(r[3]), size(r[4])) for r in rows]
        ordered = sorted(partitions, key=lambda p: p[1])
        for (_, start, length), (_, next_start, _) in zip(ordered, ordered[1:]):
            self.assertLessEqual(start + length, next_start)
        self.assertLessEqual(max(p[1] + p[2] for p in partitions), 16 * 1024 * 1024)
        self.assertIn(("wad", 0x400000, 8 * 1024 * 1024), partitions)
        self.assertIn(("saves", 0xC00000, 4 * 1024 * 1024), partitions)

    def test_flash_policy(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn('spiffs_create_partition_image(saves "${CMAKE_BINARY_DIR}/empty_saves" FLASH_IN_PROJECT)', cmake)
        self.assertIn("if(save_seed_files)", cmake)
        # Also validate IDF-generated arguments when a build is available.
        full = ROOT / "build/flash_args"
        app = ROOT / "build/flash_app_args"
        if full.exists() and app.exists():
            self.assertIn("0xc00000 saves.bin", full.read_text().lower())
            self.assertNotIn("saves.bin", app.read_text())
            self.assertEqual((ROOT / "build/saves.bin").stat().st_size, 4 * 1024 * 1024)

    def test_no_t_diagnostic_in_production_sources(self):
        base = ROOT / "components/prboom-esp32-compat"
        for name in ("gamepad.c", "spi_lcd.c", "include/spi_lcd.h", "include/tab5_helpers.h"):
            source = (base / name).read_text(encoding="utf-8")
            for obsolete in ("spi_lcd_cycle_diagnostic", "TAB5_BTN_DISPLAY_TEST",
                             "tab5_display_test_pattern", "STATIC HOLD", "T: display test"):
                self.assertNotIn(obsolete, source)


if __name__ == "__main__":
    unittest.main()
