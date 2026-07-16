#!/usr/bin/env python3
"""Fail-closed validators for Linux libuv functional evidence."""

import argparse
import datetime
import hashlib
import json
import pathlib
import re
import sys


def fail(message):
    print(message, file=sys.stderr)
    raise SystemExit(1)


def load(path):
    try:
        return json.loads(pathlib.Path(path).read_text())
    except Exception as exc:
        fail(f"invalid JSON {path}: {exc}")


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_utc(value, label):
    if (not isinstance(value, str) or
            re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?Z",
                         value) is None):
        fail(f"{label} requires strict UTC timestamp")
    try:
        parsed = datetime.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        fail(f"{label} has invalid UTC timestamp: {exc}")
    if parsed.tzinfo != datetime.timezone.utc:
        fail(f"{label} requires UTC timezone")
    return parsed


def clean_submodule_identity(value):
    if not isinstance(value, str) or not value:
        return False
    entries = value.split("|") if "|" in value else value.splitlines()
    return bool(entries) and all(entry.startswith(" ") and "-dirty" not in entry
                                 for entry in entries)


def file_identity(path):
    resolved = pathlib.Path(path).resolve()
    try:
        stat = resolved.stat()
        digest = hashlib.sha256(resolved.read_bytes()).hexdigest()
    except Exception as exc:
        fail(f"cannot identify binary {resolved}: {exc}")
    return {"path": str(resolved), "sha256": digest, "size": stat.st_size,
            "mtime_ns": stat.st_mtime_ns}


def proc_start_ticks(pid):
    try:
        value = pathlib.Path(f"/proc/{pid}/stat").read_text()
        return int(value[value.rfind(")") + 2:].split()[19])
    except Exception as exc:
        fail(f"cannot read Linux process instance /proc/{pid}/stat: {exc}")


def record_pair(args):
    try:
        client_command = json.loads(args.client_command_json)
        server_command = json.loads(args.server_command_json)
    except Exception as exc:
        fail(f"invalid pair command JSON: {exc}")
    if not all(isinstance(command, list) and command for command in
               (client_command, server_command)):
        fail("pair commands must be non-empty argv arrays")
    value = {
        "schema_version": 1, "platform": "linux", "run_id": args.run_id,
        "pair_id": args.pair_id, "mode": args.mode, "started_utc": utc_now(),
        "binary": file_identity(args.binary),
        "processes": {
            "client": {"pid": args.client_pid,
                       "proc_start_ticks": proc_start_ticks(args.client_pid),
                       "command": client_command},
            "server": {"pid": args.server_pid,
                       "proc_start_ticks": proc_start_ticks(args.server_pid),
                       "command": server_command},
        },
    }
    pathlib.Path(args.output).write_text(json.dumps(value, indent=2) + "\n")


def validate_shutdown(args):
    groups = pathlib.Path(args.groups_dir)
    for mode in args.modes:
        path = groups / f"{mode}-shutdown.json"
        if not path.is_file():
            fail(f"missing shutdown evidence: {path.name}")
        value = load(path)
        pair_path = groups / f"{mode}-pair.json"
        if not pair_path.is_file():
            fail(f"missing pair evidence: {pair_path.name}")
        pair = load(pair_path)
        graceful = value.get("graceful")
        forced = value.get("forced_kill")
        if (value.get("run_id") != args.run_id or
                value.get("pair_id") != pair.get("pair_id") or
                value.get("completed") is not True or
                value.get("all_processes_terminated") is not True or
                type(graceful) is not bool or type(forced) is not bool or
                graceful is forced):
            fail(f"invalid shutdown evidence: {path.name}")


def validate_provenance(args):
    provenance = load(args.report).get("provenance", {})
    superproject = provenance.get("superproject", {})
    manifest = provenance.get("build_manifest", {})
    current_submodules = provenance.get("recursive_submodules", {}).get("current")
    valid = (
        isinstance(superproject.get("head"), str) and
        bool(superproject["head"]) and
        superproject.get("dirty") is False and
        manifest.get("source_commit") == superproject.get("head") and
        manifest.get("source_tree_state") == "clean" and
        manifest.get("submodule_commits") == current_submodules and
        clean_submodule_identity(current_submodules) and
        clean_submodule_identity(manifest.get("submodule_commits")) and
        manifest.get("binary_hash_match") is True and
        manifest.get("compiled_source_match") is True and
        manifest.get("compiled_relay_backend") == "libuv"
    )
    if not valid:
        fail("formal provenance mismatch or dirty source identity")


def admin_port(command):
    if not isinstance(command, list) or command.count("--admin-listen") != 1:
        return None
    index = command.index("--admin-listen")
    if index + 1 >= len(command):
        return None
    try:
        port = int(str(command[index + 1]).rsplit(":", 1)[1])
    except (IndexError, ValueError):
        return None
    return port if 0 < port < 65536 else None


