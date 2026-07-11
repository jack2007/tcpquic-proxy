#!/usr/bin/env python3
"""Cross-platform recovery harness for relay runtime snapshot Stop/kill gates.

Records T0, polls /api/v1/health and /api/v1/relay/workers, and verifies:
  - graceful stop finishes within --graceful-timeout (default 60s)
  - after hard kill, health + optional CONNECT probe recover within
    --recovery-timeout (default 90s)

Exit codes:
  0 success
  2 argument / config error
  3 start/stop/kill command failure
  4 graceful timeout
  5 recovery timeout / health never recovered
  6 workers never snapshot_complete after recovery
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence


@dataclass
class Event:
    t_rel_s: float
    name: str
    detail: Dict = field(default_factory=dict)


@dataclass
class HarnessResult:
    ok: bool
    exit_code: int
    t0_unix: float
    events: List[Event] = field(default_factory=list)
    error: str = ""


class Clock:
    def __init__(self, now_fn: Optional[Callable[[], float]] = None, sleep_fn: Optional[Callable[[float], None]] = None):
        self._now = now_fn or time.time
        self._sleep = sleep_fn or time.sleep

    def now(self) -> float:
        return self._now()

    def sleep(self, seconds: float) -> None:
        self._sleep(seconds)


def _default_http_get(url: str, token: str, timeout: float = 5.0) -> tuple[int, dict]:
    req = urllib.request.Request(
        url,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/json",
        },
        method="GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", "replace")
            try:
                payload = json.loads(body) if body else {}
            except json.JSONDecodeError:
                payload = {"_raw": body}
            return int(resp.status), payload
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        try:
            payload = json.loads(body) if body else {}
        except json.JSONDecodeError:
            payload = {"_raw": body}
        return int(exc.code), payload
    except Exception as exc:  # noqa: BLE001 - surface as transport failure
        return 0, {"error": str(exc)}


def _run_command(command: str, *, shell: bool = False) -> subprocess.CompletedProcess:
    if not command:
        raise ValueError("empty command")
    if shell:
        return subprocess.run(command, shell=True, capture_output=True, text=True, check=False)
    return subprocess.run(shlex.split(command), capture_output=True, text=True, check=False)


class RecoveryHarness:
    def __init__(
        self,
        *,
        base_url: str,
        token: str,
        start_command: str,
        stop_command: str,
        kill_command: str,
        freeze_worker_command: str = "",
        unfreeze_worker_command: str = "",
        connect_probe_command: str = "",
        graceful_timeout: float = 60.0,
        recovery_timeout: float = 90.0,
        poll_interval: float = 1.0,
        artifact_dir: Optional[Path] = None,
        clock: Optional[Clock] = None,
        http_get: Optional[Callable[[str, str, float], tuple[int, dict]]] = None,
        run_command: Optional[Callable[[str], subprocess.CompletedProcess]] = None,
        shell: bool = False,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.start_command = start_command
        self.stop_command = stop_command
        self.kill_command = kill_command
        self.freeze_worker_command = freeze_worker_command
        self.unfreeze_worker_command = unfreeze_worker_command
        self.connect_probe_command = connect_probe_command
        self.graceful_timeout = graceful_timeout
        self.recovery_timeout = recovery_timeout
        self.poll_interval = poll_interval
        self.artifact_dir = Path(artifact_dir) if artifact_dir else None
        self.clock = clock or Clock()
        self.http_get = http_get or _default_http_get
        self.run_command = run_command or (lambda cmd: _run_command(cmd, shell=shell))
        self.events: List[Event] = []
        self.t0 = 0.0

    def _log(self, name: str, **detail) -> None:
        rel = 0.0 if not self.t0 else self.clock.now() - self.t0
        self.events.append(Event(t_rel_s=rel, name=name, detail=detail))

    def _write_artifacts(self, result: HarnessResult) -> None:
        if not self.artifact_dir:
            return
        self.artifact_dir.mkdir(parents=True, exist_ok=True)
        timeline = [asdict(e) for e in result.events]
        (self.artifact_dir / "timeline.json").write_text(
            json.dumps(timeline, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        (self.artifact_dir / "result.json").write_text(
            json.dumps(asdict(result), indent=2, sort_keys=True),
            encoding="utf-8",
        )

    def _cmd(self, label: str, command: str) -> subprocess.CompletedProcess:
        self._log("command_start", label=label, command=command)
        cp = self.run_command(command)
        self._log(
            "command_end",
            label=label,
            returncode=cp.returncode,
            stdout=(cp.stdout or "")[-2000:],
            stderr=(cp.stderr or "")[-2000:],
        )
        return cp

    def _health_ok(self) -> bool:
        status, payload = self.http_get(f"{self.base_url}/health", self.token, 5.0)
        self._log("health_poll", status=status, payload=payload)
        return status == 200

    def _workers_complete(self) -> bool:
        status, payload = self.http_get(f"{self.base_url}/relay/workers", self.token, 5.0)
        complete = status == 200 and bool(payload.get("snapshot_complete"))
        self._log(
            "workers_poll",
            status=status,
            snapshot_complete=bool(payload.get("snapshot_complete")),
            worker_count=len(payload.get("workers") or []) if isinstance(payload, dict) else 0,
        )
        return complete

    def _connect_ok(self) -> bool:
        if not self.connect_probe_command:
            return True
        cp = self._cmd("connect_probe", self.connect_probe_command)
        return cp.returncode == 0

    def _wait_until(self, predicate: Callable[[], bool], timeout: float, label: str) -> bool:
        deadline = self.clock.now() + timeout
        while self.clock.now() <= deadline:
            if predicate():
                self._log("wait_satisfied", label=label)
                return True
            self.clock.sleep(self.poll_interval)
        self._log("wait_timeout", label=label, timeout=timeout)
        return False

    def run(self) -> HarnessResult:
        self.t0 = self.clock.now()
        self._log("t0")

        start = self._cmd("start", self.start_command)
        if start.returncode != 0:
            result = HarnessResult(False, 3, self.t0, self.events, "start_command failed")
            self._write_artifacts(result)
            return result

        if not self._wait_until(self._health_ok, min(30.0, self.graceful_timeout), "initial_health"):
            result = HarnessResult(False, 5, self.t0, self.events, "initial health failed")
            self._write_artifacts(result)
            return result

        if self.freeze_worker_command:
            freeze = self._cmd("freeze_worker", self.freeze_worker_command)
            if freeze.returncode != 0:
                result = HarnessResult(False, 3, self.t0, self.events, "freeze_worker_command failed")
                self._write_artifacts(result)
                return result

        stop = self._cmd("stop", self.stop_command)
        if stop.returncode != 0:
            result = HarnessResult(False, 3, self.t0, self.events, "stop_command failed")
            self._write_artifacts(result)
            return result

        # Graceful stop must return before lease barrier timeout.
        if (self.clock.now() - self.t0) > self.graceful_timeout:
            result = HarnessResult(False, 4, self.t0, self.events, "graceful stop exceeded timeout")
            self._write_artifacts(result)
            return result
        self._log("graceful_stop_ok")

        if self.unfreeze_worker_command:
            unfreeze = self._cmd("unfreeze_worker", self.unfreeze_worker_command)
            if unfreeze.returncode != 0:
                result = HarnessResult(False, 3, self.t0, self.events, "unfreeze_worker_command failed")
                self._write_artifacts(result)
                return result

        # Restart path after hard kill.
        kill = self._cmd("kill", self.kill_command)
        if kill.returncode != 0:
            result = HarnessResult(False, 3, self.t0, self.events, "kill_command failed")
            self._write_artifacts(result)
            return result

        restart = self._cmd("restart", self.start_command)
        if restart.returncode != 0:
            result = HarnessResult(False, 3, self.t0, self.events, "restart after kill failed")
            self._write_artifacts(result)
            return result

        def recovered() -> bool:
            return self._health_ok() and self._workers_complete() and self._connect_ok()

        if not self._wait_until(recovered, self.recovery_timeout, "post_kill_recovery"):
            # Distinguish health vs workers for diagnostics.
            if not self._health_ok():
                result = HarnessResult(False, 5, self.t0, self.events, "health did not recover")
            else:
                result = HarnessResult(False, 6, self.t0, self.events, "workers snapshot incomplete after recovery")
            self._write_artifacts(result)
            return result

        result = HarnessResult(True, 0, self.t0, self.events, "")
        self._write_artifacts(result)
        return result


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Relay runtime snapshot recovery harness")
    p.add_argument("--base-url", required=True, help="Admin API base, e.g. http://127.0.0.1:8080/api/v1")
    p.add_argument("--token", required=True, help="Admin bearer token")
    p.add_argument("--start-command", required=True)
    p.add_argument("--stop-command", required=True)
    p.add_argument("--kill-command", required=True)
    p.add_argument("--freeze-worker-command", default="")
    p.add_argument("--unfreeze-worker-command", default="")
    p.add_argument("--connect-probe-command", default="", help="Optional CONNECT probe; must exit 0 on success")
    p.add_argument("--graceful-timeout", type=float, default=60.0)
    p.add_argument("--recovery-timeout", type=float, default=90.0)
    p.add_argument("--poll-interval", type=float, default=1.0)
    p.add_argument("--artifact-dir", default="artifacts/relay-runtime-snapshot-recovery")
    p.add_argument("--shell", action="store_true", help="Run commands through the platform shell")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    harness = RecoveryHarness(
        base_url=args.base_url,
        token=args.token,
        start_command=args.start_command,
        stop_command=args.stop_command,
        kill_command=args.kill_command,
        freeze_worker_command=args.freeze_worker_command,
        unfreeze_worker_command=args.unfreeze_worker_command,
        connect_probe_command=args.connect_probe_command,
        graceful_timeout=args.graceful_timeout,
        recovery_timeout=args.recovery_timeout,
        poll_interval=args.poll_interval,
        artifact_dir=Path(args.artifact_dir),
        shell=args.shell,
    )
    result = harness.run()
    print(json.dumps(asdict(result), indent=2, sort_keys=True))
    return result.exit_code


if __name__ == "__main__":
    sys.exit(main())
