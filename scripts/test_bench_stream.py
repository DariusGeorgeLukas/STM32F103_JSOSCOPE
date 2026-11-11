#!/usr/bin/env python3
"""Runs the bench test against the simulated device, including injected faults.

A bench that cannot fail proves nothing, so each fault below has to be caught.

    python3 scripts/test_bench_stream.py /path/to/host_test
"""

import argparse
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_stream as bench  # noqa: E402
import simulate_device as simulate  # noqa: E402

EXPECT = {"pa2": 1.0, "pa3": 0.5}
EXPECT_DIGITAL = {"pb14": 1, "pb15": 0}


class Port:
    """The small part of a pyserial port that the bench uses, on a pty."""

    def __init__(self, descriptor):
        import os
        import select

        self.os = os
        self.select = select
        self.descriptor = descriptor

    @property
    def in_waiting(self):
        return 4096

    def read(self, size):
        ready, _, _ = self.select.select([self.descriptor], [], [], 0.02)
        return self.os.read(self.descriptor, size) if ready else b""

    def write(self, data):
        self.os.write(self.descriptor, data)


def run(host_test, fault, cases):
    primary, _path, secondary = simulate.open_terminal()
    descriptor = simulate.load_descriptor(host_test)
    simulator = simulate.Simulator(descriptor, host_test, fault)
    stopped = threading.Event()
    thread = threading.Thread(target=simulate.serve, args=(primary, simulator, stopped.is_set), daemon=True)
    thread.start()
    options = argparse.Namespace(
        seconds=1.0, expect=EXPECT, expect_digital=EXPECT_DIGITAL, case=cases, soak=0
    )
    try:
        return bench.run_cases(bench.Device(Port(secondary)), options)
    finally:
        stopped.set()
        thread.join(timeout=2)


def failures_of(results):
    return [failure for result in results for failure in result["failures"]]


def main():
    host_test = sys.argv[1]
    results = run(host_test, None, None)
    failed = [result["name"] for result in results if not result["passed"]]
    assert not failed, f"a clean simulated device failed: {failed}"

    for fault, case, expected in (
        ("swapped", "all-inputs-1k", "pa2"),
        ("slow-rate", "default", "rows/s"),
        ("lost-blocks", "all-inputs-10k", "lost"),
    ):
        problems = failures_of(run(host_test, fault, [case]))
        assert any(expected in problem for problem in problems), (fault, problems)
    print("bench self-test passed")


if __name__ == "__main__":
    main()
