import json
import hashlib
import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "run-linux-libuv-relay-functional.sh"
EVIDENCE_TOOL = ROOT / "scripts" / "libuv-functional-evidence.py"
NATIVE_EVIDENCE_RUNNER = ROOT / "scripts" / "run-native-test-evidence.sh"
NATIVE_TEST_EVIDENCE = ROOT / "scripts" / "native-test-binary-evidence.py"


def good_admin():
    return {
        "compiled_relay_backend": "libuv", "backend": "libuv",
        "relay_backend": "libuv", "linux_relay_backend": "libuv",
        "relay_snapshot_complete": True,
        "libuv_allocator_mode": "mimalloc", "libuv_allocator_attempted": True,
        "libuv_allocator_in_progress": False, "libuv_allocator_installed": True,
        "libuv_allocator_status": 0, "terminal_exactly_once_violation": 0,
        "relay_accounting_duplicate_release": 0,
    }


class LinuxLibuvRelayFunctionalRunnerTest(unittest.TestCase):
    def test_main_installs_libuv_allocator_before_other_runtime_bootstrap(self):
        source = (ROOT / "src" / "main.cpp").read_text()
        main = source[source.index("int main(int argc, char** argv) {"):]
        install = "TqUvInstallAllocator()"
        self.assertIn(install, main)
        self.assertLess(main.index(install), main.index("TqAdminAuth::SetRuntimeBinaryName"))

    def test_capture_snapshot_checks_process_instance_before_and_after_fetch(self):
        source = RUNNER.read_text()
        capture = source[source.index("capture_snapshot() {"):source.index("\nstop_pair() {")]
        probe = 'observed_ticks="$(linux_proc_start_ticks "$pid")"'
        self.assertEqual(capture.count(probe), 2)
        self.assertLess(capture.index(probe), capture.index('admin_get "$port"'))
        self.assertGreater(capture.rindex(probe), capture.index('admin_post "$port"'))

    maxDiff = None

    def run_runner(self, *args, env=None):
        merged = os.environ.copy()
        if env:
            merged.update(env)
        return subprocess.run(
            ["bash", str(RUNNER), *args],
            cwd=ROOT,
            env=merged,
            text=True,
            capture_output=True,
        )

    def test_dry_run_declares_complete_linux_matrix_and_evidence_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_runner("--dry-run", "--output-dir", tmp)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((pathlib.Path(tmp) / "report.json").read_text())
            self.assertEqual(report["platform"], "linux")
            self.assertEqual(report["required_backend"], "libuv")
            self.assertEqual(report["required_allocator"], "mimalloc")
            self.assertEqual(report["execution_mode"], "dry_run")
            self.assertRegex(report["run_id"], r"^[0-9TZ-]+-[0-9a-f]+$")
            self.assertIsNotNone(report["started_utc"])
            self.assertIsNotNone(report["ended_utc"])
            self.assertIn("git", report["provenance"])
            self.assertIn("head", report["provenance"]["git"])
            self.assertIn("dirty", report["provenance"]["git"])
            self.assertEqual(report["provenance"]["superproject"]["head"],
                             report["provenance"]["git"]["head"])
            self.assertIn("status", report["provenance"]["superproject"])
            self.assertIn("third_party/msquic", report["provenance"]["submodules"])
            self.assertIn("head", report["provenance"]["submodules"]["third_party/msquic"])
            self.assertIn("dirty", report["provenance"]["submodules"]["third_party/msquic"])
            self.assertIn("clean_relative_to_head", report["provenance"]["compiled_sources"])
            names = {case["name"] for case in report["cases"]}
            self.assertTrue({
                "http_download_off", "http_upload_off", "socks5_download_off",
                "port_forward_off", "http_download_zstd", "http_upload_zstd",
                "fin_half_close", "tcp_refused", "tcp_reset", "quic_abort_unit",
                "queue_pressure_unit", "allocator_bootstrap_failure_unit",
            }.issubset(names))
            for case in report["cases"]:
                if case["layer"] == "e2e":
                    self.assertEqual(set(case["evidence"]), {
                        "command", "exit_code", "proxy_logs", "admin_snapshot",
                        "allocator_snapshot", "terminal_counters"})
                else:
                    self.assertEqual(set(case["evidence"]), {"command", "exit_code", "case_log"})

    def test_fixture_run_rejects_non_libuv_or_uninstalled_mimalloc(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = pathlib.Path(tmp) / "bad.json"
            fixture.write_text(json.dumps({
                "compiled_relay_backend": "native",
                "libuv_allocator_mode": "mimalloc",
                "libuv_allocator_installed": False,
            }))
            out = pathlib.Path(tmp) / "out"
            result = self.run_runner(
                "--fixture-admin", str(fixture), "--output-dir", str(out),
                env={"FUNCTIONAL_CASE_DRIVER": "/bin/true"},
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("compiled_relay_backend='libuv'", result.stderr)
            report = json.loads((out / "report.json").read_text())
            self.assertEqual(report["ready"], {"client": 0, "server": 0})

    def test_fixture_rejects_incoherent_backend_and_allocator_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = pathlib.Path(tmp) / "bad.json"
            fixture.write_text(json.dumps({
                "compiled_relay_backend": "libuv", "backend": "epoll",
                "relay_backend": "libuv", "linux_relay_backend": "libuv",
                "relay_snapshot_complete": True,
                "libuv_allocator_mode": "mimalloc",
                "libuv_allocator_attempted": True,
                "libuv_allocator_in_progress": False,
                "libuv_allocator_installed": True,
                "libuv_allocator_status": -1,
            }))
            result = self.run_runner(
                "--fixture-admin", str(fixture), "--output-dir", str(pathlib.Path(tmp) / "out"),
                env={"FUNCTIONAL_CASE_DRIVER": "/bin/true"},
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("admin contract", result.stderr)

    def test_true_fixture_driver_can_never_create_formal_valid_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            fixture = base / "good.json"
            fixture.write_text(json.dumps(good_admin()))
            out = base / "out"
            result = self.run_runner(
                "--fixture-admin", str(fixture), "--output-dir", str(out),
                env={"FUNCTIONAL_CASE_DRIVER": "/bin/true"},
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((out / "report.json").read_text())
            self.assertEqual(report["execution_mode"], "fixture_case_driver")
            self.assertTrue(report["fixture"])
            self.assertTrue(report["case_driver"])
            self.assertFalse(report["valid"])
            self.assertEqual(report["formal_gate_status"], "not_applicable")

    def test_generated_admin_token_is_64_hex_characters(self):
        result = self.run_runner("--print-admin-token")
        self.assertEqual(result.returncode, 0, result.stderr)
        token = result.stdout.strip()
        self.assertEqual(len(token), 64)
        self.assertTrue(all(ch in "0123456789abcdef" for ch in token))

    def test_loopback_preflight_cannot_be_bypassed_by_no_proxy(self):
        source = RUNNER.read_text()
        self.assertIn("--noproxy ''", source)

    def test_admin_requests_never_use_environment_proxy(self):
        source = RUNNER.read_text()
        self.assertIn("curl -fsS --noproxy '*'", source)

    def test_fixture_failure_still_cleans_children_and_preserves_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            fixture = base / "good.json"
            fixture.write_text(json.dumps(good_admin()))
            driver = base / "driver.sh"
            driver.write_text("#!/bin/sh\n[ \"$1\" != tcp_reset ]\n")
            driver.chmod(driver.stat().st_mode | stat.S_IXUSR)
            marker = base / "cleaned"
            out = base / "out"
            result = self.run_runner(
                "--fixture-admin", str(fixture), "--output-dir", str(out),
                env={
                    "FUNCTIONAL_CASE_DRIVER": str(driver),
                    "FUNCTIONAL_CLEANUP_MARKER": str(marker),
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(marker.exists(), "EXIT cleanup did not run")
            report = json.loads((out / "report.json").read_text())
            self.assertEqual(report["failed"], 1)
            self.assertEqual(report["passed"], len(report["cases"]) - 1)
            failed = next(case for case in report["cases"] if case["name"] == "tcp_reset")
            self.assertEqual(failed["exit_code"], 1)
            case_dir = out / "cases" / "tcp_reset"
            for name in ("command.txt", "exit-code.txt", "proxy.log",
                         "admin.json", "allocator.json", "terminal.json"):
                self.assertTrue((case_dir / name).exists(), name)
            self.assertEqual(report["duplicate_settlement"], 0)

    def test_case_failure_kills_long_lived_grandchild_process_group(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            fixture = base / "good.json"
            fixture.write_text(json.dumps(good_admin()))
            pidfile = base / "grandchild.pid"
            driver = base / "driver.sh"
            driver.write_text(
                "#!/bin/sh\n"
                "if [ \"$1\" = http_download_off ]; then sleep 300 & echo $! >\"$PIDFILE\"; exit 9; fi\n"
                "exit 0\n"
            )
            driver.chmod(driver.stat().st_mode | stat.S_IXUSR)
            result = self.run_runner(
                "--fixture-admin", str(fixture), "--output-dir", str(base / "out"),
                env={"FUNCTIONAL_CASE_DRIVER": str(driver), "PIDFILE": str(pidfile)},
            )
            self.assertNotEqual(result.returncode, 0)
            pid = int(pidfile.read_text())
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.02)
            else:
                self.fail(f"grandchild {pid} survived process-group cleanup")

    def test_term_signal_kills_active_case_process_group_and_preserves_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            fixture = base / "good.json"
            fixture.write_text(json.dumps(good_admin()))
            pidfile = base / "grandchild.pid"
            driver = base / "driver.sh"
            driver.write_text(
                "#!/bin/sh\n"
                "sleep 300 & echo $! >\"$PIDFILE\"\n"
                "wait\n"
            )
            driver.chmod(driver.stat().st_mode | stat.S_IXUSR)
            out = base / "out"
            env = os.environ.copy()
            env.update({"FUNCTIONAL_CASE_DRIVER": str(driver), "PIDFILE": str(pidfile)})
            proc = subprocess.Popen(
                ["bash", str(RUNNER), "--fixture-admin", str(fixture),
                 "--output-dir", str(out)], cwd=ROOT, env=env,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not pidfile.exists():
                time.sleep(.02)
            self.assertTrue(pidfile.exists(), "case driver did not start")
            proc.terminate()
            stdout, stderr = proc.communicate(timeout=5)
            self.assertEqual(proc.returncode, 143, (stdout, stderr))
            self.assertTrue((out / "report.json").exists())
            pid = int(pidfile.read_text())
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(.02)
            else:
                self.fail(f"grandchild {pid} survived TERM cleanup")


class LibuvFunctionalEvidenceContractTest(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run(
            ["python3", str(EVIDENCE_TOOL), *map(str, args)],
            cwd=ROOT, text=True, capture_output=True,
        )

    def test_pair_binding_accepts_same_run_and_pair_and_rejects_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            (base / "cases" / "ok").mkdir(parents=True)
            (base / "groups").mkdir()
            binary = pathlib.Path("/bin/true").resolve()
            import hashlib
            digest = hashlib.sha256(binary.read_bytes()).hexdigest()
            (base / "report.json").write_text(json.dumps({
                "run_id": "run-1", "provenance": {"binary": {
                    "path": str(binary), "sha256": digest}}}))
            pair_path = base / "groups" / "off-pair.json"
            pair_path.write_text(json.dumps({
                "run_id": "run-1", "pair_id": "pair-off", "mode": "off",
                "binary": {"path": str(binary), "sha256": digest},
                "processes": {
                    "client": {"pid": 101, "proc_start_ticks": 1001,
                               "command": [str(binary), "client", "--compress", "off",
                                           "--admin-listen", "127.0.0.1:31001"]},
                    "server": {"pid": 102, "proc_start_ticks": 1002,
                               "command": [str(binary), "server", "--compress", "off",
                                           "--admin-listen", "127.0.0.1:31002"]}}}))
            (base / "cases" / "ok" / "case.json").write_text(json.dumps({
                "run_id": "run-1", "pair_id": "pair-off", "layer": "e2e",
                "name": "ok", "case_id": "run-1-ok"}))
            for name in ("client-before.json", "server-before.json",
                         "client-after.json", "server-after.json"):
                role = name.split("-", 1)[0]
                process = {"client": (101, 1001, 31001),
                           "server": (102, 1002, 31002)}[role]
                (base / "cases" / "ok" / name).write_text(json.dumps({
                    "_evidence": {"run_id": "run-1", "pair_id": "pair-off",
                                  "role": role, "pid": process[0],
                                  "proc_start_ticks": process[1],
                                  "admin_port": process[2]}}))
            good = self.run_tool("validate-bindings", "--output-dir", base,
                                 "--run-id", "run-1")
            self.assertEqual(good.returncode, 0, good.stderr)
            bad_path = base / "cases" / "ok" / "server-after.json"
            value = json.loads(bad_path.read_text())
            value["_evidence"]["pair_id"] = "pair-other"
            bad_path.write_text(json.dumps(value))
            bad = self.run_tool("validate-bindings", "--output-dir", base,
                                "--run-id", "run-1")
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("pair binding mismatch", bad.stderr)
            value["_evidence"]["pair_id"] = "pair-off"
            bad_path.write_text(json.dumps(value))
            pair = json.loads(pair_path.read_text())
            pair["processes"]["server"]["command"][-1] = "zstd"
            pair_path.write_text(json.dumps(pair))
            bad_pair = self.run_tool("validate-bindings", "--output-dir", base,
                                     "--run-id", "run-1")
            self.assertNotEqual(bad_pair.returncode, 0)
            self.assertIn("pair manifest", bad_pair.stderr)

    def test_pair_binding_rejects_wrong_binary_role_port_pid_and_start_ticks(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp); (base / "cases" / "ok").mkdir(parents=True)
            (base / "groups").mkdir(); binary = pathlib.Path("/bin/true").resolve()
            import hashlib
            digest = hashlib.sha256(binary.read_bytes()).hexdigest()
            report = {"run_id":"run-1","provenance":{"binary":{"path":str(binary),"sha256":digest}}}
            (base/"report.json").write_text(json.dumps(report))
            pair = {"run_id":"run-1","pair_id":"pair-off","mode":"off",
                    "binary":{"path":str(binary),"sha256":digest},"processes":{
                    "client":{"pid":101,"proc_start_ticks":1001,"command":[str(binary),"client","--compress","off","--admin-listen","127.0.0.1:31001"]},
                    "server":{"pid":102,"proc_start_ticks":1002,"command":[str(binary),"server","--compress","off","--admin-listen","127.0.0.1:31002"]}}}
            pair_path=base/"groups"/"off-pair.json"; pair_path.write_text(json.dumps(pair))
            descriptor_path=base/"cases"/"ok"/"case.json"
            descriptor_path.write_text(json.dumps({"run_id":"run-1","pair_id":"pair-off",
                "case_id":"run-1-ok","layer":"e2e","name":"ok"}))
            snapshots={}
            for name,role,pid,ticks,port in (
                ("client-before.json","client",101,1001,31001),("client-after.json","client",101,1001,31001),
                ("server-before.json","server",102,1002,31002),("server-after.json","server",102,1002,31002)):
                snapshots[name]={"_evidence":{"run_id":"run-1","pair_id":"pair-off","role":role,"pid":pid,"proc_start_ticks":ticks,"admin_port":port}}
                (base/"cases"/"ok"/name).write_text(json.dumps(snapshots[name]))
            mutations=(
                ("client-before.json","role","server"),("client-before.json","admin_port",31002),
                ("client-before.json","pid",999),("client-before.json","proc_start_ticks",9999))
            for filename,key,bad_value in mutations:
                path=base/"cases"/"ok"/filename; value=json.loads(path.read_text()); old=value["_evidence"][key]
                value["_evidence"][key]=bad_value; path.write_text(json.dumps(value))
                result=self.run_tool("validate-bindings","--output-dir",base,"--run-id","run-1")
                self.assertNotEqual(result.returncode,0,(filename,key,result.stderr))
                value["_evidence"][key]=old; path.write_text(json.dumps(value))
            for index,bad_value in ((0,"/bin/false"),(1,"server")):
                pair=json.loads(pair_path.read_text()); old=pair["processes"]["client"]["command"][index]
                pair["processes"]["client"]["command"][index]=bad_value; pair_path.write_text(json.dumps(pair))
                result=self.run_tool("validate-bindings","--output-dir",base,"--run-id","run-1")
                self.assertNotEqual(result.returncode,0,result.stderr)
                pair["processes"]["client"]["command"][index]=old; pair_path.write_text(json.dumps(pair))
            for key,bad_value in (("run_id","run-2"),("name","renamed"),
                                  ("case_id","run-1-other")):
                descriptor=json.loads(descriptor_path.read_text())
                old=descriptor[key]; descriptor[key]=bad_value
                descriptor_path.write_text(json.dumps(descriptor))
                result=self.run_tool("validate-bindings","--output-dir",base,"--run-id","run-1")
                self.assertNotEqual(result.returncode,0,(key,result.stderr))
                descriptor[key]=old; descriptor_path.write_text(json.dumps(descriptor))

    def test_formal_provenance_rejects_stale_commit_and_dirty_tree_or_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            report_path=pathlib.Path(tmp)/"report.json"
            base={"provenance":{"superproject":{"head":"abc","dirty":False},
                  "recursive_submodules":{"current":" clean-submodules"},
                  "build_manifest":{"source_commit":"abc","source_tree_state":"clean",
                  "submodule_commits":" clean-submodules","binary_hash_match":True,
                  "compiled_source_match":True,"compiled_relay_backend":"libuv"}}}
            report_path.write_text(json.dumps(base))
            good=self.run_tool("validate-provenance","--report",report_path)
            self.assertEqual(good.returncode,0,good.stderr)
            for path,value in ((["provenance","build_manifest","source_commit"],"old"),
                               (["provenance","superproject","dirty"],True),
                               (["provenance","build_manifest","source_tree_state"],"dirty"),
                               (["provenance","build_manifest","submodule_commits"],"+dirty-submodule")):
                doc=json.loads(json.dumps(base)); target=doc
                for key in path[:-1]: target=target[key]
                target[path[-1]]=value; report_path.write_text(json.dumps(doc))
                bad=self.run_tool("validate-provenance","--report",report_path)
                self.assertNotEqual(bad.returncode,0,(path,bad.stderr))
            dirty_suffix=json.loads(json.dumps(base))
            dirty_identity=" abc123 third_party/libuv (heads/main-dirty)"
            dirty_suffix["provenance"]["recursive_submodules"]["current"]=dirty_identity
            dirty_suffix["provenance"]["build_manifest"]["submodule_commits"]=dirty_identity
            report_path.write_text(json.dumps(dirty_suffix))
            bad=self.run_tool("validate-provenance","--report",report_path)
            self.assertNotEqual(bad.returncode,0,bad.stderr)

    def test_pair_provenance_records_commands_pid_and_linux_start_ticks(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            process = subprocess.Popen(["sleep", "30"])
            try:
                output = base / "pair.json"
                result = self.run_tool(
                    "record-pair", "--output", output, "--run-id", "run-1",
                    "--pair-id", "pair-zstd", "--mode", "zstd",
                    "--binary", "/bin/sleep", "--client-pid", process.pid,
                    "--server-pid", process.pid,
                    "--client-command-json", json.dumps(["/bin/sleep", "30", "--compress", "zstd"]),
                    "--server-command-json", json.dumps(["/bin/sleep", "30", "--compress", "zstd"]),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                pair = json.loads(output.read_text())
                self.assertEqual(pair["run_id"], "run-1")
                self.assertEqual(pair["pair_id"], "pair-zstd")
                self.assertEqual(pair["mode"], "zstd")
                for role in ("client", "server"):
                    self.assertEqual(pair["processes"][role]["pid"], process.pid)
                    self.assertGreater(pair["processes"][role]["proc_start_ticks"], 0)
                    self.assertIn("--compress", pair["processes"][role]["command"])
                self.assertEqual(len(pair["binary"]["sha256"]), 64)
                self.assertEqual(pair["binary"]["path"], str(pathlib.Path("/bin/sleep").resolve()))
            finally:
                process.terminate()
                process.wait()

    def test_shutdown_validation_requires_every_started_group(self):
        with tempfile.TemporaryDirectory() as tmp:
            groups = pathlib.Path(tmp)
            (groups / "off-pair.json").write_text(json.dumps({"pair_id":"pair-off"}))
            (groups / "zstd-pair.json").write_text(json.dumps({"pair_id":"pair-zstd"}))
            (groups / "off-shutdown.json").write_text(json.dumps({
                "run_id": "run-1", "pair_id": "pair-off",
                "completed": True, "all_processes_terminated": True,
                "graceful":False,"forced_kill":True}))
            missing = self.run_tool(
                "validate-shutdown", "--groups-dir", groups, "--run-id", "run-1",
                "--modes", "off", "zstd")
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("zstd-shutdown.json", missing.stderr)
            (groups / "zstd-shutdown.json").write_text(json.dumps({
                "run_id": "run-1", "pair_id": "pair-zstd",
                "completed": True, "all_processes_terminated": True,
                "graceful":True,"forced_kill":False}))
            good = self.run_tool(
                "validate-shutdown", "--groups-dir", groups, "--run-id", "run-1",
                "--modes", "off", "zstd")
            self.assertEqual(good.returncode, 0, good.stderr)
            value=json.loads((groups/"zstd-shutdown.json").read_text())
            for key,bad_value in (("pair_id","pair-off"),("graceful","true"),("forced_kill",True)):
                changed=dict(value); changed[key]=bad_value
                (groups/"zstd-shutdown.json").write_text(json.dumps(changed))
                bad=self.run_tool("validate-shutdown","--groups-dir",groups,"--run-id","run-1","--modes","off","zstd")
                self.assertNotEqual(bad.returncode,0,(key,bad.stderr))
            (groups/"zstd-shutdown.json").write_text(json.dumps(value))

    def test_reset_requires_exact_status_target_audit_and_relay_activity(self):
        with tempfile.TemporaryDirectory() as tmp:
            case = pathlib.Path(tmp)
            (case / "case.json").write_text(json.dumps({
                "name": "tcp_reset", "case_id": "case-reset"}))
            (case / "fault.json").write_text(json.dumps({
                "case_id": "case-reset", "target_port": 32001,
                "utc": "2026-07-15T00:00:02Z",
                "expected_http_status": 500, "observed_http_status": 500,
                "connection_result": "500_before_tunnel",
                "read_outcome": "http_error_before_tunnel"}))
            (case / "deltas.json").write_text(json.dumps({"relay_activity_delta": 0}))
            audit = case / "target-audit.jsonl"
            audit.write_text(
                json.dumps({"utc": "2026-07-15T00:00:00Z", "case_id": "case-reset",
                            "port": 32001, "action": "accepted"}) + "\n" +
                json.dumps({"utc": "2026-07-15T00:00:01Z", "case_id": "case-reset",
                            "port": 32001, "action": "reset_so_linger_0"}) + "\n")
            good = self.run_tool("validate-fault", "--name", "tcp_reset",
                                 "--case-dir", case, "--audit", audit)
            self.assertEqual(good.returncode, 0, good.stderr)
            audit.write_text("")
            bad = self.run_tool("validate-fault", "--name", "tcp_reset",
                                "--case-dir", case, "--audit", audit)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("missing reset audit", bad.stderr)

    def test_reset_accepts_audited_200_then_connection_reset_not_arbitrary_eof(self):
        with tempfile.TemporaryDirectory() as tmp:
            case = pathlib.Path(tmp)
            (case / "case.json").write_text(json.dumps({
                "name": "tcp_reset", "case_id": "case-reset"}))
            fault = {"utc": "2026-07-15T00:00:02Z", "case_id": "case-reset",
                     "target_port": 32001, "expected_http_status": 200,
                     "observed_http_status": 200, "connection_result": "200_then_reset",
                     "read_outcome": "connection_reset"}
            (case / "fault.json").write_text(json.dumps(fault))
            (case / "deltas.json").write_text(json.dumps({"relay_activity_delta": 0}))
            audit = case / "target-audit.jsonl"
            audit.write_text(
                json.dumps({"utc": "2026-07-15T00:00:00Z", "case_id": "case-reset",
                            "port": 32001, "action": "accepted"}) + "\n" +
                json.dumps({"utc": "2026-07-15T00:00:01Z", "case_id": "case-reset",
                            "port": 32001, "action": "reset_so_linger_0"}) + "\n")
            no_activity = self.run_tool("validate-fault", "--name", "tcp_reset",
                                        "--case-dir", case, "--audit", audit)
            self.assertNotEqual(no_activity.returncode, 0)
            self.assertIn("relay_activity_delta", no_activity.stderr)
            (case / "deltas.json").write_text(json.dumps({"relay_activity_delta": 1}))
            good = self.run_tool("validate-fault", "--name", "tcp_reset",
                                 "--case-dir", case, "--audit", audit)
            self.assertEqual(good.returncode, 0, good.stderr)
            fault["read_outcome"] = "empty_read_without_reset_result"
            (case / "fault.json").write_text(json.dumps(fault))
            bad = self.run_tool("validate-fault", "--name", "tcp_reset",
                                "--case-dir", case, "--audit", audit)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("read_outcome", bad.stderr)

    def test_reset_audit_rejects_reversed_future_and_duplicate_actions(self):
        with tempfile.TemporaryDirectory() as tmp:
            case=pathlib.Path(tmp)
            (case/"case.json").write_text(json.dumps({"name":"tcp_reset","case_id":"case-reset"}))
            (case/"fault.json").write_text(json.dumps({"utc":"2026-07-15T00:00:02Z","case_id":"case-reset",
                "target_port":32001,"expected_http_status":500,"observed_http_status":500,
                "connection_result":"500_before_tunnel","read_outcome":"http_error_before_tunnel"}))
            (case/"deltas.json").write_text(json.dumps({"relay_activity_delta":0}))
            audit=case/"audit.jsonl"
            variants=(
                [("reset_so_linger_0","2026-07-15T00:00:01Z"),("accepted","2026-07-15T00:00:00Z")],
                [("accepted","2026-07-15T00:00:00Z"),("reset_so_linger_0","2026-07-15T00:00:03Z")],
                [("accepted","20260715T000000Z"),("reset_so_linger_0","2026-07-15T00:00:01Z")],
                [("accepted","2026-07-15T00:00:00Z"),("reset_so_linger_0","2026-07-15T00:00:01Z"),
                 ("accepted","2026-07-14T00:00:00Z"),("reset_so_linger_0","2026-07-14T00:00:01Z")])
            for entries in variants:
                audit.write_text("".join(json.dumps({"utc":utc,"case_id":"case-reset","port":32001,"action":action})+"\n" for action,utc in entries))
                result=self.run_tool("validate-fault","--name","tcp_reset","--case-dir",case,"--audit",audit)
                self.assertNotEqual(result.returncode,0,(entries,result.stderr))

    def test_refused_rejects_arbitrary_http_response(self):
        with tempfile.TemporaryDirectory() as tmp:
            case = pathlib.Path(tmp)
            (case / "case.json").write_text(json.dumps({
                "name": "tcp_refused", "case_id": "case-refused"}))
            (case / "fault.json").write_text(json.dumps({
                "utc": "2026-07-15T00:00:00Z",
                "case_id": "case-refused", "target_port": 32002,
                "expected_http_status": 502, "observed_http_status": 500,
                "connection_result": "arbitrary_error"}))
            result = self.run_tool("validate-fault", "--name", "tcp_refused",
                                   "--case-dir", case)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exact HTTP 502", result.stderr)

    def test_refused_requires_timestamped_selected_port_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            case = pathlib.Path(tmp)
            (case / "case.json").write_text(json.dumps({
                "name": "tcp_refused", "case_id": "case-refused"}))
            (case / "fault.json").write_text(json.dumps({
                "case_id": "case-refused", "target_port": 32002,
                "expected_http_status": 502, "observed_http_status": 502,
                "connection_result": "connection_refused"}))
            result = self.run_tool("validate-fault", "--name", "tcp_refused",
                                   "--case-dir", case)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("UTC timestamp", result.stderr)

    def test_fin_requires_all_terminal_ownership_counters_to_converge(self):
        with tempfile.TemporaryDirectory() as tmp:
            poll = pathlib.Path(tmp) / "fin-poll.jsonl"
            fields = {
                "relay_active_relays": 0, "relay_pending_events": 0,
                "relay_pending_bytes": 0, "relay_active_send_reservations": 0,
                "terminal_retained_owner_count": 0, "terminal_shutdown_pending": 1,
                "terminal_sink_pending": 0, "relay_stop_remaining": 0,
            }
            poll.write_text(json.dumps({"utc": "2026-07-15T00:00:00Z",
                                        "client": fields, "server": fields}) + "\n")
            good = self.run_tool("validate-fin", "--poll", poll)
            self.assertEqual(good.returncode, 0, good.stderr)
            fields = dict(fields); fields["relay_pending_bytes"] = 1
            poll.write_text(json.dumps({"utc": "2026-07-15T00:00:01Z",
                                        "client": fields, "server": fields}) + "\n")
            bad = self.run_tool("validate-fin", "--poll", poll)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("relay_pending_bytes", bad.stderr)

    def test_zstd_requires_command_and_nonzero_runtime_counter_delta(self):
        with tempfile.TemporaryDirectory() as tmp:
            case = pathlib.Path(tmp)
            (case / "command.txt").write_text("raypx2 client --compress zstd\n")
            (case / "deltas.json").write_text(json.dumps({
                "zstd_compress_input_delta": 4096,
                "zstd_compress_output_delta": 512,
                "zstd_decompress_input_delta": 512,
                "zstd_decompress_output_delta": 4096,
                "zstd_failures_delta": 0,
            }))
            good = self.run_tool("validate-zstd", "--case-dir", case)
            self.assertEqual(good.returncode, 0, good.stderr)
            (case / "deltas.json").write_text(json.dumps({
                "zstd_compress_input_delta": 0, "zstd_compress_output_delta": 0,
                "zstd_decompress_input_delta": 0, "zstd_decompress_output_delta": 0,
                "zstd_failures_delta": 0,
            }))
            bad = self.run_tool("validate-zstd", "--case-dir", case)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("nonzero zstd runtime evidence", bad.stderr)


class NativeEvidenceRunnerTest(unittest.TestCase):
    @staticmethod
    def make_binaries(build, count):
        binaries = build / "bin" / "Release"
        binaries.mkdir(parents=True)
        for index in range(count):
            path = binaries / f"tcpquic_case_{index:02d}_test"
            path.write_text("#!/bin/sh\nexit 0\n")
            path.chmod(path.stat().st_mode | stat.S_IXUSR)

    @staticmethod
    def fake_cmake_env(base):
        tools = base / "tools"
        tools.mkdir(parents=True)
        cmake = tools / "cmake"
        cmake.write_text("#!/bin/sh\nexit 0\n")
        cmake.chmod(cmake.stat().st_mode | stat.S_IXUSR)
        env = os.environ.copy()
        env["PATH"] = f"{tools}:{env['PATH']}"
        return env

    def make_formal_root(self, base, count=43, seal=True):
        root = base / "root"
        scripts = root / "scripts"
        scripts.mkdir(parents=True)
        shutil.copy2(NATIVE_EVIDENCE_RUNNER,
                     scripts / "run-native-test-evidence.sh")
        shutil.copy2(ROOT / "scripts" / "native-test-suite.txt",
                     scripts / "native-test-suite.txt")
        shutil.copy2(NATIVE_TEST_EVIDENCE,
                     scripts / "native-test-binary-evidence.py")
        (root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(native_evidence NONE)\n"
            "set(TCPQUIC_RELAY_BACKEND native CACHE STRING \"\")\n")
        (root / ".gitignore").write_text("/build/\n")
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        subprocess.run(["git", "config", "user.email", "test@example.com"],
                       cwd=root, check=True)
        subprocess.run(["git", "config", "user.name", "Native Evidence Test"],
                       cwd=root, check=True)
        subprocess.run(["git", "add", "."], cwd=root, check=True)
        subprocess.run(["git", "commit", "-qm", "fixture"], cwd=root, check=True)
        build = root / "build"
        subprocess.run(["cmake", "-S", str(root), "-B", str(build),
                        "-DTCPQUIC_RELAY_BACKEND=native"],
                       check=True, capture_output=True, text=True)
        canonical = (scripts / "native-test-suite.txt").read_text().splitlines()
        self.make_binaries(build, count)
        binaries = build / "bin" / "Release"
        for path, name in zip(sorted(binaries.glob("tcpquic_*_test")), canonical):
            path.rename(binaries / name)
        raypx2 = binaries / "raypx2"
        raypx2.write_text("#!/bin/sh\nexit 0\n")
        raypx2.chmod(raypx2.stat().st_mode | stat.S_IXUSR)
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                              check=True, text=True, capture_output=True).stdout.strip()
        submodules = subprocess.run(
            ["git", "submodule", "status", "--recursive"], cwd=root,
            check=True, text=True, capture_output=True).stdout.rstrip("\n").replace("\n", "|")
        if seal:
            manifest = {
                "schema_version": 2,
                "compiled_relay_backend": "native",
                "source_commit": head,
                "source_tree_state": "clean",
                "submodule_commits": submodules,
                "binary_sha256": hashlib.sha256(raypx2.read_bytes()).hexdigest(),
                "cmake_cache_path": str((build / "CMakeCache.txt").resolve()),
                "cmake_cache_sha256": hashlib.sha256(
                    (build / "CMakeCache.txt").read_bytes()).hexdigest(),
            }
            (binaries / "raypx2.build-manifest.json").write_text(
                json.dumps(manifest))
        self.seal_existing_test_suite(root, build)
        return root, scripts / "run-native-test-evidence.sh", build

    def seal_existing_test_suite(self, root, build):
        canonical = (ROOT / "scripts" / "native-test-suite.txt").read_text().splitlines()
        for name in canonical:
            binary = build / "bin" / "Release" / name
            if not binary.is_file():
                continue
            result = subprocess.run(
                ["python3", str(NATIVE_TEST_EVIDENCE), "seal-test",
                 "--root", str(root), "--cache", str(build / "CMakeCache.txt"),
                 "--binary", str(binary), "--target", name],
                text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def seal_test_suite(self, root, build):
        self.seal_existing_test_suite(root, build)

    def verify_test_suite(self, root, build):
        output = build / "test-suite-validation.json"
        result = subprocess.run(
            ["python3", str(NATIVE_TEST_EVIDENCE), "verify-suite",
             "--root", str(root), "--cache", str(build / "CMakeCache.txt"),
             "--binary-dir", str(build / "bin" / "Release"),
             "--suite", str(ROOT / "scripts" / "native-test-suite.txt"),
             "--output", str(output)], text=True, capture_output=True,
        )
        body = json.loads(output.read_text()) if output.exists() else None
        return result, body

    def test_native_test_sidecars_bind_canonical_suite_to_linked_binaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            root, _, build = self.make_formal_root(base, 43)
            binaries = build / "bin" / "Release"
            alias = binaries / "tcpquic_tcp_tunnel_test"
            alias.unlink()
            alias.symlink_to("tcpquic_tunnel_test")
            self.seal_test_suite(root, build)
            result, body = self.verify_test_suite(root, build)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(body["valid"])
            self.assertEqual(len(body["tests"]), 43)
            self.assertTrue(all(len(item["sidecar_sha256"]) == 64
                                for item in body["tests"]))

            victim = build / "bin" / "Release" / "tcpquic_acl_test"
            shutil.copy2("/bin/true", victim)
            replaced, replaced_body = self.verify_test_suite(root, build)
            self.assertNotEqual(replaced.returncode, 0)
            self.assertFalse(replaced_body["valid"])
            self.assertIn("binary_sha256", " ".join(replaced_body["errors"]))

    def test_native_test_sidecars_reject_missing_or_tampered_identity(self):
        mutations = {
            "binary_path": "/tmp/not-the-canonical-test-binary",
            "source_commit": "0" * 40,
            "compiled_relay_backend": "libuv",
            "cmake_cache_sha256": "0" * 64,
            "submodule_commits": "bogus",
        }
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            root, _, build = self.make_formal_root(base, 43)
            self.seal_test_suite(root, build)
            victim = build / "bin" / "Release" / "tcpquic_acl_test.build-manifest.json"
            original = victim.read_text()
            victim.unlink()
            missing, missing_body = self.verify_test_suite(root, build)
            self.assertNotEqual(missing.returncode, 0)
            self.assertFalse(missing_body["valid"])
            victim.write_text(original)
            for field, value in mutations.items():
                with self.subTest(field=field):
                    body = json.loads(original)
                    body[field] = value
                    victim.write_text(json.dumps(body))
                    result, evidence = self.verify_test_suite(root, build)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(evidence["valid"])
                    victim.write_text(original)

    def test_native_evidence_fixture_indexes_binaries_but_is_never_formal_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            build = base / "build"
            self.make_binaries(build, 2)
            out = base / "evidence"
            result = subprocess.run(
                ["bash", str(NATIVE_EVIDENCE_RUNNER), "--build-dir", str(build),
                 "--output-dir", str(out), "--build-driver", "/bin/true",
                 "--expected-test-count", "2", "--fixture"],
                cwd=ROOT, text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            index = json.loads((out / "index.json").read_text())
            self.assertEqual(index["schema_version"], 1)
            self.assertEqual(index["execution_mode"], "fixture")
            self.assertEqual(index["formal_gate_status"], "not_applicable")
            self.assertFalse(index["valid"])
            self.assertEqual(index["build"]["exit_code"], 0)
            self.assertEqual(index["expected_test_count"], 2)
            self.assertEqual(len(index["tests"]), 2)
            for item in index["tests"]:
                self.assertEqual(item["exit_code"], 0)
                self.assertEqual(len(item["sha256"]), 64)
                self.assertEqual(item["mtime_epoch_ns"],
                                 pathlib.Path(item["path"]).stat().st_mtime_ns)
                self.assertTrue(item["started_utc"])
                self.assertTrue(item["ended_utc"])
            self.assertTrue(index["git"]["head"])
            self.assertIn("dirty", index["git"])

    def test_native_evidence_rejects_overrides_without_fixture(self):
        for arguments in (("--build-driver", "/bin/true"),
                          ("--expected-test-count", "2")):
            result = subprocess.run(
                ["bash", str(NATIVE_EVIDENCE_RUNNER), *arguments],
                cwd=ROOT, text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 64)
            self.assertIn("--fixture", result.stderr)

    def test_native_evidence_formal_mode_defaults_to_exactly_43(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            for count in (0, 42, 43, 44):
                root, runner, build = self.make_formal_root(
                    base / f"formal-{count}", count)
                out = base / f"formal-{count}" / "evidence"
                result = subprocess.run(
                    ["bash", str(runner), "--build-dir", str(build),
                     "--output-dir", str(out)],
                    cwd=root, text=True, capture_output=True,
                )
                evidence = json.loads((out / "index.json").read_text())
                self.assertEqual(evidence["execution_mode"], "production")
                self.assertEqual(evidence["expected_test_count"], 43)
                provenance = evidence["build_provenance"]
                self.assertEqual(evidence["build"]["command"],
                                 [provenance["cmake_executable"], "--build",
                                  str(build), "--clean-first", "-j2"])
                self.assertEqual(provenance["compiled_relay_backend"], "native")
                self.assertEqual(provenance["canonical_test_count"], 43)
                self.assertEqual(provenance["head"], evidence["git"]["head"])
                self.assertTrue(provenance["valid"] if count == 43 else
                                not provenance["valid"])
                self.assertEqual(evidence["test_count"], count)
                self.assertEqual(evidence["valid"], count == 43)
                self.assertEqual(evidence["formal_gate_status"],
                                 "passed" if count == 43 else "failed")
                self.assertEqual(result.returncode, 0 if count == 43 else 1)

    def test_native_evidence_rejects_dirty_superproject(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            root, runner, build = self.make_formal_root(base, 43)
            with (root / "CMakeLists.txt").open("a") as output:
                output.write("# dirty\n")
            result = subprocess.run(
                ["bash", str(runner), "--build-dir", str(build),
                 "--output-dir", str(base / "out")],
                cwd=root, text=True, capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("superproject is dirty", result.stderr)

    def test_native_evidence_rejects_recursive_dirty_submodule_suffix(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            root, runner, build = self.make_formal_root(base, 43)
            tools = base / "tools"
            tools.mkdir()
            real_git = shutil.which("git")
            fake_git = tools / "git"
            fake_git.write_text(
                "#!/usr/bin/env python3\n"
                "import os,sys\n"
                "if 'submodule' in sys.argv and 'status' in sys.argv:\n"
                " print(' 0123456789012345678901234567890123456789 third_party/child (heads/main-dirty)')\n"
                " raise SystemExit(0)\n"
                f"os.execv({real_git!r}, [{real_git!r}, *sys.argv[1:]])\n")
            fake_git.chmod(fake_git.stat().st_mode | stat.S_IXUSR)
            env = os.environ.copy()
            env["PATH"] = f"{tools}:{env['PATH']}"
            result = subprocess.run(
                ["bash", str(runner), "--build-dir", str(build),
                 "--output-dir", str(base / "out")], cwd=root,
                env=env, text=True, capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("recursive submodules are not initialized and clean",
                          result.stderr)

    def test_native_evidence_rejects_unsealed_build_identity(self):
        mutations = {
            "backend": ("compiled_relay_backend", "libuv"),
            "commit": ("source_commit", "0" * 40),
            "tree": ("source_tree_state", "dirty"),
            "submodules": ("submodule_commits", "bogus"),
            "binary": ("binary_sha256", "0" * 64),
        }
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            for label, (field, value) in mutations.items():
                with self.subTest(label=label):
                    root, runner, build = self.make_formal_root(
                        base / label, 43)
                    manifest = build / "bin" / "Release" / "raypx2.build-manifest.json"
                    body = json.loads(manifest.read_text())
                    body[field] = value
                    manifest.write_text(json.dumps(body))
                    result = subprocess.run(
                        ["bash", str(runner), "--build-dir", str(build),
                         "--output-dir", str(base / label / "out")],
                        cwd=root, text=True, capture_output=True,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    evidence = json.loads(
                        (base / label / "out" / "index.json").read_text())
                    self.assertFalse(evidence["valid"])
                    self.assertFalse(evidence["build_provenance"]["valid"])

    def test_native_evidence_rejects_fake_cmake_and_dummy_43_without_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            build = base / "build"
            self.make_binaries(build, 43)
            result = subprocess.run(
                ["bash", str(NATIVE_EVIDENCE_RUNNER), "--build-dir", str(build),
                 "--output-dir", str(base / "out")],
                cwd=ROOT, env=self.fake_cmake_env(base / "env"),
                text=True, capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)

    def test_native_evidence_uses_cached_absolute_cmake_not_path_fake(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            root, runner, build = self.make_formal_root(base, 43)
            env = self.fake_cmake_env(base / "env")
            result = subprocess.run(
                ["bash", str(runner), "--build-dir", str(build),
                 "--output-dir", str(base / "out")], cwd=root, env=env,
                text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            evidence = json.loads((base / "out" / "index.json").read_text())
            cached = evidence["build_provenance"]["cmake_executable"]
            self.assertEqual(evidence["build"]["command"][0], cached)
            self.assertNotEqual(pathlib.Path(cached), base / "env" / "tools" / "cmake")

    def test_native_evidence_rejects_canonical_suite_without_cache_or_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            for missing in ("cache", "manifest"):
                root, runner, build = self.make_formal_root(
                    base / missing, 43, seal=missing != "manifest")
                if missing == "cache":
                    (build / "CMakeCache.txt").unlink()
                result = subprocess.run(
                    ["bash", str(runner), "--build-dir", str(build),
                     "--output-dir", str(base / missing / "out")],
                    cwd=root, text=True, capture_output=True,
                )
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