def validate_bindings(args):
    root = pathlib.Path(args.output_dir)
    report = load(root / "report.json")
    report_binary = report.get("provenance", {}).get("binary", {})
    manifests = {}
    for path in (root / "groups").glob("*-pair.json"):
        pair = load(path)
        pair_id = pair.get("pair_id")
        if not pair_id or pair_id in manifests:
            fail(f"pair manifest has missing/duplicate pair_id: {path.name}")
        manifests[pair_id] = (path, pair)
    for case_dir in sorted((root / "cases").glob("*")):
        descriptor = load(case_dir / "case.json")
        if descriptor.get("layer") != "e2e":
            continue
        descriptor_identity = (descriptor.get("run_id"), descriptor.get("name"),
                               descriptor.get("case_id"))
        expected_identity = (args.run_id, case_dir.name,
                             f"{args.run_id}-{case_dir.name}")
        if descriptor_identity != expected_identity:
            fail(f"pair binding mismatch: {case_dir.name}/case.json: "
                 f"expected={expected_identity!r} observed={descriptor_identity!r}")
        expected = (args.run_id, descriptor.get("pair_id"))
        if expected[1] is None:
            fail(f"pair binding mismatch: {case_dir.name} has no pair_id")
        if expected[1] not in manifests:
            fail(f"pair manifest missing for {case_dir.name}: {expected[1]}")
        pair_path, pair = manifests[expected[1]]
        expected_mode = "zstd" if "zstd" in descriptor.get("name", "") else "off"
        pair_binary = pair.get("binary", {})
        processes = pair.get("processes", {})
        manifest_ok = (pair.get("run_id") == args.run_id and
                       pair.get("mode") == expected_mode and
                       pair_binary.get("path") == report_binary.get("path") and
                       pair_binary.get("sha256") == report_binary.get("sha256") and
                       set(processes) == {"client", "server"})
        expected_processes = {}
        for role, process in processes.items():
            command = process.get("command", [])
            port = admin_port(command)
            command_binary = None
            if isinstance(command, list) and command:
                command_binary = str(pathlib.Path(command[0]).resolve())
            manifest_ok = (manifest_ok and isinstance(process.get("pid"), int) and
                           process["pid"] > 0 and
                           isinstance(process.get("proc_start_ticks"), int) and
                           process["proc_start_ticks"] > 0 and
                           len(command) >= 2 and command_binary == pair_binary.get("path") and
                           command[1] == role and port is not None and
                           "--compress" in command and expected_mode in command)
            expected_processes[role] = (process.get("pid"),
                                        process.get("proc_start_ticks"), port)
        if not manifest_ok:
            fail(f"pair manifest mismatch: {pair_path.name}")
        for name in ("client-before.json", "server-before.json",
                     "client-after.json", "server-after.json"):
            evidence = load(case_dir / name).get("_evidence", {})
            role = name.split("-", 1)[0]
            pid, ticks, port = expected_processes[role]
            observed = (evidence.get("run_id"), evidence.get("pair_id"),
                        evidence.get("role"), evidence.get("pid"),
                        evidence.get("proc_start_ticks"), evidence.get("admin_port"))
            expected_snapshot = (expected[0], expected[1], role, pid, ticks, port)
            if observed != expected_snapshot:
                fail(f"pair binding mismatch: {case_dir.name}/{name}: "
                     f"expected={expected_snapshot!r} observed={observed!r}")


def validate_fault(args):
    case_dir = pathlib.Path(args.case_dir)
    descriptor = load(case_dir / "case.json")
    fault = load(case_dir / "fault.json")
    case_id = descriptor.get("case_id")
    if descriptor.get("name") != args.name or fault.get("case_id") != case_id:
        fail("fault evidence case_id/name mismatch")
    port = fault.get("target_port")
    if not isinstance(port, int) or not (0 < port < 65536):
        fail("fault evidence missing target_port")
    fault_time = parse_utc(fault.get("utc"), "fault evidence")
    if args.name == "tcp_refused":
        if (fault.get("expected_http_status") != 502 or
                fault.get("observed_http_status") != 502 or
                fault.get("connection_result") != "connection_refused"):
            fail("tcp_refused requires exact HTTP 502 and connection_refused evidence")
        return
    if args.name != "tcp_reset":
        fail(f"unsupported fault case: {args.name}")
    status = fault.get("observed_http_status")
    before_tunnel = (status == 500 and fault.get("expected_http_status") == 500 and
                     fault.get("connection_result") == "500_before_tunnel" and
                     fault.get("read_outcome") == "http_error_before_tunnel")
    after_tunnel = (status == 200 and fault.get("expected_http_status") == 200 and
                    fault.get("connection_result") == "200_then_reset" and
                    fault.get("read_outcome") in
                    ("connection_reset", "broken_pipe_after_reset",
                     "eof_after_audited_reset"))
    if not (before_tunnel or after_tunnel):
        fail("tcp_reset requires exact 500_before_tunnel or 200_then_reset read_outcome evidence")
    if not args.audit:
        fail("missing reset audit")
    actions = []
    try:
        for line in pathlib.Path(args.audit).read_text().splitlines():
            if not line.strip():
                continue
            item = json.loads(line)
            if item.get("case_id") == case_id and item.get("port") == port:
                actions.append((item.get("action"),
                                parse_utc(item.get("utc"), "reset audit")))
    except Exception as exc:
        fail(f"invalid reset audit: {exc}")
    if (len(actions) != 2 or [action for action, _ in actions] !=
            ["accepted", "reset_so_linger_0"]):
        fail("missing reset audit: exactly ordered accepted/reset_so_linger_0 required")
    if not actions[0][1] < actions[1][1] <= fault_time:
        fail("reset audit timestamps must satisfy accepted < reset <= fault")
    deltas = load(case_dir / "deltas.json")
    if after_tunnel and int(deltas.get("relay_activity_delta", 0)) <= 0:
        fail("tcp_reset requires positive relay_activity_delta")


