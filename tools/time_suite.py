#!/usr/bin/env python3
"""Per-test timing for the unittest suite. Usage: tools/time_suite.py [log].

Runs the full discover suite once with a timing result; prints the slowest
tests first. Output also goes to the given log (or stdout). Run when the
machine is otherwise idle: concurrent suites skew the numbers.
"""
import sys
import time
import unittest


def main() -> int:
    log_path = sys.argv[1] if len(sys.argv) > 1 else None
    loader = unittest.TestLoader()
    suite = loader.discover("tests", pattern="test_*.py")

    results = []
    current = {}

    class TimingResult(unittest.TextTestResult):
        def startTest(self, test):
            current[test] = time.perf_counter()
            super().startTest(test)

        def stopTest(self, test):
            elapsed = time.perf_counter() - current.pop(test, time.perf_counter())
            results.append((elapsed, str(test)))
            super().stopTest(test)

    runner = unittest.TextTestRunner(resultclass=TimingResult, verbosity=0)
    outcome = runner.run(suite)
    results.sort(reverse=True)
    lines = [f"{elapsed:8.2f}s  {name}" for elapsed, name in results[:25]]
    lines.append(f"total {sum(e for e, _ in results):.1f}s over {len(results)} tests, "
                 f"failures={len(outcome.failures)} errors={len(outcome.errors)}")
    text = "\n".join(lines) + "\n"
    if log_path:
        with open(log_path, "w") as handle:
            handle.write(text)
    sys.stdout.write(text)
    return 0 if outcome.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
