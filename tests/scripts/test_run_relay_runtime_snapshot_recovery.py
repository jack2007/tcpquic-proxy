#!/usr/bin/env python3
"""Self-tests for run_relay_runtime_snapshot_recovery.py (no live process required)."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Dict, List, Tuple


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = Path(__file__).resolve().parent / "run_relay_runtime_snapshot_recovery.py"


def load_module():
    spec = importlib.util.spec_from_file_location("relay_recovery", MODULE_PATH)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


mod = load_module()


class FakeClock:
    def __init__(self, start: float = 1000.0) -> None:
        self.t = start

    def now(self) -> float:
        return self.t

    def sleep(self, seconds: float) -> None:
        self.t += seconds


class FakeProcess:
    def __init__(self, returncode: int = 0, stdout: str = "", stderr: str = "") -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class RecoveryHarnessTests(unittest.TestCase):
    def _ok_http(self, sequence: List[Tuple[int, dict]]):
        calls = {"n": 0}

        def http_get(url: str, token: str, timeout: float = 5.0):
            idx = min(calls["n"], len(sequence) - 1)
            calls["n"] += 1
            return sequence[idx]

        return http_get, calls

    def _commands(self, mapping: Dict[str, int]):
        seen: List[str] = []

        def run_command(command: str) -> FakeProcess:
            seen.append(command)
            # Match by label embedded as first token convention in tests.
            for key, code in mapping.items():
                if key in command:
                    return FakeProcess(code)
            return FakeProcess(0)

        return run_command, seen

    def test_successful_recovery(self) -> None:
        clock = FakeClock()

        def http_get(url: str, token: str, timeout: float = 5.0):
            if url.endswith("/health"):
                return 200, {"status": "ok"}
            return 200, {"snapshot_complete": True, "workers": [{"worker_id": "aggregate"}]}

        run_command, seen = self._commands(
            {
                "start-ok": 0,
                "stop-ok": 0,
                "kill-ok": 0,
                "connect-ok": 0,
            }
        )
        with tempfile.TemporaryDirectory() as tmp:
            harness = mod.RecoveryHarness(
                base_url="http://127.0.0.1:9/api/v1",
                token="t",
                start_command="start-ok",
                stop_command="stop-ok",
                kill_command="kill-ok",
                connect_probe_command="connect-ok",
                graceful_timeout=60,
                recovery_timeout=90,
                poll_interval=1,
                artifact_dir=Path(tmp),
                clock=mod.Clock(now_fn=clock.now, sleep_fn=clock.sleep),
                http_get=http_get,
                run_command=run_command,
            )
            result = harness.run()
            self.assertTrue(result.ok)
            self.assertEqual(result.exit_code, 0)
            self.assertTrue((Path(tmp) / "timeline.json").exists())
            self.assertIn("start-ok", seen[0])

    def test_graceful_timeout_boundary(self) -> None:
        clock = FakeClock()

        def http_get(url, token, timeout=5.0):
            return 200, {"status": "ok", "snapshot_complete": True, "workers": []}

        def run_command(command: str) -> FakeProcess:
            if command == "stop-slow":
                clock.t += 61  # exceed graceful timeout of 60
            return FakeProcess(0)

        harness = mod.RecoveryHarness(
            base_url="http://127.0.0.1:9/api/v1",
            token="t",
            start_command="start",
            stop_command="stop-slow",
            kill_command="kill",
            graceful_timeout=60,
            recovery_timeout=90,
            poll_interval=1,
            clock=mod.Clock(now_fn=clock.now, sleep_fn=clock.sleep),
            http_get=http_get,
            run_command=run_command,
        )
        result = harness.run()
        self.assertFalse(result.ok)
        self.assertEqual(result.exit_code, 4)

    def test_health_never_recovers(self) -> None:
        clock = FakeClock()
        phase = {"after_kill": False}

        def http_get(url, token, timeout=5.0):
            if url.endswith("/health"):
                if phase["after_kill"]:
                    return 0, {"error": "down"}
                return 200, {"status": "ok"}
            return 200, {"snapshot_complete": True, "workers": []}

        def run_command(command: str) -> FakeProcess:
            if command == "kill":
                phase["after_kill"] = True
            return FakeProcess(0)

        harness = mod.RecoveryHarness(
            base_url="http://127.0.0.1:9/api/v1",
            token="t",
            start_command="start",
            stop_command="stop",
            kill_command="kill",
            graceful_timeout=60,
            recovery_timeout=5,
            poll_interval=1,
            clock=mod.Clock(now_fn=clock.now, sleep_fn=clock.sleep),
            http_get=http_get,
            run_command=run_command,
        )
        result = harness.run()
        self.assertFalse(result.ok)
        self.assertEqual(result.exit_code, 5)

    def test_command_nonzero_exit(self) -> None:
        clock = FakeClock()
        harness = mod.RecoveryHarness(
            base_url="http://127.0.0.1:9/api/v1",
            token="t",
            start_command="start-fail",
            stop_command="stop",
            kill_command="kill",
            graceful_timeout=60,
            recovery_timeout=90,
            poll_interval=1,
            clock=mod.Clock(now_fn=clock.now, sleep_fn=clock.sleep),
            http_get=lambda *a, **k: (200, {"status": "ok"}),
            run_command=lambda c: FakeProcess(1 if "start-fail" in c else 0),
        )
        result = harness.run()
        self.assertFalse(result.ok)
        self.assertEqual(result.exit_code, 3)

    def test_recovery_timeout_90s_boundary(self) -> None:
        clock = FakeClock()

        def http_get(url, token, timeout=5.0):
            if url.endswith("/health"):
                return 200, {"status": "ok"}
            # workers never complete
            return 200, {"snapshot_complete": False, "workers": []}

        harness = mod.RecoveryHarness(
            base_url="http://127.0.0.1:9/api/v1",
            token="t",
            start_command="start",
            stop_command="stop",
            kill_command="kill",
            graceful_timeout=60,
            recovery_timeout=90,
            poll_interval=10,
            clock=mod.Clock(now_fn=clock.now, sleep_fn=clock.sleep),
            http_get=http_get,
            run_command=lambda _c: FakeProcess(0),
        )
        result = harness.run()
        self.assertFalse(result.ok)
        self.assertEqual(result.exit_code, 6)
        self.assertGreaterEqual(clock.t - 1000.0, 90.0)


class CliHelpTests(unittest.TestCase):
    def test_help_exits_zero(self) -> None:
        cp = subprocess.run(
            [sys.executable, str(MODULE_PATH), "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(cp.returncode, 0)
        self.assertIn("recovery harness", cp.stdout.lower())


if __name__ == "__main__":
    unittest.main()