FIN_ZERO_FIELDS = (
    "relay_active_relays", "relay_pending_events", "relay_pending_bytes",
    "relay_active_send_reservations", "terminal_retained_owner_count",
    "terminal_sink_pending", "relay_stop_remaining",
)


def validate_fin(args):
    path = pathlib.Path(args.poll)
    try:
        samples = [json.loads(line) for line in path.read_text().splitlines()
                   if line.strip()]
    except Exception as exc:
        fail(f"invalid FIN poll evidence: {exc}")
    if not samples:
        fail("FIN poll evidence is empty")
    final = samples[-1]
    if not final.get("utc"):
        fail("FIN poll final sample has no UTC timestamp")
    for role in ("client", "server"):
        values = final.get(role, {})
        for field in FIN_ZERO_FIELDS:
            if field not in values or values[field] != 0:
                fail(f"FIN convergence failed: {role}.{field}={values.get(field)!r}")


def validate_zstd(args):
    case_dir = pathlib.Path(args.case_dir)
    if args.pair:
        pair = load(args.pair)
        commands = pair.get("processes", {})
        has_zstd = (pair.get("mode") == "zstd" and
                    all("--compress" in value.get("command", []) and
                        "zstd" in value.get("command", [])
                        for value in commands.values()) and len(commands) == 2)
    else:
        has_zstd = "--compress zstd" in (case_dir / "command.txt").read_text()
    if not has_zstd:
        fail("zstd evidence command is missing --compress zstd")
    deltas = load(case_dir / "deltas.json")
    fields = (
        "zstd_compress_input_delta", "zstd_compress_output_delta",
        "zstd_decompress_input_delta", "zstd_decompress_output_delta",
    )
    if any(int(deltas.get(field, 0)) <= 0 for field in fields):
        fail("nonzero zstd runtime evidence is required")
    if int(deltas.get("zstd_failures_delta", 0)) != 0:
        fail("zstd runtime failures must be zero")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    pair = sub.add_parser("record-pair")
    pair.add_argument("--output", required=True)
    pair.add_argument("--run-id", required=True)
    pair.add_argument("--pair-id", required=True)
    pair.add_argument("--mode", choices=("off", "zstd"), required=True)
    pair.add_argument("--binary", required=True)
    pair.add_argument("--client-pid", type=int, required=True)
    pair.add_argument("--server-pid", type=int, required=True)
    pair.add_argument("--client-command-json", required=True)
    pair.add_argument("--server-command-json", required=True)
    pair.set_defaults(func=record_pair)
    bindings = sub.add_parser("validate-bindings")
    bindings.add_argument("--output-dir", required=True)
    bindings.add_argument("--run-id", required=True)
    bindings.set_defaults(func=validate_bindings)
    provenance = sub.add_parser("validate-provenance")
    provenance.add_argument("--report", required=True)
    provenance.set_defaults(func=validate_provenance)
    fault = sub.add_parser("validate-fault")
    fault.add_argument("--name", required=True)
    fault.add_argument("--case-dir", required=True)
    fault.add_argument("--audit")
    fault.set_defaults(func=validate_fault)
    fin = sub.add_parser("validate-fin")
    fin.add_argument("--poll", required=True)
    fin.set_defaults(func=validate_fin)
    zstd = sub.add_parser("validate-zstd")
    zstd.add_argument("--case-dir", required=True)
    zstd.add_argument("--pair")
    zstd.set_defaults(func=validate_zstd)
    shutdown = sub.add_parser("validate-shutdown")
    shutdown.add_argument("--groups-dir", required=True)
    shutdown.add_argument("--run-id", required=True)
    shutdown.add_argument("--modes", nargs="+", required=True)
    shutdown.set_defaults(func=validate_shutdown)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
