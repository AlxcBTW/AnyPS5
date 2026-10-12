import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import perf_report


def side(**counts):
    return {"counter": "valgrind", "workloads": {name: {"ir": count, "runs": [count]} for name, count in counts.items()}}


class PerfReportTests(unittest.TestCase):
    def test_flags_only_beyond_the_threshold(self):
        table = perf_report.rows(side(a=1000, b=1000, c=1000), side(a=1005, b=1020, c=980), 1.0)
        self.assertEqual([row[4] for row in table], ["ok", "slower", "faster"])
        self.assertAlmostEqual(table[1][3], 2.0)

    def test_missing_workload_has_no_data(self):
        table = perf_report.rows(side(a=1000), side(b=1000), 1.0)
        self.assertEqual([(row[0], row[4]) for row in table], [("a", "no data"), ("b", "no data")])

    def test_cachegrind_summary_is_parsed(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "cachegrind.out"
            log.write_text("desc: I1 cache:\nevents: Ir\nfn=main\n1 5\nsummary: 123456\n")
            self.assertEqual(perf_report.parse_count("valgrind", log, ""), 123456)

    def test_perf_stat_output_is_parsed(self):
        self.assertEqual(perf_report.parse_count("perf", None, "987654,,instructions:u,1000,100.00,,\n"), 987654)

    def test_spread_uses_repeated_runs(self):
        value = {"counter": "valgrind", "workloads": {"a": {"ir": 1000, "runs": [1000, 1001]}}}
        self.assertAlmostEqual(perf_report.spread(value), 0.1)


if __name__ == "__main__":
    unittest.main()
