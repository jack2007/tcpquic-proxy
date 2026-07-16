import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "run-dgx-netem-delay-loss-matrix.sh"
EVIDENCE = ROOT / "scripts" / "dgx-netem-evidence.py"


class DgxBackendMatrixTest(unittest.TestCase):
    def test_relay_metrics_capture_has_bounded_backend_readiness_retry(self):
        source = RUNNER.read_text(encoding="utf-8")
        capture = source[
            source.index("capture_relay_metrics() {"):
            source.index("\ncollect_case_evidence() {")
        ]
        self.assertIn("RELAY_METRICS_READY_ATTEMPTS", capture)
        self.assertIn("compiled_relay_backend", capture)
        self.assertIn("relay metrics readiness timed out", capture)

    def test_cache_evidence_normalizes_backend_build_directory(self):
        spec = importlib.util.spec_from_file_location("dgx_netem_evidence", EVIDENCE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        values = []
        for backend in ("native", "libuv"):
            build = self.root / f"cache-normalize-{backend}"
            build.mkdir()
            cache = build / "CMakeCache.txt"
            cache.write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                f"QUIC_OUTPUT_DIR:STRING={build}/msquic/bin/Release\n",
                encoding="utf-8",
            )
            values.append(module.cache_evidence(cache))
        self.assertEqual(values[0], values[1])
        self.assertEqual(values[0]["QUIC_OUTPUT_DIR"],
                         "<BUILD_DIR>/msquic/bin/Release")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.fake_bin = self.root / "fake-bin"
        self.fake_bin.mkdir()
        self.command_log = self.root / "commands.log"
        self.remote_state = self.root / "remote-state"
        self.remote_state.mkdir()
        fake_program = """#!/usr/bin/env python3
import hashlib, json, os, pathlib, re, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["FAKE_COMMAND_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps({"command": name, "argv": args}) + "\\n")
state = pathlib.Path(os.environ["FAKE_REMOTE_STATE"])
if name == "rsync":
    source = pathlib.Path(args[-2])
    remote_name = args[-1].rsplit("/", 1)[-1]
    (state / (remote_name + ".sha")).write_text(hashlib.sha256(source.read_bytes()).hexdigest())
elif name == "ssh":
    remote = args[-1] if args else ""
    if "restart iperf" in remote and os.environ.get("FAKE_IPERF_RESTART_FAIL") == "1":
        raise SystemExit(9)
    match = re.search(r"sha256sum '([^']+)'", remote)
    if match:
        remote_name = pathlib.Path(match.group(1)).name
        hash_file = state / (remote_name + ".sha")
        if not hash_file.exists():
            raise SystemExit(1)
        print(hash_file.read_text())
    elif "tc " in remote or remote.startswith("tc "):
        print("qdisc netem 1: root limit 5000000 delay 10ms loss 5% dropped 4 backlog 1200b 3p")
    elif "/api/v1/memory/allocator:dump" in remote:
        backend = "libuv" if "Expected-Backend: libuv" in remote else "native"
        installed = os.environ.get("FAKE_LIBUV_ALLOCATOR_INSTALLED", "1") == "1"
        body = {"compiled_relay_backend": backend, "status": "dumped"}
        if backend == "libuv":
            system = os.environ.get("FAKE_LIBUV_ALLOCATOR_MODE") == "system"
            body.update({"libuv_allocator_mode":"system" if system else "mimalloc",
                         "libuv_allocator_attempted":False if system else True,
                         "libuv_allocator_in_progress":False,
                         "libuv_allocator_installed":False if system else installed,
                         "libuv_allocator_status":0 if system or installed else -1})
        print(json.dumps(body))
    elif "/api/v1/relay/metrics" in remote:
        backend = "libuv" if "Expected-Backend: libuv" in remote else "native"
        pending = int(os.environ.get("FAKE_RELAY_PENDING_BYTES", "12"))
        print(json.dumps({"compiled_relay_backend":backend,
              "active_relays":0,"relay_pending_bytes":pending,
              "relay_pending_events":0,"relay_outstanding_quic_sends":0,
              "relay_outstanding_quic_send_bytes":0,
              "linux_relay_pending_tcp_write_bytes":0,
              "relay_event_queue_full_errors":0,
              "relay_errors":0,"linux_relay_quic_send_failures":0,
              "relay_snapshot_complete":True}))
    elif remote.startswith("ss "):
        count_file = state / "ss-empty-count"
        count = int(count_file.read_text()) if count_file.exists() else 0
        if os.environ.get("FAKE_SS_EMPTY_ONCE") == "1" and count < 2:
            count_file.write_text(str(count + 1))
        else:
            print("bbr rtt:20.0/2.0 bytes_in_flight:4096 bw:800Mbps")
    elif remote.startswith("ps "):
        print("7.5 2048 10 5")
    elif remote.startswith("cat /home/jack/dgx-netem-server.pid"):
        print("456")
elif name == "tc":
    print("qdisc netem 1: root limit 5000000 delay 10ms loss 5% dropped 4 backlog 1200b 3p")
elif name == "curl":
    backend = "libuv" if "X-Tcpquic-Expected-Backend: libuv" in args else "native"
    installed = os.environ.get("FAKE_LIBUV_ALLOCATOR_INSTALLED", "1") == "1"
    body = {"compiled_relay_backend":backend,"status":"dumped"}
    if any("memory/allocator:dump" in arg for arg in args) and backend == "libuv":
        system = os.environ.get("FAKE_LIBUV_ALLOCATOR_MODE") == "system"
        body.update({"libuv_allocator_mode":"system" if system else "mimalloc",
                     "libuv_allocator_attempted":False if system else True,
                     "libuv_allocator_in_progress":False,
                     "libuv_allocator_installed":False if system else installed,
                     "libuv_allocator_status":0 if system or installed else -1})
    elif any("/api/v1/relay/metrics" in arg for arg in args):
        pending = int(os.environ.get("FAKE_RELAY_PENDING_BYTES", "12"))
        body.update({"active_relays":0,"relay_pending_bytes":pending,
                     "relay_pending_events":0,"relay_outstanding_quic_sends":0,
                     "relay_outstanding_quic_send_bytes":0,
                     "linux_relay_pending_tcp_write_bytes":0,
                     "relay_event_queue_full_errors":0,
                     "relay_errors":0,"linux_relay_quic_send_failures":0,
                     "relay_snapshot_complete":True})
    print(json.dumps(body))
elif name == "ss":
    count_file = state / "ss-empty-count"
    count = int(count_file.read_text()) if count_file.exists() else 0
    if os.environ.get("FAKE_SS_EMPTY_ONCE") == "1" and count < 2:
        count_file.write_text(str(count + 1))
    else:
        print("bbr rtt:20.0/2.0 bytes_in_flight:4096 bw:800Mbps")
elif name == "ps":
    print("12.5 1024 30 20")
elif name == "iperf3":
    marker = state / "iperf-failed-once"
    count_file = state / "iperf-count"
    count = int(count_file.read_text()) + 1 if count_file.exists() else 1
    count_file.write_text(str(count))
    fail_at = int(os.environ.get("FAKE_IPERF_FAIL_AT", "0"))
    fail_count = int(os.environ.get("FAKE_IPERF_FAIL_COUNT", "0"))
    if (fail_at and count == fail_at) or (fail_count and count <= fail_count):
        error = os.environ.get("FAKE_IPERF_ERROR", "synthetic data failure")
        print(json.dumps({"error": error}))
        raise SystemExit(int(os.environ.get("FAKE_IPERF_EXIT_CODE", "7")))
    if os.environ.get("FAKE_IPERF_FAIL_ONCE") == "1" and not marker.exists():
        marker.write_text("1")
        error = os.environ.get("FAKE_IPERF_ERROR", "synthetic data failure")
        print(json.dumps({"error": error}))
        raise SystemExit(int(os.environ.get("FAKE_IPERF_EXIT_CODE", "7")))
    print(json.dumps({"start":{"test_start":{"duration":1}},
          "end":{"sum_sent":{"bits_per_second":101000000,"retransmits":1},
                 "sum_received":{"bits_per_second":100000000}}}))
"""
        for command in ("ssh", "tc", "curl", "rsync", "ss", "ps", "iperf3"):
            path = self.fake_bin / command
            path.write_text(fake_program, encoding="utf-8")
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        self.native_build = self._make_build("native", "mimalloc", "OFF")
        self.libuv_build = self._make_build("libuv", "mimalloc", "OFF")

    def _make_build(self, backend, allocator, sanitizer):
        build = self.root / f"build-{backend}"
        binary = build / "bin" / "Release" / "raypx2"
        binary.parent.mkdir(parents=True)
        binary.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
        (build / "CMakeCache.txt").write_text(
            "\n".join(
                [
                    f"TCPQUIC_RELAY_BACKEND:STRING={backend}",
                    "TCPQUIC_USE_MIMALLOC:BOOL=ON",
                    f"TCPQUIC_SANITIZER:STRING={sanitizer}",
                    "CMAKE_BUILD_TYPE:STRING=Release",
                    "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++",
                    "CMAKE_CXX_COMPILER_VERSION:STRING=14.2.0",
                    f"TCPQUIC_ALLOCATOR:STRING={allocator}",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        (build / "tcpquic-build-provenance.txt").write_text(
            "\n".join(
                [
                    "schema_version=2",
                    "source_commit=1111111111111111111111111111111111111111",
                    "source_tree_state=clean",
                    "source_diff_sha256=" + "0" * 64,
                    "submodule_commits=libuv:aaaa;msquic:bbbb;mimalloc:cccc",
                    f"compiled_relay_backend={backend}",
                    "compiler_path=/usr/bin/c++",
                    "compiler_id=GNU",
                    "compiler_version=14.2.0",
                    "compile_flags=-O3 -DNDEBUG",
                    "link_flags=-static-libgcc",
                    "link_libraries=msquic|uv_a|mimalloc-static",
                    f"source_files=main.cpp@{'1' * 64}|relay_backend_{backend}.cpp@{'2' * 64}",
                    f"backend_source_files=relay_backend_{backend}.cpp@{'2' * 64}",
                    f"common_source_files=main.cpp@{'1' * 64}",
                    "cache_evidence={\"CMAKE_BUILD_TYPE\":\"Release\",\"TCPQUIC_USE_MIMALLOC\":\"ON\"}",
                    "optimization=Release",
                    "build_shared_libs=OFF",
                    "mimalloc=ON",
                    "sanitizer=OFF",
                    "tuning=wan",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        self._seal_build(build, backend)
        return build

    def _seal_build(self, build, backend):
        binary = build / "bin" / "Release" / "raypx2"
        subprocess.run(
            [
                "python3",
                str(EVIDENCE),
                "seal-build",
                "--backend",
                backend,
                "--binary",
                str(binary),
                "--cmake-cache",
                str(build / "CMakeCache.txt"),
                "--provenance",
                str(build / "tcpquic-build-provenance.txt"),
                "--output",
                str(binary) + ".build-manifest.json",
            ],
            check=True,
        )

    def _run(self, *args, result_root=None, check=True):
        env = os.environ.copy()
        env.update(
            {
                "PATH": f"{self.fake_bin}:{env['PATH']}",
                "FAKE_COMMAND_LOG": str(self.command_log),
                "FAKE_REMOTE_STATE": str(self.remote_state),
                "RESULT_ROOT": str(result_root or self.root / "results"),
                "REMOTE_SSH": "fake@example",
                "RUNS": "3",
            }
        )
        result = subprocess.run(
            ["bash", str(RUNNER), *args],
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if check and result.returncode != 0:
            self.fail(result.stdout)
        return result

    def _run_with_env(self, extra_env, *args, result_root=None, check=True):
        old = {key: os.environ.get(key) for key in extra_env}
        os.environ.update(extra_env)
        try:
            return self._run(*args, result_root=result_root, check=check)
        finally:
            for key, value in old.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value

    def _write_minimal_complete_summaries(self, results, run_ids, fail_libuv_run=None):
        fields = [
            "phase", "order", "direction", "delay_ms", "loss_pct", "run",
            "iperf_rc", "received_mbps", "retransmits", "qdisc_drop_delta",
            "qdisc_backlog_max_bytes", "srtt_avg_ms", "bbr_bw_avg_mbps",
            "bytes_in_flight_max", "cpu_pct", "rss_bytes", "context_switches",
            "pending_bytes", "event_queue_full_errors_max", "quic_send_failures",
            "relay_errors", "send_error_scope", "evidence_status", "notes",
        ]
        for backend in ("native", "libuv"):
            path = results / backend / "summary.csv"
            with path.open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                order = 0
                for loss in (5, 10):
                    for delay in (10, 20, 50, 80, 100, 150, 200):
                        for direction in ("download", "upload"):
                            for run in run_ids:
                                order += 1
                                failed = backend == "libuv" and run == fail_libuv_run
                                writer.writerow({
                                    "phase": "matrix", "order": order,
                                    "direction": direction, "delay_ms": delay,
                                    "loss_pct": loss, "run": run,
                                    "iperf_rc": 1 if failed else 0,
                                    "received_mbps": "" if failed else 100 + run,
                                    "retransmits": run, "qdisc_drop_delta": run,
                                    "qdisc_backlog_max_bytes": 1000 + run,
                                    "srtt_avg_ms": delay * 2,
                                    "bbr_bw_avg_mbps": 200 + run,
                                    "bytes_in_flight_max": 300 + run,
                                    "cpu_pct": 10 + run, "rss_bytes": 10000 + run,
                                    "context_switches": 20 + run,
                                    "pending_bytes": 0,
                                    "event_queue_full_errors_max": 0,
                                    "relay_errors": 0,
                                    "quic_send_failures": 0,
                                    "send_error_scope": "quic",
                                    "evidence_status": "complete" if not failed else "incomplete",
                                    "notes": "iperf_rc_1" if failed else "ok",
                                })
        self._write_runtime_result_evidence(results)
        self._write_anchor_order_evidence(results)

    def _write_runtime_result_evidence(self, results):
        for backend in ("native", "libuv"):
            root = results / backend
            proxy = root / "proxy"
            proxy.mkdir(parents=True, exist_ok=True)
            manifest = json.loads(
                (root / "build-metadata.txt.build-manifest.json").read_text(
                    encoding="utf-8"
                )
            )
            allocator = {"compiled_relay_backend": backend, "status": "dumped"}
            if backend == "libuv":
                allocator.update({
                    "libuv_allocator_mode": "mimalloc",
                    "libuv_allocator_attempted": True,
                    "libuv_allocator_in_progress": False,
                    "libuv_allocator_installed": True,
                    "libuv_allocator_status": 0,
                })
            process = {
                "schema_version": 1, "pid": 100 if backend == "native" else 200,
                "starttime_ticks": 777, "exe_path": f"/tmp/raypx2-{backend}",
                "exe_sha256": manifest["binary_sha256"], "exe_device": 1,
                "exe_inode": 1000 if backend == "native" else 2000,
                "captured_unix_ns": 1,
            }
            for side in ("local", "remote"):
                for phase in ("before", "after"):
                    (proxy / f"{side}-allocator.{phase}.json").write_text(
                        json.dumps(allocator), encoding="utf-8"
                    )
                    (proxy / f"{side}-process-image.{phase}.json").write_text(
                        json.dumps(process), encoding="utf-8"
                    )
            metadata_path = root / "build-metadata.txt.json"
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            hashes = {}
            for path in sorted(proxy.glob("*-allocator.*.json")) + sorted(
                proxy.glob("*-process-image.*.json")
            ):
                hashes[path.relative_to(root).as_posix()] = hashlib.sha256(
                    path.read_bytes()
                ).hexdigest()
            metadata["runtime_evidence_sha256"] = hashes
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")

    def _write_anchor_order_evidence(self, results):
        for backend in ("native", "libuv"):
            path = results / backend / "summary.csv"
            with path.open(newline="") as stream:
                reader = csv.DictReader(stream)
                fields = reader.fieldnames
            with path.open("a", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                for phase in ("pre-anchor", "post-anchor"):
                    for direction in ("download", "upload"):
                        row = {field: "" for field in fields}
                        values = {
                            "phase": phase, "order": 0, "direction": direction,
                            "delay_ms": 10, "loss_pct": 5, "run": 1,
                            "iperf_rc": 0, "received_mbps": 100,
                            "retransmits": 0, "qdisc_drop_delta": 0,
                            "qdisc_backlog_max_bytes": 100,
                            "srtt_avg_ms": 20, "bbr_bw_avg_mbps": 100,
                            "bytes_in_flight_max": 100, "cpu_pct": 10,
                            "rss_bytes": 1000, "context_switches": 1,
                            "pending_bytes": 0, "event_queue_full_errors_max": 0,
                            "relay_errors": 0, "quic_send_failures": 0,
                            "send_error_scope": "linux_relay",
                            "evidence_status": "complete", "notes": "ok",
                        }
                        row.update({key: value for key, value in values.items() if key in row})
                        writer.writerow(row)
        (results / "backend-execution-order.csv").write_text(
            "backend,event,timestamp\n"
            "native,start,2026-01-01T00:00:00Z\n"
            "native,end,2026-01-01T00:01:00Z\n"
            "libuv,start,2026-01-01T00:02:00Z\n"
            "libuv,end,2026-01-01T00:03:00Z\n",
            encoding="utf-8",
        )

    def test_rejects_invalid_or_mixed_endpoint_backends(self):
        invalid = self._run("--backend", "other", "--dry-run", check=False)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertIn("backend must be native or libuv", invalid.stdout)

        mixed = self._run(
            "--backend",
            "native",
            "--local-backend",
            "native",
            "--remote-backend",
            "libuv",
            "--dry-run",
            check=False,
        )
        self.assertNotEqual(mixed.returncode, 0)
        self.assertIn("local and remote backend must match", mixed.stdout)

        unsafe = self._run_with_env(
            {"REMOTE_DIR": "/tmp/x';touch /tmp/injected;'"},
            "--backend", "native", "--dry-run", check=False,
        )
        self.assertNotEqual(unsafe.returncode, 0)
        self.assertIn("REMOTE_DIR contains unsafe characters", unsafe.stdout)

        unsafe_admin = self._run_with_env(
            {"SERVER_ADMIN_LISTEN": "127.0.0.1:18081';id;'"},
            "--backend", "native", "--dry-run", check=False,
        )
        self.assertNotEqual(unsafe_admin.returncode, 0)
        self.assertIn("SERVER_ADMIN_LISTEN must be host:port", unsafe_admin.stdout)

        unsafe_delay = self._run_with_env(
            {"DELAYS": "10 20;id"}, "--backend", "native", "--dry-run",
            check=False,
        )
        self.assertNotEqual(unsafe_delay.returncode, 0)
        self.assertIn("DELAYS must contain integers", unsafe_delay.stdout)

        invalid_boundaries = (
            ({"SERVER_ADMIN_LISTEN": "127.0.0.1:0"},
             "SERVER_ADMIN_LISTEN port is out of range"),
            ({"SERVER_ADMIN_LISTEN": "127.0.0.1:65536"},
             "SERVER_ADMIN_LISTEN port is out of range"),
            ({"DELAYS": "60001"}, "DELAYS value is out of range"),
            ({"LOSSES": "x"}, "LOSSES must contain integers"),
            ({"LOSSES": "101"}, "LOSSES value is out of range"),
        )
        for env, message in invalid_boundaries:
            with self.subTest(env=env):
                rejected = self._run_with_env(
                    env, "--backend", "native", "--dry-run", check=False,
                )
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn(message, rejected.stdout)

    def test_sealed_manifest_binds_binary_cache_and_build_provenance(self):
        sealed = self._run(
            "--seal-build",
            "--backend",
            "native",
            "--native-build",
            str(self.native_build),
            check=False,
        )
        self.assertEqual(sealed.returncode, 0, sealed.stdout)
        manifest_path = (
            self.native_build / "bin" / "Release" / "raypx2.build-manifest.json"
        )
        self.assertTrue(manifest_path.is_file())
        manifest = __import__("json").loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["source_commit"], "1" * 40)
        self.assertEqual(manifest["compiled_relay_backend"], "native")
        self.assertIn("binary_sha256", manifest)
        self.assertIn("cmake_cache_sha256", manifest)
        self.assertIn("common_evidence_sha256", manifest)

        binary = self.native_build / "bin" / "Release" / "raypx2"
        binary.write_text("changed after sealing\n", encoding="utf-8")
        rejected = self._run(
            "--backend",
            "native",
            "--native-build",
            str(self.native_build),
            "--libuv-build",
            str(self.libuv_build),
            "--dry-run",
            check=False,
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("binary hash does not match sealed build manifest", rejected.stdout)

    def test_cmake_post_build_seals_raypx2_with_live_link_time_provenance(self):
        cmake = (ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("tcpquic-build-provenance.txt", cmake)
        self.assertIn("dgx-netem-evidence.py", cmake)
        self.assertIn("seal-live-build", cmake)
        self.assertIn("LINK_DEPENDS", cmake)
        self.assertIn('BYPRODUCTS\n            "${CMAKE_BINARY_DIR}/tcpquic-build-provenance.txt"\n            "${TCPQUIC_OUTPUT_DIR}/raypx2.build-manifest.json"', cmake)
        self.assertNotIn('file(WRITE "${CMAKE_BINARY_DIR}/tcpquic-build-provenance.txt"', cmake)
        self.assertIn("$<TARGET_FILE:tcpquic-proxy>", cmake)

        if not shutil.which("ninja"):
            self.skipTest("Ninja is required for generated graph BYPRODUCT verification")
        build = self.root / "cmake-ninja-byproduct"
        configured = subprocess.run(
            ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", "-DTCPQUIC_RELAY_BACKEND=native",
             "-DTCPQUIC_ENABLE_CRASHPAD=OFF", "-DBUILD_TESTING=OFF",
             "-DENABLE_CLANG_TIDY=OFF"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.assertEqual(configured.returncode, 0, configured.stdout)
        queried = subprocess.run(
            ["ninja", "-C", str(build), "-t", "query",
             "bin/Release/raypx2.build-manifest.json"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.assertEqual(queried.returncode, 0, queried.stdout)
        self.assertIn("input: CXX_EXECUTABLE_LINKER__tcpquic-proxy_Release", queried.stdout)

    def test_live_link_seal_observes_incremental_dirty_source(self):
        repo = self.root / "live-repo"
        repo.mkdir()
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(["git", "-C", str(repo), "config", "user.email", "test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(repo), "config", "user.name", "Test"], check=True)
        source = repo / "main.cpp"
        backend_source = repo / "relay_backend_native.cpp"
        source.write_text("int common() { return 1; }\n", encoding="utf-8")
        backend_source.write_text("int backend() { return 1; }\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
        subprocess.run(["git", "-C", str(repo), "commit", "-qm", "base"], check=True)
        build = self.root / "live-build"
        build.mkdir()
        binary = build / "raypx2"
        binary.write_bytes(b"first-link")
        cache = build / "CMakeCache.txt"
        cache.write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\n"
            "TCPQUIC_USE_MIMALLOC:BOOL=ON\n",
            encoding="utf-8",
        )
        manifest = build / "raypx2.build-manifest.json"
        command = [
            "python3", str(EVIDENCE), "seal-live-build", "--backend", "native",
            "--binary", str(binary), "--cmake-cache", str(cache),
            "--source-root", str(repo), "--source-base", str(repo),
            "--target-sources", "main.cpp|relay_backend_native.cpp",
            "--backend-sources", "relay_backend_native.cpp",
            "--link-libraries", "uv_a|mimalloc-static",
            "--provenance", str(build / "tcpquic-build-provenance.txt"),
            "--output", str(manifest),
        ]
        subprocess.run(command, check=True)
        self.assertEqual(json.loads(manifest.read_text())["source_tree_state"], "clean")
        source.write_text("int common() { return 2; }\n", encoding="utf-8")
        binary.write_bytes(b"incremental-link")
        subprocess.run(command, check=True)
        dirty = json.loads(manifest.read_text())
        self.assertEqual(dirty["source_tree_state"], "dirty")
        self.assertIn("main.cpp@", dirty["common_source_files"])

    def test_dry_run_writes_backend_metadata_and_complete_fixed_matrix(self):
        results = self.root / "eval"
        common = (
            "--rounds",
            "3",
            "--matrix",
            "full",
            "--native-build",
            str(self.native_build),
            "--libuv-build",
            str(self.libuv_build),
            "--dry-run",
        )
        self._run("--backend", "native", *common, result_root=results)
        self._run("--backend", "libuv", *common, result_root=results)

        required = {
            "commit",
            "submodule_commits",
            "compiled_relay_backend",
            "local_backend",
            "remote_backend",
            "cmake_cache_path",
            "cmake_cache_sha256",
            "compiler_path",
            "compiler_id",
            "compiler_version",
            "optimization",
            "binary_sha256",
            "remote_binary_sha256",
            "tuning",
            "mimalloc",
            "sanitizer",
            "common_evidence_sha256",
        }
        for backend in ("native", "libuv"):
            backend_root = results / backend
            metadata = dict(
                line.split("=", 1)
                for line in (backend_root / "build-metadata.txt").read_text(
                    encoding="utf-8"
                ).splitlines()
                if "=" in line
            )
            self.assertTrue(required.issubset(metadata), metadata)
            self.assertEqual(metadata["compiled_relay_backend"], backend)
            self.assertEqual(metadata["local_backend"], backend)
            self.assertEqual(metadata["remote_backend"], backend)
            case_dirs = list((backend_root / "cases").iterdir())
            self.assertEqual(len(case_dirs), 14 * 2 * 3 + 4)
            with (backend_root / "summary.csv").open(newline="") as stream:
                phases = [row["phase"] for row in csv.DictReader(stream)]
            self.assertEqual(phases.count("pre-anchor"), 2)
            self.assertEqual(phases.count("matrix"), 14 * 2 * 3)
            self.assertEqual(phases.count("post-anchor"), 2)
            self.assertTrue((backend_root / "proxy" / "local-allocator.before.json").is_file())
            self.assertTrue((backend_root / "proxy" / "remote-allocator.after.json").is_file())
            summary_header = (backend_root / "summary.csv").read_text(
                encoding="utf-8"
            ).splitlines()[0]
            for field in (
                "qdisc_drop_delta",
                "qdisc_backlog_max_bytes",
                "srtt_avg_ms",
                "bbr_bw_avg_mbps",
                "bytes_in_flight_max",
                "bytes_in_flight_source",
                "cpu_pct",
                "rss_bytes",
                "context_switches",
                "pending_bytes",
                "event_queue_full_errors_max",
                "quic_send_failures",
            ):
                self.assertIn(field, summary_header)

        commands = [
            json.loads(line)
            for line in self.command_log.read_text(encoding="utf-8").splitlines()
        ]
        names = {entry["command"] for entry in commands}
        self.assertTrue(
            {"ssh", "rsync", "tc", "curl", "ss", "iperf3"}.issubset(names)
        )
        self.assertTrue(any(
            entry["command"] == "ss" and str(18080) in " ".join(entry["argv"])
            for entry in commands
        ))
        self.assertTrue(
            any(
                entry["command"] == "tc"
                and entry["argv"][-2:] == ["dev", "enp1s0f0np0"]
                for entry in commands
            )
        )
        execution = (results / "backend-execution-order.csv").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            execution,
            r"native,start,.*\nnative,end,.*\nlibuv,start,.*\nlibuv,end,",
        )
        self.assertTrue(
            any(
                entry["command"] == "ssh"
                and "raypx2-libuv" in entry["argv"][-1]
                for entry in commands
            )
        )
        compared = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertEqual(compared.returncode, 0, compared.stdout)

    def test_libuv_runtime_allocator_failure_rejects_dry_run_evidence(self):
        result = self._run_with_env(
            {"FAKE_LIBUV_ALLOCATOR_INSTALLED": "0"},
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=self.root / "allocator-failure", check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libuv mimalloc replacement is not installed", result.stdout)

    def test_failed_case_with_missing_transport_evidence_is_recorded_and_continues(self):
        results = self.root / "continued-failure"
        result = self._run_with_env(
            {"FAKE_IPERF_FAIL_ONCE": "1", "FAKE_SS_EMPTY_ONCE": "1",
             "CONTINUE_ON_CASE_FAILURE": "1", "DELAYS": "10", "LOSSES": "5",
             "DATA_IFACE": "dgx-test0"},
            "--backend", "native", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        with (results / "native" / "summary.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 10)
        self.assertEqual(rows[0]["iperf_rc"], "7")
        self.assertEqual(rows[0]["notes"], "iperf_rc_7")
        self.assertEqual(rows[0]["evidence_status"], "incomplete")
        self.assertIn("srtt", rows[0]["evidence_missing"])
        self.assertTrue(any(row["notes"] == "ok" for row in rows[1:]))
        commands = [json.loads(line) for line in self.command_log.read_text().splitlines()]
        qdisc_remote = [entry["argv"][-1] for entry in commands
                        if entry["command"] == "ssh" and "qdisc" in entry["argv"][-1]]
        self.assertTrue(qdisc_remote)
        self.assertTrue(all("dgx-test0" in command for command in qdisc_remote))
        self.assertTrue(all("enp1s0f0np0" not in command for command in qdisc_remote))

    def test_retries_one_healthy_iperf_control_failure(self):
        results = self.root / "control-retry"
        result = self._run_with_env(
            {
                "FAKE_IPERF_FAIL_ONCE": "1",
                "FAKE_IPERF_ERROR": (
                    "unable to receive cookie at server: Bad file descriptor"
                ),
                "FAKE_RELAY_PENDING_BYTES": "0",
                "DELAYS": "10",
                "LOSSES": "5",
                "DATA_IFACE": "dgx-test0",
            },
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        case = results / "libuv" / "cases" / "order-0001-pre-anchor-download-010ms-5loss-run1"
        self.assertEqual((case / "attempt-1" / "iperf.rc").read_text().strip(), "7")
        self.assertIn(
            "unable to receive cookie",
            (case / "attempt-1" / "iperf.stdout.json").read_text(),
        )
        self.assertTrue((case / "attempt-1" / "local-relay-metrics.json").is_file())
        self.assertTrue((case / "attempt-1" / "remote-relay-metrics.json").is_file())
        for name in (
            "local-qdisc-before.txt", "local-qdisc-after.txt",
            "local-qdisc-samples.txt", "local-ss.txt", "local-process.txt",
            "local-relay-metrics.before.json",
        ):
            self.assertTrue((case / "attempt-1" / name).is_file(), name)
        self.assertEqual(
            len((case / "local-process.txt").read_text().splitlines()), 2
        )
        anomalies = (results / "libuv" / "known-anomalies.txt").read_text()
        self.assertIn("iperf control transient", anomalies)
        commands = [json.loads(line) for line in self.command_log.read_text().splitlines()]
        self.assertEqual(sum(entry["command"] == "iperf3" for entry in commands), 11)
        self.assertTrue(any(
            entry["command"] == "ssh" and "restart iperf" in entry["argv"][-1]
            for entry in commands
        ))

    def test_does_not_retry_unclassified_iperf_failure(self):
        results = self.root / "unclassified-no-retry"
        result = self._run_with_env(
            {
                "FAKE_IPERF_FAIL_ONCE": "1",
                "FAKE_IPERF_ERROR": "synthetic data failure",
                "FAKE_RELAY_PENDING_BYTES": "0",
                "DELAYS": "10",
                "LOSSES": "5",
                "DATA_IFACE": "dgx-test0",
            },
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        commands = [json.loads(line) for line in self.command_log.read_text().splitlines()]
        self.assertEqual(sum(entry["command"] == "iperf3" for entry in commands), 1)
        self.assertFalse(any(
            entry["command"] == "ssh" and "restart iperf" in entry["argv"][-1]
            for entry in commands
        ))

    def test_timeout_with_control_text_is_not_retried(self):
        results = self.root / "timeout-no-retry"
        result = self._run_with_env(
            {
                "FAKE_IPERF_FAIL_ONCE": "1",
                "FAKE_IPERF_ERROR": "unable to receive cookie",
                "FAKE_RELAY_PENDING_BYTES": "0",
                "FAKE_IPERF_EXIT_CODE": "124",
                "DELAYS": "10", "LOSSES": "5", "DATA_IFACE": "dgx-test0",
            },
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        commands = [json.loads(line) for line in self.command_log.read_text().splitlines()]
        self.assertEqual(sum(entry["command"] == "iperf3" for entry in commands), 1)

    def test_control_retry_can_be_disabled(self):
        results = self.root / "control-retry-disabled"
        result = self._run_with_env(
            {
                "FAKE_IPERF_FAIL_ONCE": "1",
                "FAKE_IPERF_ERROR": "unable to receive cookie",
                "FAKE_RELAY_PENDING_BYTES": "0",
                "IPERF_CONTROL_RETRIES": "0",
                "DELAYS": "10", "LOSSES": "5", "DATA_IFACE": "dgx-test0",
            },
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        commands = [json.loads(line) for line in self.command_log.read_text().splitlines()]
        self.assertEqual(sum(entry["command"] == "iperf3" for entry in commands), 1)

    def test_signal_exit_with_control_text_is_not_retried(self):
        results = self.root / "signal-no-retry"
        result = self._run_with_env(
            {
                "FAKE_IPERF_FAIL_ONCE": "1",
                "FAKE_IPERF_ERROR": "unable to receive cookie",
                "FAKE_RELAY_PENDING_BYTES": "0",
                "FAKE_IPERF_EXIT_CODE": "143",
                "DELAYS": "10", "LOSSES": "5", "DATA_IFACE": "dgx-test0",
            },
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        commands = [json.loads(line) for line in self.command_log.read_text().splitlines()]
        self.assertEqual(sum(entry["command"] == "iperf3" for entry in commands), 1)

    def test_second_control_failure_is_blocking_even_when_continue_is_enabled(self):
        results = self.root / "second-control-failure"
        result = self._run_with_env(
            {
                "FAKE_IPERF_FAIL_COUNT": "2",
                "FAKE_IPERF_ERROR": "unable to receive cookie",
                "FAKE_RELAY_PENDING_BYTES": "0",
                "CONTINUE_ON_CASE_FAILURE": "1",
                "DELAYS": "10", "LOSSES": "5", "DATA_IFACE": "dgx-test0",
            },
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        commands = [json.loads(line) for line in self.command_log.read_text().splitlines()]
        self.assertEqual(sum(entry["command"] == "iperf3" for entry in commands), 2)

    def test_unhealthy_relay_blocks_control_retry_even_when_continue_is_enabled(self):
        results = self.root / "unhealthy-control-failure"
        result = self._run_with_env(
            {
                "FAKE_IPERF_FAIL_ONCE": "1",
                "FAKE_IPERF_ERROR": "unable to receive cookie",
                "FAKE_RELAY_PENDING_BYTES": "12",
                "CONTINUE_ON_CASE_FAILURE": "1",
                "DELAYS": "10", "LOSSES": "5", "DATA_IFACE": "dgx-test0",
            },
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        commands = [json.loads(line) for line in self.command_log.read_text().splitlines()]
        self.assertEqual(sum(entry["command"] == "iperf3" for entry in commands), 1)

    def test_retry_restart_failure_uses_standard_blocking_path(self):
        results = self.root / "restart-failure"
        result = self._run_with_env(
            {
                "FAKE_IPERF_FAIL_ONCE": "1",
                "FAKE_IPERF_ERROR": "unable to receive cookie",
                "FAKE_RELAY_PENDING_BYTES": "0",
                "FAKE_IPERF_RESTART_FAIL": "1",
                "CONTINUE_ON_CASE_FAILURE": "1",
                "DELAYS": "10", "LOSSES": "5", "DATA_IFACE": "dgx-test0",
            },
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        case = results / "libuv" / "cases" / "order-0001-pre-anchor-download-010ms-5loss-run1"
        self.assertEqual(
            (case / "freeze" / "reason.txt").read_text().strip(),
            "iperf_server_restart_failed",
        )

    def test_two_invocation_resume_preserves_global_order_process_and_final_validation(self):
        results = self.root / "resumed-eval"
        common = (
            "--backend", "native", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
        )
        first = self._run_with_env(
            {"FAKE_IPERF_FAIL_AT": "5"}, *common,
            result_root=results, check=False,
        )
        self.assertNotEqual(first.returncode, 0)
        with (results / "native" / "summary.csv").open(newline="") as stream:
            first_rows = list(csv.DictReader(stream))
        self.assertEqual([int(row["order"]) for row in first_rows], [1, 2, 3, 4])
        original_process = (
            results / "native" / "proxy" / "local-process-image.before.json"
        ).read_bytes()

        resumed = self._run_with_env(
            {"APPEND_SUMMARY": "1", "RESUME_AFTER_ORDER": "4",
             "SKIP_START_SERVICES": "1", "SKIP_PREFLIGHT": "1"},
            *common, result_root=results, check=False,
        )
        self.assertEqual(resumed.returncode, 0, resumed.stdout)
        self.assertEqual(
            (results / "native" / "proxy" / "local-process-image.before.json").read_bytes(),
            original_process,
        )
        with (results / "native" / "summary.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual([int(row["order"]) for row in rows], list(range(1, 89)))
        self.assertEqual(len({(row["phase"], row["direction"], row["delay_ms"],
                              row["loss_pct"], row["run"]) for row in rows}), 88)
        self.assertEqual(sum(row["phase"] == "pre-anchor" for row in rows), 2)
        self.assertEqual(sum(row["phase"] == "post-anchor" for row in rows), 2)
        with (results / "native" / "execution-order.csv").open(newline="") as stream:
            local_order = list(csv.DictReader(stream))
        self.assertEqual([(row["backend"], row["event"]) for row in local_order],
                         [("native", "start"), ("native", "end")])

        self._run(
            "--backend", "libuv", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
            result_root=results,
        )
        compared = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertEqual(compared.returncode, 0, compared.stdout)

    def test_resume_rejects_changed_process_identity_before_appending(self):
        common = (
            "--backend", "native", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
        )
        changes = {
            "starttime": {"FAKE_PROCESS_STARTTIME": "888"},
            "pid": {"FAKE_PROCESS_PID_DELTA": "1"},
            "device": {"FAKE_PROCESS_DEVICE": "2"},
            "inode": {"FAKE_PROCESS_INODE_DELTA": "1"},
            "exe-sha": {"FAKE_PROCESS_SHA256": "f" * 64},
        }
        for name, change in changes.items():
            with self.subTest(identity_field=name):
                results = self.root / f"resume-process-change-{name}"
                (self.remote_state / "iperf-count").unlink(missing_ok=True)
                first = self._run_with_env(
                    {"FAKE_IPERF_FAIL_AT": "5", "DELAYS": "10", "LOSSES": "5"},
                    *common, result_root=results, check=False,
                )
                self.assertNotEqual(first.returncode, 0)
                protected = [
                    results / "native" / "summary.csv",
                    results / "native" / "scenario-order.csv",
                    results / "native" / "execution-order.csv",
                ]
                before = {path: path.read_bytes() for path in protected}
                rejected = self._run_with_env(
                    {"APPEND_SUMMARY": "1", "RESUME_AFTER_ORDER": "4",
                     "SKIP_START_SERVICES": "1", "SKIP_PREFLIGHT": "1",
                     "DELAYS": "10", "LOSSES": "5", **change},
                    *common, result_root=results, check=False,
                )
                self.assertNotEqual(rejected.returncode, 0)
                self.assertTrue(
                    "resume process identity changed on local" in rejected.stdout
                    or "running process image does not match sealed binary" in rejected.stdout,
                    rejected.stdout,
                )
                self.assertEqual({path: path.read_bytes() for path in protected}, before)

    def test_resume_rejects_tampered_metadata_before_appending(self):
        results = self.root / "resume-metadata-change"
        common = (
            "--backend", "native", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
        )
        first = self._run_with_env(
            {"FAKE_IPERF_FAIL_AT": "5", "DELAYS": "10", "LOSSES": "5"},
            *common, result_root=results, check=False,
        )
        self.assertNotEqual(first.returncode, 0)
        protected = [
            results / "native" / "summary.csv",
            results / "native" / "scenario-order.csv",
            results / "native" / "execution-order.csv",
        ]
        before = {path: path.read_bytes() for path in protected}
        metadata_path = results / "native" / "build-metadata.txt.json"
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        metadata["compiled_relay_backend"] = "libuv"
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")

        rejected = self._run_with_env(
            {"APPEND_SUMMARY": "1", "RESUME_AFTER_ORDER": "4",
             "SKIP_START_SERVICES": "1", "SKIP_PREFLIGHT": "1",
             "DELAYS": "10", "LOSSES": "5"},
            *common, result_root=results, check=False,
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("resume metadata differs", rejected.stdout)
        self.assertEqual({path: path.read_bytes() for path in protected}, before)

    def test_resume_rejects_tampered_text_metadata_before_appending(self):
        results = self.root / "resume-text-metadata-change"
        common = (
            "--backend", "native", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
        )
        first = self._run_with_env(
            {"FAKE_IPERF_FAIL_AT": "5", "DELAYS": "10", "LOSSES": "5"},
            *common, result_root=results, check=False,
        )
        self.assertNotEqual(first.returncode, 0)
        protected = [
            results / "native" / "summary.csv",
            results / "native" / "scenario-order.csv",
            results / "native" / "execution-order.csv",
        ]
        before = {path: path.read_bytes() for path in protected}
        metadata_path = results / "native" / "build-metadata.txt"
        metadata_path.write_text(
            metadata_path.read_text(encoding="utf-8").replace(
                "compiled_relay_backend=native", "compiled_relay_backend=libuv"
            ),
            encoding="utf-8",
        )
        rejected = self._run_with_env(
            {"APPEND_SUMMARY": "1", "RESUME_AFTER_ORDER": "4",
             "SKIP_START_SERVICES": "1", "SKIP_PREFLIGHT": "1",
             "DELAYS": "10", "LOSSES": "5"},
            *common, result_root=results, check=False,
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("resume text metadata differs", rejected.stdout)
        self.assertEqual({path: path.read_bytes() for path in protected}, before)

    def test_real_proc_process_snapshot_preserves_and_rejects_identity(self):
        before = self.root / "real-process.before.json"
        same = self.root / "real-process.same.json"
        different = self.root / "real-process.different.json"
        processes = []
        try:
            first = subprocess.Popen(
                ["python3", "-c", "import time; time.sleep(30)"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            processes.append(first)
            expected = hashlib.sha256(
                (Path("/proc") / str(first.pid) / "exe").read_bytes()
            ).hexdigest()
            for output in (before, same):
                subprocess.run(
                    ["python3", str(EVIDENCE), "capture-process", "--pid",
                     str(first.pid), "--output", str(output)],
                    check=True,
                )
            stable = subprocess.run(
                ["python3", str(EVIDENCE), "validate-process", "--before",
                 str(before), "--after", str(same), "--expected-sha256", expected],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertEqual(stable.returncode, 0, stable.stdout)

            second = subprocess.Popen(
                ["python3", "-c", "import time; time.sleep(30)"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            processes.append(second)
            subprocess.run(
                ["python3", str(EVIDENCE), "capture-process", "--pid",
                 str(second.pid), "--output", str(different)],
                check=True,
            )
            changed = subprocess.run(
                ["python3", str(EVIDENCE), "validate-process", "--before",
                 str(before), "--after", str(different), "--expected-sha256", expected],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("running process image changed during matrix", changed.stdout)
        finally:
            for process in processes:
                process.terminate()
            for process in processes:
                process.wait(timeout=5)

    def test_resume_fails_closed_on_duplicate_existing_summary_order(self):
        results = self.root / "resume-duplicate"
        common = (
            "--backend", "native", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), "--dry-run",
        )
        first = self._run_with_env(
            {"FAKE_IPERF_FAIL_AT": "5", "DELAYS": "10", "LOSSES": "5"},
            *common, result_root=results, check=False,
        )
        self.assertNotEqual(first.returncode, 0)
        summary = results / "native" / "summary.csv"
        lines = summary.read_text(encoding="utf-8").splitlines()
        summary.write_text("\n".join(lines + [lines[1]]) + "\n", encoding="utf-8")
        rejected = self._run_with_env(
            {"APPEND_SUMMARY": "1", "RESUME_AFTER_ORDER": "4",
             "SKIP_START_SERVICES": "1", "SKIP_PREFLIGHT": "1",
             "DELAYS": "10", "LOSSES": "5"},
            *common, result_root=results, check=False,
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("resume summary must contain exactly unique completed orders", rejected.stdout)

    def test_raw_case_evidence_producer_parses_tc_ss_process_and_admin(self):
        case = self.root / "raw-case"
        case.mkdir()
        (case / "qdisc-before.txt").write_text(
            "qdisc netem root dropped 10 backlog 100b 1p\n", encoding="utf-8")
        (case / "qdisc-after.txt").write_text(
            "qdisc netem root dropped 16 backlog 900b 2p\n", encoding="utf-8")
        (case / "qdisc-samples.txt").write_text(
            "backlog 1200b 3p dropped 14\nbacklog 800b 2p dropped 16\n",
            encoding="utf-8",
        )
        (case / "ss.txt").write_text(
            "bbr wscale:7,7 rtt:20.5/4.0 bytes_in_flight:4096 bw:800Mbps\n"
            "rtt:22/5 bytes_in_flight:8192 bw:1.2Gbps\n",
            encoding="utf-8",
        )
        (case / "process.txt").write_text(
            "local,before,1000000000,100,1024,30,100,4096\n"
            "local,after,2000000000,110,1050,36,100,4096\n"
            "remote,before,1000000000,200,2048,10,100,4096\n"
            "remote,after,2000000000,220,2050,15,100,4096\n",
            encoding="utf-8")
        metrics = {
            "compiled_relay_backend": "libuv",
            "relay_pending_bytes": 321,
            "relay_event_queue_full_errors": 2,
            "relay_errors": 3,
            "linux_relay_quic_send_failures": 1,
            "relay_snapshot_complete": True,
        }
        (case / "metrics.json").write_text(json.dumps(metrics), encoding="utf-8")
        output = case / "evidence.json"
        result = subprocess.run(
            ["python3", str(EVIDENCE), "parse-case", "--case-dir", str(case),
             "--output", str(output)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        evidence = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(evidence["qdisc_drop_delta"], 6)
        self.assertEqual(evidence["qdisc_backlog_max_bytes"], 1200)
        self.assertEqual(evidence["srtt_avg_ms"], 21.25)
        self.assertEqual(evidence["srtt_max_ms"], 22.0)
        self.assertEqual(evidence["bbr_bw_avg_mbps"], 1000.0)
        self.assertEqual(evidence["bytes_in_flight_max"], 8192)
        self.assertEqual(evidence["cpu_pct"], 30.0)
        self.assertEqual(evidence["rss_bytes"], (1050 + 2050) * 4096)
        self.assertEqual(evidence["context_switches"], 11)
        self.assertEqual(evidence["pending_bytes"], 321)
        self.assertEqual(evidence["event_queue_full_errors_max"], 2)
        self.assertEqual(evidence["quic_send_failures"], 1)

        (case / "ss.txt").write_text(
            "bbr rtt:20/2 mss:1400 unacked:7 bw:800Mbps\n",
            encoding="utf-8",
        )
        derived = subprocess.run(
            ["python3", str(EVIDENCE), "parse-case", "--case-dir", str(case),
             "--output", str(output)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.assertEqual(derived.returncode, 0, derived.stdout)
        evidence = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(evidence["bytes_in_flight_max"], 9800)
        self.assertEqual(evidence["bytes_in_flight_source"], "derived_unacked_mss")

        (case / "ss.txt").write_text("missing transport metrics\n", encoding="utf-8")
        missing = subprocess.run(
            ["python3", str(EVIDENCE), "parse-case", "--case-dir", str(case),
             "--output", str(output)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("data-validity missing", missing.stdout)

    def test_metadata_detects_address_sanitizer_and_system_allocator(self):
        cache = self.libuv_build / "CMakeCache.txt"
        contents = cache.read_text(encoding="utf-8")
        contents = contents.replace("TCPQUIC_USE_MIMALLOC:BOOL=ON", "TCPQUIC_USE_MIMALLOC:BOOL=OFF")
        contents = contents.replace("TCPQUIC_SANITIZER:STRING=OFF\n", "")
        contents += "CMAKE_CXX_FLAGS:STRING=-fsanitize=address\n"
        cache.write_text(contents, encoding="utf-8")
        provenance = self.libuv_build / "tcpquic-build-provenance.txt"
        contents = provenance.read_text(encoding="utf-8")
        contents = contents.replace("mimalloc=ON", "mimalloc=OFF")
        contents = contents.replace("sanitizer=OFF", "sanitizer=address")
        provenance.write_text(contents, encoding="utf-8")
        self._seal_build(self.libuv_build, "libuv")
        results = self.root / "asan-eval"
        self._run_with_env(
            {"FAKE_LIBUV_ALLOCATOR_MODE": "system"},
            "--backend",
            "libuv",
            "--native-build",
            str(self.native_build),
            "--libuv-build",
            str(self.libuv_build),
            "--dry-run",
            result_root=results,
        )
        metadata = dict(
            line.split("=", 1)
            for line in (results / "libuv" / "build-metadata.txt").read_text(
                encoding="utf-8"
            ).splitlines()
            if "=" in line
        )
        self.assertEqual(metadata["sanitizer"], "address")
        self.assertEqual(metadata["mimalloc"], "OFF")

    def test_preflight_accepts_comparable_builds_and_rejects_compiler_drift(self):
        results = self.root / "preflight"
        args = (
            "--preflight",
            "--native-build",
            str(self.native_build),
            "--libuv-build",
            str(self.libuv_build),
        )
        accepted = self._run(*args, result_root=results)
        self.assertIn("preflight metadata is consistent", accepted.stdout)
        self.assertTrue((results / "native" / "build-metadata.txt").is_file())
        self.assertTrue((results / "libuv" / "build-metadata.txt").is_file())

        cache = self.libuv_build / "CMakeCache.txt"
        cache.write_text(
            cache.read_text(encoding="utf-8").replace(
                "CMAKE_CXX_COMPILER_VERSION:STRING=14.2.0",
                "CMAKE_CXX_COMPILER_VERSION:STRING=15.0.0",
            ),
            encoding="utf-8",
        )
        rejected = self._run(*args, result_root=results, check=False)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("CMake cache hash does not match sealed build manifest", rejected.stdout)

    def test_preflight_rejects_source_submodule_link_and_allocator_drift(self):
        fields = {
            "source_commit": "2222222222222222222222222222222222222222",
            "submodule_commits": "libuv:dddd;msquic:bbbb;mimalloc:cccc",
            "link_flags": "-static-libgcc -Wl,--as-needed",
            "link_libraries": "msquic|uv_a|unexpected-library|mimalloc-static",
            "common_source_files": f"main.cpp@{'9' * 64}",
            "mimalloc": "OFF",
        }
        for field, replacement in fields.items():
            with self.subTest(field=field):
                provenance = self.libuv_build / "tcpquic-build-provenance.txt"
                original = provenance.read_text(encoding="utf-8")
                lines = []
                for line in original.splitlines():
                    lines.append(f"{field}={replacement}" if line.startswith(field + "=") else line)
                provenance.write_text("\n".join(lines) + "\n", encoding="utf-8")
                self._seal_build(self.libuv_build, "libuv")
                result = self._run(
                    "--preflight",
                    "--native-build",
                    str(self.native_build),
                    "--libuv-build",
                    str(self.libuv_build),
                    result_root=self.root / f"drift-{field}",
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("common sealed build evidence differs", result.stdout)
                provenance.write_text(original, encoding="utf-8")
                self._seal_build(self.libuv_build, "libuv")

    def test_preflight_rejects_dirty_source_build(self):
        provenance = self.libuv_build / "tcpquic-build-provenance.txt"
        provenance.write_text(
            provenance.read_text(encoding="utf-8").replace(
                "source_tree_state=clean", "source_tree_state=dirty"
            ),
            encoding="utf-8",
        )
        self._seal_build(self.libuv_build, "libuv")
        result = self._run(
            "--preflight", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build),
            result_root=self.root / "dirty-source", check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("performance comparison requires clean source trees", result.stdout)

    def test_compare_only_uses_three_run_medians_and_never_decides_backend(self):
        results = self.root / "eval"
        self._run(
            "--preflight",
            "--native-build",
            str(self.native_build),
            "--libuv-build",
            str(self.libuv_build),
            result_root=results,
        )
        values = {
            "native": [80.0, 100.0, 120.0],
            "libuv": [90.0, 110.0, 130.0],
        }
        fields = [
            "phase", "order",
            "direction",
            "delay_ms",
            "loss_pct",
            "run",
            "iperf_rc",
            "received_mbps",
            "retransmits",
            "qdisc_drop_delta",
            "qdisc_backlog_max_bytes",
            "srtt_avg_ms",
            "bbr_bw_avg_mbps",
            "bytes_in_flight_max",
            "cpu_pct",
            "rss_bytes",
            "context_switches",
            "pending_bytes",
            "event_queue_full_errors_max",
            "relay_errors",
            "quic_send_failures",
            "send_error_scope",
            "evidence_status",
            "notes",
        ]
        for backend, samples in values.items():
            backend_root = results / backend
            backend_root.mkdir(parents=True, exist_ok=True)
            with (backend_root / "summary.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                order = 0
                for loss in (5, 10):
                    for delay in (10, 20, 50, 80, 100, 150, 200):
                        for direction in ("download", "upload"):
                            for run, received in enumerate(samples, 1):
                                order += 1
                                writer.writerow(
                                    {
                                        "phase": "matrix",
                                        "order": order,
                                        "direction": direction,
                                        "delay_ms": delay,
                                        "loss_pct": loss,
                                        "run": run,
                                        "iperf_rc": 0,
                                        "received_mbps": received,
                                        "retransmits": run,
                                        "qdisc_drop_delta": run * 2,
                                        "qdisc_backlog_max_bytes": run * 3,
                                        "srtt_avg_ms": delay * 2,
                                        "bbr_bw_avg_mbps": received + 5,
                                        "bytes_in_flight_max": run * 100,
                                        "cpu_pct": 20 + run,
                                        "rss_bytes": 1000 + run,
                                        "context_switches": 200 + run,
                                        "pending_bytes": 300 + run,
                                        "event_queue_full_errors_max": 0,
                                        "relay_errors": 0,
                                        "quic_send_failures": 0,
                                        "send_error_scope": "quic",
                                        "evidence_status": "complete",
                                        "notes": "ok",
                                    }
                                )

        self._write_runtime_result_evidence(results)
        self._write_anchor_order_evidence(results)

        self._run("--compare-only", str(results), result_root=results)
        with (results / "comparison.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 14 * 2)
        first = rows[0]
        self.assertEqual(first["native_runs"], "3")
        self.assertEqual(first["libuv_runs"], "3")
        self.assertEqual(first["native_median_mbps"], "100.00")
        self.assertEqual(first["libuv_median_mbps"], "110.00")
        self.assertEqual(first["libuv_delta_pct"], "10.00")
        for field in (
            "native_retransmits_median",
            "libuv_qdisc_drop_delta_median",
            "native_cpu_pct_median",
            "libuv_rss_bytes_median",
            "native_context_switches_median",
            "libuv_pending_bytes_median",
            "native_event_queue_full_errors_max",
            "libuv_quic_send_failures_max",
            "native_compiled_backend",
            "libuv_compiled_backend",
        ):
            self.assertIn(field, first)

        report = (results / "comparison.md").read_text(encoding="utf-8").lower()
        self.assertNotIn("backend_pass", report)
        self.assertNotIn("backend_fail", report)
        self.assertNotIn("淘汰线", report)
        self.assertIn("不作自动取舍结论", report)

        metadata_path = results / "libuv" / "build-metadata.txt.json"
        swapped = json.loads(metadata_path.read_text(encoding="utf-8"))
        swapped["compiled_relay_backend"] = "native"
        metadata_path.write_text(json.dumps(swapped), encoding="utf-8")
        mismatch = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertNotEqual(mismatch.returncode, 0)
        self.assertIn("metadata backend mismatch", mismatch.stdout)

    def test_compare_only_rejects_incomplete_fixed_matrix(self):
        results = self.root / "incomplete"
        self._run(
            "--preflight",
            "--native-build",
            str(self.native_build),
            "--libuv-build",
            str(self.libuv_build),
            result_root=results,
        )
        for backend in ("native", "libuv"):
            backend_root = results / backend
            backend_root.mkdir(parents=True, exist_ok=True)
            (backend_root / "summary.csv").write_text(
                "phase,direction,delay_ms,loss_pct,run,iperf_rc,received_mbps,evidence_status,notes\n"
                "matrix,download,10,5,1,0,100,complete,ok\n"
                "matrix,download,10,5,2,0,101,complete,ok\n",
                encoding="utf-8",
            )
        self._write_runtime_result_evidence(results)
        self._write_anchor_order_evidence(results)
        result = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incomplete fixed matrix", result.stdout)

    def test_compare_rejects_duplicate_rounds_and_fewer_than_three_successes(self):
        for name, run_ids, failed_run, expected in (
            ("duplicate", (1, 1, 3), None, "run ids"),
            ("failed", (1, 2, 3), 3, "fewer than 3 successful"),
        ):
            with self.subTest(name=name):
                results = self.root / name
                self._run(
                    "--preflight", "--native-build", str(self.native_build),
                    "--libuv-build", str(self.libuv_build), result_root=results,
                )
                self._write_minimal_complete_summaries(results, run_ids, failed_run)
                result = self._run(
                    "--compare-only", str(results), result_root=results, check=False
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stdout)

    def test_compare_rejects_stale_remote_deployment_and_missing_metric(self):
        results = self.root / "stale"
        self._run(
            "--preflight", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), result_root=results,
        )
        self._write_minimal_complete_summaries(results, (1, 2, 3))
        (self.remote_state / "raypx2-libuv.sha").write_text("f" * 64)
        stale = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn("libuv remote deployment hash is stale", stale.stdout)

        expected = json.loads(
            (results / "libuv" / "build-metadata.txt.json").read_text(
                encoding="utf-8"
            )
        )["remote_binary_sha256"]
        (self.remote_state / "raypx2-libuv.sha").write_text(expected)
        path = results / "libuv" / "summary.csv"
        with path.open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        rows[0]["bbr_bw_avg_mbps"] = ""
        with path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
        missing = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("data-validity missing bbr_bw_avg_mbps", missing.stdout)

    def test_compare_requires_allocator_and_stable_process_image_evidence(self):
        results = self.root / "runtime-evidence"
        self._run(
            "--preflight", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), result_root=results,
        )
        self._write_minimal_complete_summaries(results, (1, 2, 3))
        allocator = results / "libuv" / "proxy" / "remote-allocator.after.json"
        allocator.unlink()
        missing_allocator = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertNotEqual(missing_allocator.returncode, 0)
        self.assertIn("invalid runtime evidence: missing", missing_allocator.stdout)

        self._write_runtime_result_evidence(results)
        process = results / "native" / "proxy" / "local-process-image.after.json"
        data = json.loads(process.read_text(encoding="utf-8"))
        data["starttime_ticks"] += 1
        process.write_text(json.dumps(data), encoding="utf-8")
        metadata_path = results / "native" / "build-metadata.txt.json"
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        relative = process.relative_to(results / "native").as_posix()
        metadata["runtime_evidence_sha256"][relative] = hashlib.sha256(
            process.read_bytes()
        ).hexdigest()
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
        changed_process = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertNotEqual(changed_process.returncode, 0)
        self.assertIn("running process image changed during matrix", changed_process.stdout)

    def test_compare_requires_complete_anchors_and_backend_execution_order(self):
        results = self.root / "anchor-evidence"
        self._run(
            "--preflight", "--native-build", str(self.native_build),
            "--libuv-build", str(self.libuv_build), result_root=results,
        )
        self._write_minimal_complete_summaries(results, (1, 2, 3))
        path = results / "native" / "summary.csv"
        with path.open(newline="") as stream:
            rows = list(csv.DictReader(stream))
            fields = rows[0].keys()
        removed = False
        kept = []
        for row in rows:
            if not removed and row.get("phase") == "pre-anchor":
                removed = True
                continue
            kept.append(row)
        with path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(kept)
        missing_anchor = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertNotEqual(missing_anchor.returncode, 0)
        self.assertIn("native anchor set is incomplete", missing_anchor.stdout)

        self._write_minimal_complete_summaries(results, (1, 2, 3))
        (results / "backend-execution-order.csv").unlink()
        missing_order = self._run(
            "--compare-only", str(results), result_root=results, check=False
        )
        self.assertNotEqual(missing_order.returncode, 0)
        self.assertIn("missing backend execution order evidence", missing_order.stdout)

    def test_user_preservation_and_anomaly_semantics_remain_present(self):
        script = RUNNER.read_text(encoding="utf-8")
        self.assertIn('PRESERVE_LOCAL_RAYPX2="${PRESERVE_LOCAL_RAYPX2:-1}"', script)
        self.assertIn('CONTINUE_ON_CASE_FAILURE="${CONTINUE_ON_CASE_FAILURE:-0}"', script)
        self.assertIn('if r["iperf_rc"] != "0" or r["notes"] != "ok":', script)
        self.assertIn("known-anomalies.txt", script)
        self.assertIn("combo_median_delta_gt_20", script)
        self.assertIn("remote binary hash mismatch after sync", script)
        self.assertIn(
            "sed -n 's/.*admin token file //p' "
            "/home/jack/dgx-netem-server.stderr.log | head -1",
            script,
        )
        self.assertNotIn("awk '{print \\\\$NF}'", script)
        self.assertIn("capture_remote_allocator_with_retry", script)
        self.assertIn("remote allocator admin endpoint did not become ready", script)
        self.assertIn("for attempt in $(seq 1 80)", script)
        self.assertIn("pkill -9 -x raypx2-native", script)
        self.assertIn("pkill -9 -x raypx2-libuv", script)
        self.assertIn("remote admin port is not owned exclusively", script)
        self.assertIn("set -eu; PID_FILE=/home/jack/dgx-netem-iperf3.pid", script)
        self.assertIn("*[!0-9]*", script)
        self.assertIn("grep -Fx -- 'iperf3 -s -B $REMOTE_IP -p $IPERF_PORT'", script)


if __name__ == "__main__":
    unittest.main()
