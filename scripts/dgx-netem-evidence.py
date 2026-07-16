#!/usr/bin/env python3
"""Build and runtime evidence helpers for the Linux DGX backend comparison."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time


MANIFEST_SCHEMA = 2
PROVENANCE_REQUIRED = {
    "schema_version",
    "source_commit",
    "source_tree_state",
    "source_diff_sha256",
    "submodule_commits",
    "compiled_relay_backend",
    "compiler_path",
    "compiler_id",
    "compiler_version",
    "compile_flags",
    "link_flags",
    "link_libraries",
    "source_files",
    "backend_source_files",
    "common_source_files",
    "cache_evidence",
    "optimization",
    "build_shared_libs",
    "mimalloc",
    "sanitizer",
    "tuning",
}
MANIFEST_REQUIRED = PROVENANCE_REQUIRED | {
    "binary_path",
    "binary_sha256",
    "cmake_cache_path",
    "cmake_cache_sha256",
    "provenance_sha256",
    "common_evidence_sha256",
}
# Source files and resolved target libraries are the intentional backend/source-set
# dimension.  They remain sealed and reported, but are excluded from the common
# build-equivalence digest.
COMMON_FIELDS = sorted(PROVENANCE_REQUIRED - {
    "schema_version", "compiled_relay_backend", "source_files",
    "backend_source_files",
})


def fail(message):
    raise SystemExit(message)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_key_values(path):
    values = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            fail(f"invalid provenance line: {line}")
        key, value = line.split("=", 1)
        if key in values:
            fail(f"duplicate provenance key: {key}")
        values[key] = value
    missing = sorted(PROVENANCE_REQUIRED - values.keys())
    if missing:
        fail("missing build provenance fields: " + ",".join(missing))
    if values["schema_version"] != str(MANIFEST_SCHEMA):
        fail("unsupported build provenance schema")
    return values


def canonical_hash(values):
    payload = json.dumps(values, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(payload).hexdigest()


def run_git(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args], check=True, text=True,
        stdout=subprocess.PIPE,
    ).stdout.rstrip("\n")


def cache_evidence(path):
    evidence = {}
    build_directory = str(Path(path).resolve().parent)
    ignored = {"TCPQUIC_RELAY_BACKEND"}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line or ":" not in line:
            continue
        key_type, value = line.split("=", 1)
        key, value_type = key_type.split(":", 1)
        if key in ignored:
            continue
        if value_type in ("BOOL", "STRING") or key in (
            "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_LINKER",
        ):
            evidence[key] = value.replace(build_directory, "<BUILD_DIR>")
    return evidence


def hashed_sources(source_base, target_sources, backend_sources):
    base = Path(source_base).resolve()
    backend_names = {Path(item).as_posix() for item in backend_sources.split("|") if item}
    all_items, backend_items, common_items = [], [], []
    for item in sorted(set(filter(None, target_sources.split("|")))):
        if item.startswith("$<"):
            continue
        path = Path(item)
        full = path if path.is_absolute() else base / path
        if not full.is_file():
            fail(f"target source is missing at link time: {item}")
        normalized = path.as_posix() if not path.is_absolute() else full.as_posix()
        entry = f"{normalized}@{sha256(full)}"
        all_items.append(entry)
        if normalized in backend_names:
            backend_items.append(entry)
        else:
            common_items.append(entry)
    missing_backend = sorted(backend_names - {entry.rsplit("@", 1)[0] for entry in backend_items})
    if missing_backend:
        fail("declared backend sources are not target sources: " + ",".join(missing_backend))
    return "|".join(all_items), "|".join(backend_items), "|".join(common_items)


def seal_live_build(args):
    root = Path(args.source_root).resolve()
    binary = Path(args.binary).resolve()
    cache = Path(args.cmake_cache).resolve()
    status = run_git(root, "status", "--porcelain=v1", "--untracked-files=all")
    diff = run_git(root, "diff", "--binary", "HEAD")
    sources, backend_sources, common_sources = hashed_sources(
        args.source_base, args.target_sources, args.backend_sources.replace(";", "|"))
    cache_values = cache_evidence(cache)
    sanitizer = "address" if "-fsanitize=address" in " ".join(cache_values.values()) else "OFF"
    compiler = cache_values.get("CMAKE_CXX_COMPILER", "")
    compiler_banner = ""
    if compiler:
        try:
            compiler_banner = subprocess.run(
                [compiler, "--version"], check=True, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            ).stdout.splitlines()[0]
        except (OSError, subprocess.CalledProcessError, IndexError) as exc:
            fail(f"cannot capture compiler identity at link time: {exc}")
    provenance = {
        "schema_version": str(MANIFEST_SCHEMA),
        "source_commit": run_git(root, "rev-parse", "HEAD"),
        "source_tree_state": "dirty" if status else "clean",
        "source_diff_sha256": hashlib.sha256((status + "\n" + diff).encode()).hexdigest(),
        "submodule_commits": run_git(root, "submodule", "status", "--recursive").replace("\n", "|"),
        "compiled_relay_backend": args.backend,
        "compiler_path": compiler,
        "compiler_id": compiler_banner,
        "compiler_version": compiler_banner,
        "compile_flags": " ".join(value for key, value in cache_values.items() if key.startswith("CMAKE_CXX_FLAGS")),
        "link_flags": " ".join(value for key, value in cache_values.items() if key.startswith("CMAKE_EXE_LINKER_FLAGS")),
        "link_libraries": args.link_libraries,
        "source_files": sources,
        "backend_source_files": backend_sources,
        "common_source_files": common_sources,
        "cache_evidence": json.dumps(cache_values, sort_keys=True, separators=(",", ":")),
        "optimization": cache_values.get("CMAKE_BUILD_TYPE", ""),
        "build_shared_libs": cache_values.get("BUILD_SHARED_LIBS", ""),
        "mimalloc": cache_values.get("TCPQUIC_USE_MIMALLOC", ""),
        "sanitizer": sanitizer,
        "tuning": "wan",
    }
    provenance_path = Path(args.provenance)
    provenance_path.write_text(
        "".join(f"{key}={value}\n" for key, value in provenance.items()), encoding="utf-8")
    seal_build(argparse.Namespace(
        backend=args.backend, binary=str(binary), cmake_cache=str(cache),
        provenance=str(provenance_path), output=args.output,
    ))


def seal_build(args):
    binary = Path(args.binary).resolve()
    cache = Path(args.cmake_cache).resolve()
    provenance_path = Path(args.provenance).resolve()
    for path in (binary, cache, provenance_path):
        if not path.is_file():
            fail(f"missing build evidence input: {path}")
    provenance = parse_key_values(provenance_path)
    if provenance["compiled_relay_backend"] != args.backend:
        fail("provenance backend mismatch")
    common = {key: provenance[key] for key in COMMON_FIELDS}
    manifest = dict(provenance)
    manifest.update(
        {
            "schema_version": MANIFEST_SCHEMA,
            "binary_path": str(binary),
            "binary_sha256": sha256(binary),
            "cmake_cache_path": str(cache),
            "cmake_cache_sha256": sha256(cache),
            "provenance_sha256": sha256(provenance_path),
            "common_evidence_sha256": canonical_hash(common),
        }
    )
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_manifest(path, expected_backend=None, binary=None, cache=None):
    path = Path(path)
    if not path.is_file():
        fail(f"missing sealed build manifest: {path}")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"invalid sealed build manifest: {exc}")
    missing = sorted(MANIFEST_REQUIRED - manifest.keys())
    if missing:
        fail("missing sealed build manifest fields: " + ",".join(missing))
    if manifest["schema_version"] != MANIFEST_SCHEMA:
        fail("unsupported sealed build manifest schema")
    if expected_backend and manifest["compiled_relay_backend"] != expected_backend:
        fail("sealed build manifest backend mismatch")
    if binary is not None and sha256(binary) != manifest["binary_sha256"]:
        fail("binary hash does not match sealed build manifest")
    if cache is not None and sha256(cache) != manifest["cmake_cache_sha256"]:
        fail("CMake cache hash does not match sealed build manifest")
    common = {key: manifest[key] for key in COMMON_FIELDS}
    if canonical_hash(common) != manifest["common_evidence_sha256"]:
        fail("sealed build manifest common evidence hash mismatch")
    return manifest


def verify_build(args):
    manifest = load_manifest(
        args.manifest,
        expected_backend=args.backend,
        binary=args.binary,
        cache=args.cmake_cache,
    )
    print(json.dumps(manifest, sort_keys=True))


def compare_builds(args):
    native = load_manifest(args.native, expected_backend="native")
    libuv = load_manifest(args.libuv, expected_backend="libuv")
    if native["source_tree_state"] != "clean" or libuv["source_tree_state"] != "clean":
        fail("performance comparison requires clean source trees")
    differences = [key for key in COMMON_FIELDS if native[key] != libuv[key]]
    if differences:
        fail("common sealed build evidence differs: " + ",".join(differences))


def validate_result(args):
    root = Path(args.root)
    manifests = {}
    deployments = []
    for backend in ("native", "libuv"):
        backend_root = root / backend
        metadata_path = backend_root / "build-metadata.txt.json"
        sealed_path = backend_root / "build-metadata.txt.build-manifest.json"
        if not metadata_path.is_file():
            fail(f"missing result metadata: {metadata_path}")
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        runtime_hashes = metadata.get("runtime_evidence_sha256")
        if not isinstance(runtime_hashes, dict):
            fail(f"missing runtime evidence hashes: {backend}")
        summary_path = backend_root / "summary.csv"
        try:
            summary_rows = list(csv.DictReader(summary_path.open(newline="")))
        except OSError as exc:
            fail(f"missing summary for anchor validation: {exc}")
        anchors = [row for row in summary_rows if row.get("phase") in ("pre-anchor", "post-anchor")]
        expected_anchors = {
            (phase, direction, "10", "5", "1")
            for phase in ("pre-anchor", "post-anchor")
            for direction in ("download", "upload")
        }
        actual_anchors = {
            (row.get("phase"), row.get("direction"), row.get("delay_ms"),
             row.get("loss_pct"), row.get("run")) for row in anchors
        }
        if actual_anchors != expected_anchors or len(anchors) != 4:
            fail(f"{backend} anchor set is incomplete")
        if any(row.get("iperf_rc") != "0" or row.get("notes") != "ok" or
               row.get("evidence_status") != "complete" for row in anchors):
            fail(f"{backend} anchor evidence is not successful/complete")
        for field in (
            "remote_host", "remote_binary_path", "remote_binary_sha256",
            "deployment_verified", "binary_path", "binary_sha256", "cmake_cache_path",
        ):
            if field not in metadata:
                fail(f"missing result metadata field: {backend}.{field}")
        if metadata["compiled_relay_backend"] != backend:
            fail("metadata backend mismatch")
        if metadata["deployment_verified"] is not True:
            fail("deployment evidence is not verified")
        manifest = load_manifest(
            sealed_path,
            expected_backend=backend,
            binary=metadata["binary_path"],
            cache=metadata["cmake_cache_path"],
        )
        if manifest["source_tree_state"] != "clean":
            fail("performance comparison requires clean source trees")
        if metadata["binary_sha256"] != manifest["binary_sha256"]:
            fail("result metadata local binary hash drift")
        if metadata["remote_binary_sha256"] != manifest["binary_sha256"]:
            fail("result metadata remote binary hash drift")
        for side in ("local", "remote"):
            before = backend_root / "proxy" / f"{side}-allocator.before.json"
            after = backend_root / "proxy" / f"{side}-allocator.after.json"
            for evidence_path in (
                before, after,
                backend_root / "proxy" / f"{side}-process-image.before.json",
                backend_root / "proxy" / f"{side}-process-image.after.json",
            ):
                relative = evidence_path.relative_to(backend_root).as_posix()
                if not evidence_path.is_file():
                    fail(f"invalid runtime evidence: missing {backend}.{relative}")
                if runtime_hashes.get(relative) != sha256(evidence_path):
                    fail(f"runtime evidence hash mismatch: {backend}.{relative}")
            validate_allocator(argparse.Namespace(
                backend=backend, manifest=str(sealed_path), snapshot=str(before)))
            validate_allocator(argparse.Namespace(
                backend=backend, manifest=str(sealed_path), snapshot=str(after)))
            validate_process(argparse.Namespace(
                before=str(backend_root / "proxy" / f"{side}-process-image.before.json"),
                after=str(backend_root / "proxy" / f"{side}-process-image.after.json"),
                expected_sha256=manifest["binary_sha256"],
            ))
        manifests[backend] = manifest
        deployments.append((
            backend,
            metadata["remote_host"],
            metadata["remote_binary_path"],
            metadata["remote_binary_sha256"],
        ))
    differences = [
        key for key in COMMON_FIELDS
        if manifests["native"][key] != manifests["libuv"][key]
    ]
    if differences:
        fail("common sealed build evidence differs: " + ",".join(differences))
    order_path = root / "backend-execution-order.csv"
    try:
        order = list(csv.DictReader(order_path.open(newline="")))
    except OSError as exc:
        fail(f"missing backend execution order evidence: {exc}")
    pairs = [(row.get("backend"), row.get("event")) for row in order]
    if pairs != [("native", "start"), ("native", "end"),
                 ("libuv", "start"), ("libuv", "end")]:
        fail("backend execution order is invalid")
    if any(not row.get("timestamp") for row in order):
        fail("backend execution order timestamp is missing")
    for deployment in deployments:
        print("\t".join(deployment))


def validate_allocator(args):
    manifest = load_manifest(args.manifest, expected_backend=args.backend)
    try:
        snapshot = json.loads(Path(args.snapshot).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"invalid allocator snapshot: {exc}")
    if snapshot.get("compiled_relay_backend") != args.backend:
        fail("allocator snapshot backend mismatch")
    if snapshot.get("status") != "dumped":
        fail("allocator snapshot schema/status is invalid")
    if args.backend == "native":
        return
    mimalloc_expected = manifest["mimalloc"] in ("ON", "1", "TRUE") and \
        manifest["sanitizer"] != "address"
    if mimalloc_expected:
        if not (
            snapshot.get("libuv_allocator_mode") == "mimalloc"
            and snapshot.get("libuv_allocator_attempted") is True
            and snapshot.get("libuv_allocator_in_progress") is False
            and snapshot.get("libuv_allocator_installed") is True
            and snapshot.get("libuv_allocator_status") == 0
        ):
            fail("libuv mimalloc replacement is not installed")
    elif not (
        snapshot.get("libuv_allocator_mode") == "system"
        and snapshot.get("libuv_allocator_attempted") is False
        and snapshot.get("libuv_allocator_in_progress") is False
        and snapshot.get("libuv_allocator_installed") is False
        and snapshot.get("libuv_allocator_status") == 0
    ):
        fail("libuv system allocator runtime state is invalid")


def process_image(pid):
    proc = Path("/proc") / str(pid)
    try:
        stat_fields = (proc / "stat").read_text(encoding="utf-8").split()
        exe = (proc / "exe").resolve(strict=True)
        proc_exe = proc / "exe"
        inode = proc_exe.stat()
    except (OSError, ValueError, IndexError) as exc:
        fail(f"cannot capture process image for pid {pid}: {exc}")
    return {
        "schema_version": 1,
        "pid": int(pid),
        "starttime_ticks": int(stat_fields[21]),
        "exe_path": str(exe),
        "exe_sha256": sha256(proc_exe),
        "exe_device": inode.st_dev,
        "exe_inode": inode.st_ino,
        "captured_unix_ns": time.time_ns(),
    }


def capture_process(args):
    Path(args.output).write_text(
        json.dumps(process_image(args.pid), sort_keys=True) + "\n", encoding="utf-8")


def load_process_snapshot(path, expected_sha):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"invalid process image snapshot: {exc}")
    required = {
        "schema_version", "pid", "starttime_ticks", "exe_path", "exe_sha256",
        "exe_device", "exe_inode", "captured_unix_ns",
    }
    missing = sorted(required - data.keys())
    if missing or data.get("schema_version") != 1:
        fail("invalid process image snapshot schema: " + ",".join(missing))
    if data["exe_sha256"] != expected_sha:
        fail("running process image does not match sealed binary")
    return data


def validate_process(args):
    before = load_process_snapshot(args.before, args.expected_sha256)
    after = load_process_snapshot(args.after, args.expected_sha256)
    identity = ("pid", "starttime_ticks", "exe_sha256", "exe_device", "exe_inode")
    if any(before[key] != after[key] for key in identity):
        fail("running process image changed during matrix")


def parse_case(args):
    case = Path(args.case_dir)
    required_files = {
        name: case / name for name in (
            "qdisc-before.txt", "qdisc-after.txt", "qdisc-samples.txt",
            "ss.txt", "process.txt", "metrics.json",
        )
    }
    missing_files = [name for name, path in required_files.items() if not path.is_file()]
    if missing_files and not args.allow_incomplete:
        fail("data-validity missing files: " + ",".join(missing_files))

    def text(name):
        path = required_files[name]
        return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""

    def last_int(pattern, value):
        matches = re.findall(pattern, value)
        return int(matches[-1]) if matches else None

    before_drop = last_int(r"\bdropped\s+(\d+)", text("qdisc-before.txt"))
    after_drop = last_int(r"\bdropped\s+(\d+)", text("qdisc-after.txt"))
    backlogs = [int(value) for value in re.findall(r"\bbacklog\s+(\d+)b\b", text("qdisc-samples.txt"))]
    ss_text = text("ss.txt")
    srtt = [float(value) for value in re.findall(r"\brtt:([0-9.]+)(?:/[0-9.]+)?", ss_text)]
    inflight = [int(value) for value in re.findall(r"\bbytes_in_flight:(\d+)", ss_text)]
    inflight_source = "tcp_info_bytes_in_flight"
    if not inflight:
        # iproute2 builds which omit bytes_in_flight still expose TCP_INFO's
        # unacked segment count and current MSS on the same socket line.
        # Their product is the kernel-backed cross-version approximation.
        for line in ss_text.splitlines():
            mss = re.search(r"\bmss:(\d+)", line)
            unacked = re.search(r"\bunacked:(\d+)", line)
            if mss and unacked:
                inflight.append(int(mss.group(1)) * int(unacked.group(1)))
        inflight_source = "derived_unacked_mss"
    bandwidth = []
    for value, unit in re.findall(r"\bbw:([0-9.]+)([KMG])?bps\b", ss_text, re.I):
        scale = {"": 1e-6, "K": 1e-3, "M": 1.0, "G": 1e3}[unit.upper()]
        bandwidth.append(float(value) * scale)
    process_samples = {}
    for line in text("process.txt").splitlines():
        parts = line.split(",")
        if len(parts) != 8:
            continue
        try:
            side, phase = parts[:2]
            process_samples[(side, phase)] = tuple(int(value) for value in parts[2:])
        except ValueError:
            pass
    cpu_values, rss_bytes, context = [], [], []
    for side in ("local", "remote"):
        before = process_samples.get((side, "before"))
        after = process_samples.get((side, "after"))
        if not before or not after:
            continue
        before_ns, before_ticks, before_pages, before_ctx, hz, page_size = before
        after_ns, after_ticks, after_pages, after_ctx, after_hz, after_page_size = after
        elapsed = (after_ns - before_ns) / 1e9
        if elapsed <= 0 or hz != after_hz or page_size != after_page_size:
            continue
        cpu_values.append(max(0, after_ticks - before_ticks) / hz / elapsed * 100)
        rss_bytes.append(max(before_pages, after_pages) * page_size)
        context.append(max(0, after_ctx - before_ctx))
    try:
        metrics = json.loads(text("metrics.json"))
    except json.JSONDecodeError as exc:
        fail(f"data-validity invalid metrics JSON: {exc}")
    required_metrics = (
        "relay_pending_bytes", "relay_event_queue_full_errors", "relay_errors",
        "linux_relay_quic_send_failures", "relay_snapshot_complete",
    )
    missing = [key for key in required_metrics if key not in metrics]
    # The pre-netem root qdisc commonly has no ``dropped`` counter.  Its
    # absence means zero; the post-netem counter is mandatory evidence.
    if before_drop is None:
        before_drop = 0
    if after_drop is None:
        missing.append("qdisc_drop")
    if not backlogs:
        missing.append("qdisc_backlog")
    if not srtt:
        missing.append("srtt")
    if not bandwidth:
        missing.append("bbr_bw")
    if not inflight:
        missing.append("bytes_in_flight")
    if len(cpu_values) != 2 or len(rss_bytes) != 2 or len(context) != 2:
        missing.append("process_stats")
    if metrics.get("relay_snapshot_complete") is not True:
        missing.append("relay_snapshot_complete=true")
    if missing and not args.allow_incomplete:
        fail("data-validity missing: " + ",".join(missing))
    evidence = {"evidence_status": "incomplete" if missing else "complete",
                "evidence_missing": ",".join(sorted(set(missing)))}
    optional = {
        "qdisc_drop_delta": max(0, after_drop - before_drop) if after_drop is not None else None,
        "qdisc_backlog_max_bytes": max(backlogs) if backlogs else None,
        "srtt_avg_ms": round(statistics.mean(srtt), 3) if srtt else None,
        "srtt_max_ms": max(srtt) if srtt else None,
        "bbr_bw_avg_mbps": round(statistics.mean(bandwidth), 3) if bandwidth else None,
        "bytes_in_flight_max": max(inflight) if inflight else None,
        "bytes_in_flight_source": inflight_source if inflight else None,
        "cpu_pct": round(sum(cpu_values), 3) if len(cpu_values) == 2 else None,
        "rss_bytes": sum(rss_bytes) if len(rss_bytes) == 2 else None,
        "context_switches": sum(context) if len(context) == 2 else None,
        "pending_bytes": int(metrics["relay_pending_bytes"]) if "relay_pending_bytes" in metrics else None,
        "event_queue_full_errors_max": int(metrics["relay_event_queue_full_errors"]) if "relay_event_queue_full_errors" in metrics else None,
        "relay_errors": int(metrics["relay_errors"]) if "relay_errors" in metrics else None,
        "quic_send_failures": int(metrics["linux_relay_quic_send_failures"]) if "linux_relay_quic_send_failures" in metrics else None,
        "send_error_scope": "linux_relay" if "linux_relay_quic_send_failures" in metrics else None,
    }
    evidence.update({key: value for key, value in optional.items() if value is not None})
    Path(args.output).write_text(json.dumps(evidence, sort_keys=True) + "\n", encoding="utf-8")


def parse_args():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    seal = sub.add_parser("seal-build")
    seal.add_argument("--backend", choices=("native", "libuv"), required=True)
    seal.add_argument("--binary", required=True)
    seal.add_argument("--cmake-cache", required=True)
    seal.add_argument("--provenance", required=True)
    seal.add_argument("--output", required=True)
    seal.set_defaults(func=seal_build)
    live = sub.add_parser("seal-live-build")
    live.add_argument("--backend", choices=("native", "libuv"), required=True)
    live.add_argument("--binary", required=True)
    live.add_argument("--cmake-cache", required=True)
    live.add_argument("--source-root", required=True)
    live.add_argument("--source-base", required=True)
    live.add_argument("--target-sources", required=True)
    live.add_argument("--backend-sources", default="")
    live.add_argument("--link-libraries", required=True)
    live.add_argument("--provenance", required=True)
    live.add_argument("--output", required=True)
    live.set_defaults(func=seal_live_build)
    verify = sub.add_parser("verify-build")
    verify.add_argument("--backend", choices=("native", "libuv"), required=True)
    verify.add_argument("--binary", required=True)
    verify.add_argument("--cmake-cache", required=True)
    verify.add_argument("--manifest", required=True)
    verify.set_defaults(func=verify_build)
    compare = sub.add_parser("compare-builds")
    compare.add_argument("--native", required=True)
    compare.add_argument("--libuv", required=True)
    compare.set_defaults(func=compare_builds)
    result = sub.add_parser("validate-result")
    result.add_argument("--root", required=True)
    result.set_defaults(func=validate_result)
    allocator = sub.add_parser("validate-allocator")
    allocator.add_argument("--backend", choices=("native", "libuv"), required=True)
    allocator.add_argument("--manifest", required=True)
    allocator.add_argument("--snapshot", required=True)
    allocator.set_defaults(func=validate_allocator)
    capture = sub.add_parser("capture-process")
    capture.add_argument("--pid", type=int, required=True)
    capture.add_argument("--output", required=True)
    capture.set_defaults(func=capture_process)
    process = sub.add_parser("validate-process")
    process.add_argument("--before", required=True)
    process.add_argument("--after", required=True)
    process.add_argument("--expected-sha256", required=True)
    process.set_defaults(func=validate_process)
    case = sub.add_parser("parse-case")
    case.add_argument("--case-dir", required=True)
    case.add_argument("--output", required=True)
    case.add_argument("--allow-incomplete", action="store_true")
    case.set_defaults(func=parse_case)
    return parser.parse_args()


def main():
    args = parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
