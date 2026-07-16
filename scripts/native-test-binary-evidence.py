#!/usr/bin/env python3
"""Seal and verify link-time evidence for the canonical native test suite."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cache_values(path):
    values = {}
    for line in Path(path).read_text(errors="replace").splitlines():
        key_type, separator, value = line.partition("=")
        if not separator or ":" not in key_type or line.startswith(("#", "//")):
            continue
        key, _ = key_type.split(":", 1)
        values[key] = value
    return values


def run_git(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args], check=True, text=True,
        capture_output=True).stdout


def git_identity(root):
    status = run_git(root, "status", "--porcelain", "--untracked-files=all")
    raw = run_git(root, "submodule", "status", "--recursive").rstrip("\n")
    return {
        "source_commit": run_git(root, "rev-parse", "HEAD").strip(),
        "source_tree_state": "dirty" if status else "clean",
        "submodule_commits": raw.replace("\n", "|"),
        "submodules_clean": all(
            line.startswith(" ") and "-dirty" not in line
            for line in raw.splitlines()),
    }


def build_identity(root, cache):
    root, cache = Path(root).resolve(), Path(cache).resolve()
    values = cache_values(cache)
    if Path(values.get("CMAKE_HOME_DIRECTORY", "missing")).resolve() != root:
        raise ValueError("CMAKE_HOME_DIRECTORY does not match source root")
    if values.get("TCPQUIC_RELAY_BACKEND") != "native":
        raise ValueError("TCPQUIC_RELAY_BACKEND is not native")
    return {
        "compiled_relay_backend": "native",
        "cmake_cache_path": str(cache),
        "cmake_cache_sha256": sha256(cache),
    }


def seal_test(args):
    root, binary, cache = map(Path, (args.root, args.binary, args.cache))
    root, cache = root.resolve(), cache.resolve()
    # Keep the canonical path for symlinked test aliases. Resolving the binary
    # would make tcpquic_tcp_tunnel_test overwrite tcpquic_tunnel_test's
    # sidecar even though the evidence suite executes both names.
    binary = binary.absolute()
    if not binary.is_file():
        raise ValueError(f"test binary is missing: {binary}")
    identity = git_identity(root)
    manifest = {
        "schema_version": 1,
        "target_name": args.target,
        "binary_path": str(binary),
        "binary_sha256": sha256(binary),
        **identity,
        **build_identity(root, cache),
    }
    manifest.pop("submodules_clean")
    output = Path(args.output) if args.output else binary.with_name(
        binary.name + ".build-manifest.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


def verify_suite(args):
    root, cache = Path(args.root).resolve(), Path(args.cache).resolve()
    binary_dir, suite_path = Path(args.binary_dir).resolve(), Path(args.suite)
    errors, tests = [], []
    try:
        current = {**git_identity(root), **build_identity(root, cache)}
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        current = {}
        errors.append(f"current build identity is invalid: {exc}")
    if current and (current["source_tree_state"] != "clean" or
                    not current["submodules_clean"]):
        errors.append("source checkout or recursive submodules are dirty")
    suite = [line.strip() for line in suite_path.read_text().splitlines()
             if line.strip()]
    if len(suite) != 43 or len(set(suite)) != 43:
        errors.append("canonical suite must contain 43 unique target names")
    discovered = sorted(
        path.name for path in binary_dir.glob("tcpquic_*_test")
        if path.is_file() and os.access(path, os.X_OK))
    if len(discovered) != len(set(discovered)) or set(discovered) != set(suite):
        errors.append("linked test binary set does not exactly match canonical suite")
    for target in suite:
        binary = binary_dir / target
        sidecar = binary.with_name(binary.name + ".build-manifest.json")
        item_errors = []
        manifest = {}
        if not binary.is_file():
            item_errors.append("binary is missing")
        if not sidecar.is_file():
            item_errors.append("sidecar is missing")
        else:
            try:
                manifest = json.loads(sidecar.read_text())
            except (OSError, json.JSONDecodeError) as exc:
                item_errors.append(f"sidecar is invalid: {exc}")
        if manifest:
            expected = {
                "target_name": target,
                "binary_path": str(binary),
                "source_commit": current.get("source_commit"),
                "source_tree_state": "clean",
                "submodule_commits": current.get("submodule_commits"),
                "compiled_relay_backend": "native",
                "cmake_cache_path": str(cache),
                "cmake_cache_sha256": current.get("cmake_cache_sha256"),
            }
            for field, value in expected.items():
                if manifest.get(field) != value:
                    item_errors.append(f"{field} mismatch")
            if binary.is_file() and manifest.get("binary_sha256") != sha256(binary):
                item_errors.append("binary_sha256 mismatch")
        if item_errors:
            errors.extend(f"{target}: {message}" for message in item_errors)
        tests.append({
            "target_name": target,
            "binary_path": str(binary),
            "binary_sha256": sha256(binary) if binary.is_file() else "",
            "sidecar_path": str(sidecar),
            "sidecar_sha256": sha256(sidecar) if sidecar.is_file() else "",
            "valid": not item_errors,
            "errors": item_errors,
        })
    result = {
        "schema_version": 1,
        "valid": not errors,
        "errors": errors,
        "canonical_test_count": len(suite),
        "current_identity": current,
        "tests": tests,
    }
    Path(args.output).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if errors:
        raise ValueError("; ".join(errors))


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    seal = subparsers.add_parser("seal-test")
    seal.add_argument("--root", required=True)
    seal.add_argument("--cache", required=True)
    seal.add_argument("--binary", required=True)
    seal.add_argument("--target", required=True)
    seal.add_argument("--output")
    seal.set_defaults(function=seal_test)
    verify = subparsers.add_parser("verify-suite")
    verify.add_argument("--root", required=True)
    verify.add_argument("--cache", required=True)
    verify.add_argument("--binary-dir", required=True)
    verify.add_argument("--suite", required=True)
    verify.add_argument("--output", required=True)
    verify.set_defaults(function=verify_suite)
    args = parser.parse_args()
    try:
        args.function(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"native test evidence: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
