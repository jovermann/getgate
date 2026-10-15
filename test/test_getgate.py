# Copyright (c) 2026 Johannes Overmann
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class GetgateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        root = Path(cls.directory.name)
        (root / "departures").write_text('<html><script src="app.js"></script></html>')
        (root / "app.js").write_text('const departuresApi = "api/departures";')
        (root / "api").mkdir()
        (root / "api" / "departures").write_text(
            '{"departures":['
            '{"airlineCode":"BA","flightNumber":123,"timeScheduled":"14:30","gate":"A10","stand":"501"},'
            '{"flightNumber":"BA124","timeScheduled":"15:00","stand":"501"},'
            '{"flightNumber":"BA125","timeScheduled":"16:00","stand":"555"},'
            '{"flightNumber":"BA126","timeScheduled":"17:00"},'
            '{"flightNumber":"BA127","localDate":"1900-01-01","timeScheduled":"18:00","gate":"A11"}]}'
        )
        cls.page_url = (root / "departures").as_uri()

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_cli(self, *args):
        env = dict(os.environ, GETGATE_PAGE_URL=self.page_url)
        return subprocess.run([str(Path(__file__).resolve().parents[1] / "getgate"), *args],
                              env=env, text=True, capture_output=True, timeout=10)

    def test_published_gate(self):
        result = self.run_cli("BA123")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "A10\n")

    def test_stand_heuristics(self):
        for number, expected in (("BA124", "A1\n"), ("BA125", "C55\n")):
            with self.subTest(number=number):
                result = self.run_cli(number)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected)
                self.assertIn("estimated from stand", result.stderr)

    def test_time_filter(self):
        result = self.run_cli("--time", "15:00", "BA123")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Flight not found", result.stderr)

    def test_unpublished_gate(self):
        result = self.run_cli("BA126")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no gate or mappable stand", result.stderr)

    def test_wrong_date_is_excluded(self):
        result = self.run_cli("BA127")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Flight not found", result.stderr)

    def test_invalid_arguments(self):
        for args in (("--airport", "JFK", "BA123"), ("--time", "25:00", "BA123")):
            with self.subTest(args=args):
                self.assertEqual(self.run_cli(*args).returncode, 1)


if __name__ == "__main__":
    unittest.main()
