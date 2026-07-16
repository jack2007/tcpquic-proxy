#!/usr/bin/env bash
# Reproducible Linux native build/test evidence collector.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build/native-plan"
OUT="$ROOT/build/native-test-evidence"
BUILD_DRIVER=""
EXPECTED_TEST_COUNT=43
FIXTURE=false
BUILD_DRIVER_OVERRIDDEN=false
EXPECTED_TEST_COUNT_OVERRIDDEN=false

usage() {
  echo "usage: $0 [--build-dir DIR] [--output-dir DIR] [--fixture [--build-driver PATH] [--expected-test-count N]]" >&2
}
while (($#)); do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --output-dir) OUT="$2"; shift 2 ;;
    --build-driver)
      BUILD_DRIVER="$2"; BUILD_DRIVER_OVERRIDDEN=true; shift 2 ;;
    --expected-test-count)
      EXPECTED_TEST_COUNT="$2"; EXPECTED_TEST_COUNT_OVERRIDDEN=true; shift 2 ;;
    --fixture) FIXTURE=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 64 ;;
  esac
done
[[ "$(uname -s)" == Linux ]] || { echo "native evidence is Linux-only" >&2; exit 69; }
if ! $FIXTURE && { $BUILD_DRIVER_OVERRIDDEN || $EXPECTED_TEST_COUNT_OVERRIDDEN; }; then
  echo "--build-driver and --expected-test-count require explicit --fixture mode" >&2
  exit 64
fi
[[ "$EXPECTED_TEST_COUNT" =~ ^[1-9][0-9]*$ ]] || { echo "expected test count must be positive" >&2; exit 64; }
if $FIXTURE; then
  EXECUTION_MODE=fixture
else
  EXECUTION_MODE=production
  EXPECTED_TEST_COUNT=43
fi

mkdir -p "$OUT/logs"
STARTED_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
GIT_HEAD="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=all 2>/dev/null)" ]]; then
  GIT_DIRTY=true
else
  GIT_DIRTY=false
fi
FORMAL_PREFLIGHT="$OUT/formal-preflight.json"
if [[ "$EXECUTION_MODE" == production ]]; then
  python3 - "$ROOT" "$BUILD_DIR/CMakeCache.txt" \
    "$ROOT/scripts/native-test-suite.txt" "$FORMAL_PREFLIGHT" <<'PY'
import json, os, pathlib, shutil, subprocess, sys
root, cache_path, suite_path, output = map(pathlib.Path, sys.argv[1:])
root = root.resolve()
errors = []
cache = {}
if not cache_path.is_file():
    errors.append("CMakeCache.txt is missing")
else:
    for line in cache_path.read_text(errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        key_type, value = line.split("=", 1)
        key, _ = key_type.split(":", 1)
        cache[key] = value
home = pathlib.Path(cache.get("CMAKE_HOME_DIRECTORY", "missing"))
if not home.is_absolute() or home.resolve() != root:
    errors.append("CMAKE_HOME_DIRECTORY does not resolve to repository root")
if cache.get("TCPQUIC_RELAY_BACKEND") != "native":
    errors.append("TCPQUIC_RELAY_BACKEND is not native")
cmake = pathlib.Path(cache.get("CMAKE_COMMAND", "missing"))
if not cmake.is_absolute() or not cmake.exists() or not os.access(cmake, os.X_OK):
    errors.append("CMAKE_COMMAND is not an executable absolute path")
cmake = cmake.resolve() if cmake.is_absolute() else cmake
suite = [line.strip() for line in suite_path.read_text().splitlines() if line.strip()]
if len(suite) != 43 or len(set(suite)) != 43:
    errors.append("canonical native test suite must contain 43 unique names")
git = pathlib.Path(shutil.which("git") or "")
if not git.is_absolute():
    errors.append("git executable cannot be resolved")
def run_git(*args):
    return subprocess.run([str(git), "-C", str(root), *args], check=True,
                          text=True, capture_output=True).stdout
try:
    head = run_git("rev-parse", "HEAD").strip()
    status = run_git("status", "--porcelain", "--untracked-files=all")
    submodule_raw = run_git("submodule", "status", "--recursive").rstrip("\n")
    if status:
        errors.append("Git superproject is dirty")
    if any(not line.startswith(" ") or "-dirty" in line
           for line in submodule_raw.splitlines()):
        errors.append("recursive submodules are not initialized and clean")
except (OSError, subprocess.CalledProcessError) as exc:
    head, status, submodule_raw = "unknown", "error", ""
    errors.append(f"Git identity validation failed: {exc}")
value = {"valid": not errors, "errors": errors, "git_executable": str(git),
         "head": head, "superproject_status": status,
         "submodule_commits": submodule_raw.replace("\n", "|"),
         "canonical_tests": suite, "cmake_cache_path": str(cache_path.resolve()),
         "cmake_home_directory": str(home.resolve()) if home.is_absolute() else str(home),
         "cmake_executable": str(cmake),
         "compiled_relay_backend": cache.get("TCPQUIC_RELAY_BACKEND")}
json.dump(value, open(output, "w"), indent=2)
if errors:
    print("native formal preflight: " + "; ".join(errors), file=sys.stderr)
    raise SystemExit(65)
PY
  PREFLIGHT_RC=$?
  ((PREFLIGHT_RC == 0)) || exit "$PREFLIGHT_RC"
  CMAKE_EXECUTABLE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["cmake_executable"])' "$FORMAL_PREFLIGHT")"
  # Formal evidence must never reuse ignored outputs sealed at an older HEAD.
  # A clean build makes CMake relink raypx2 and every canonical test so their
  # POST_BUILD manifests bind to the preflight identity above.
  BUILD_CMD=("$CMAKE_EXECUTABLE" --build "$BUILD_DIR" --clean-first -j2)
elif [[ -n "$BUILD_DRIVER" ]]; then
  BUILD_CMD=("$BUILD_DRIVER" "$BUILD_DIR")
else
  BUILD_CMD=(cmake --build "$BUILD_DIR" -j2)
fi
python3 - "$OUT/build-command.json" "${BUILD_CMD[@]}" <<'PY'
import json,sys
json.dump(sys.argv[2:],open(sys.argv[1],"w"),indent=2)
PY
BUILD_STARTED="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
"${BUILD_CMD[@]}" >"$OUT/build.log" 2>&1
BUILD_RC=$?
BUILD_ENDED="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

RESULTS="$OUT/tests.tsv"
: >"$RESULTS"
failed=0
count=0
if ((BUILD_RC == 0)); then
  for binary in "$BUILD_DIR"/bin/Release/tcpquic_*_test; do
    [[ -f "$binary" && -x "$binary" ]] || continue
    name="${binary##*/}"
    started="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    "$binary" >"$OUT/logs/$name.log" 2>&1
    rc=$?
    ended="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    resolved="$(readlink -f "$binary")"
    digest="$(sha256sum "$resolved" | awk '{print $1}')"
    read -r size mtime_epoch_ns < <(python3 - "$resolved" <<'PY'
import pathlib,sys
value=pathlib.Path(sys.argv[1]).stat(); print(value.st_size,value.st_mtime_ns)
PY
)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$name" "$resolved" "$digest" "$size" "$mtime_epoch_ns" "$started" "$ended" "$rc" >>"$RESULTS"
    ((count+=1))
    ((rc==0)) || ((failed+=1))
  done
fi
ENDED_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
FORMAL_VALIDATION="$OUT/formal-validation.json"
FORMAL_VALIDATION_RC=0
if [[ "$EXECUTION_MODE" == production ]]; then
  TEST_BINARY_VALIDATION="$OUT/test-binary-validation.json"
  python3 "$ROOT/scripts/native-test-binary-evidence.py" verify-suite \
    --root "$ROOT" --cache "$BUILD_DIR/CMakeCache.txt" \
    --binary-dir "$BUILD_DIR/bin/Release" \
    --suite "$ROOT/scripts/native-test-suite.txt" \
    --output "$TEST_BINARY_VALIDATION" || FORMAL_VALIDATION_RC=$?
  python3 - "$ROOT" "$BUILD_DIR" "$FORMAL_PREFLIGHT" "$RESULTS" \
    "$TEST_BINARY_VALIDATION" "$FORMAL_VALIDATION" <<'PY' || FORMAL_VALIDATION_RC=$?
import hashlib, json, os, pathlib, shutil, subprocess, sys
root, build, preflight_path, results_path, test_validation_path, output = sys.argv[1:]
root, build = pathlib.Path(root).resolve(), pathlib.Path(build).resolve()
preflight = json.load(open(preflight_path))
errors = []
git = preflight["git_executable"]
def run_git(*args):
    return subprocess.run([git, "-C", str(root), *args], check=True,
                          text=True, capture_output=True).stdout
head = run_git("rev-parse", "HEAD").strip()
status = run_git("status", "--porcelain", "--untracked-files=all")
submodules_raw = run_git("submodule", "status", "--recursive").rstrip("\n")
submodules = submodules_raw.replace("\n", "|")
if status:
    errors.append("Git superproject became dirty")
if head != preflight["head"]:
    errors.append("Git HEAD changed during evidence collection")
if any(not line.startswith(" ") or "-dirty" in line
       for line in submodules_raw.splitlines()):
    errors.append("recursive submodules are not initialized and clean")
if submodules != preflight["submodule_commits"]:
    errors.append("submodule identity changed during evidence collection")
names = [line.split("\t", 1)[0] for line in open(results_path) if line.strip()]
canonical = preflight["canonical_tests"]
if len(names) != len(set(names)) or set(names) != set(canonical):
    errors.append("executed native test names do not exactly match canonical suite")
test_binary_evidence = json.load(open(test_validation_path))
if not test_binary_evidence.get("valid"):
    errors.append("linked native test binary evidence is invalid")
binary = build / "bin" / "Release" / "raypx2"
manifest_path = binary.with_name("raypx2.build-manifest.json")
manifest = {}
if not binary.is_file():
    errors.append("raypx2 binary is missing")
if not manifest_path.is_file():
    errors.append("sealed raypx2 build manifest is missing")
else:
    try:
        manifest = json.load(open(manifest_path))
    except (OSError, ValueError) as exc:
        errors.append(f"sealed raypx2 build manifest is invalid: {exc}")
if manifest:
    if manifest.get("compiled_relay_backend") != "native":
        errors.append("sealed manifest backend is not native")
    if manifest.get("source_commit") != head:
        errors.append("sealed manifest source_commit does not match HEAD")
    if manifest.get("source_tree_state") != "clean":
        errors.append("sealed manifest source_tree_state is not clean")
    if manifest.get("submodule_commits") != submodules:
        errors.append("sealed manifest submodule identity does not match checkout")
    binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest() if binary.is_file() else ""
    if manifest.get("binary_sha256") != binary_sha:
        errors.append("sealed manifest binary_sha256 does not match raypx2")
    cache_path = pathlib.Path(preflight["cmake_cache_path"])
    cache_sha = hashlib.sha256(cache_path.read_bytes()).hexdigest()
    if pathlib.Path(manifest.get("cmake_cache_path", "missing")).resolve() != cache_path:
        errors.append("sealed manifest cmake_cache_path does not match build cache")
    if manifest.get("cmake_cache_sha256") != cache_sha:
        errors.append("sealed manifest cmake_cache_sha256 does not match build cache")
else:
    binary_sha = ""
value = {"valid": not errors, "errors": errors, "head": head,
         "superproject_status": status, "submodule_commits": submodules,
         "canonical_test_count": len(canonical), "executed_test_names": names,
         "cmake_executable": preflight["cmake_executable"],
         "cmake_cache_path": preflight["cmake_cache_path"],
         "cmake_home_directory": preflight["cmake_home_directory"],
         "compiled_relay_backend": preflight["compiled_relay_backend"],
         "manifest_path": str(manifest_path), "binary_path": str(binary),
         "binary_sha256": binary_sha, "manifest": manifest}
value["test_binary_evidence"] = test_binary_evidence
json.dump(value, open(output, "w"), indent=2)
if errors:
    print("native formal validation: " + "; ".join(errors), file=sys.stderr)
    raise SystemExit(1)
PY
fi
python3 - "$OUT/index.json" "$OUT/build-command.json" "$RESULTS" \
  "$STARTED_UTC" "$ENDED_UTC" "$GIT_HEAD" "$GIT_DIRTY" \
  "$BUILD_STARTED" "$BUILD_ENDED" "$BUILD_RC" "$count" "$failed" \
  "$EXPECTED_TEST_COUNT" "$EXECUTION_MODE" "$FORMAL_VALIDATION" <<'PY'
import json,sys
(out,command,results,started,ended,head,dirty,build_started,build_ended,
 build_rc,count,failed,expected,execution_mode,formal_validation_path)=sys.argv[1:]
tests=[]
sidecars={}
if execution_mode == "production":
    provenance_preview=json.load(open(formal_validation_path))
    sidecars={item["target_name"]:item for item in
              provenance_preview["test_binary_evidence"]["tests"]}
for line in open(results):
    name,path,digest,size,mtime_epoch_ns,test_started,test_ended,rc=line.rstrip("\n").split("\t")
    tests.append({"name":name,"path":path,"sha256":digest,"size":int(size),
                  "mtime_epoch_ns":int(mtime_epoch_ns),"started_utc":test_started,
                  "ended_utc":test_ended,"exit_code":int(rc),
                  "build_manifest":sidecars.get(name)})
provenance=(json.load(open(formal_validation_path))
            if execution_mode == "production" else None)
passed=(int(build_rc)==0 and int(failed)==0 and int(count)==int(expected)
        and (provenance is None or provenance["valid"]))
formal_status=("passed" if passed else "failed") if execution_mode == "production" else "not_applicable"
value={"schema_version":1,"platform":"linux","execution_mode":execution_mode,
       "formal_gate_status":formal_status,"started_utc":started,"ended_utc":ended,
       "git":{"head":head,"dirty":dirty=="true"},
       "build":{"command":json.load(open(command)),"started_utc":build_started,
                "ended_utc":build_ended,"exit_code":int(build_rc),"log":"build.log"},
       "expected_test_count":int(expected),"test_count":int(count),"failed":int(failed),
       "valid":execution_mode == "production" and passed,
       "build_provenance":provenance,
       "tests":tests}
json.dump(value,open(out,"w"),indent=2)
PY

if [[ "$EXECUTION_MODE" == fixture ]]; then
  ((BUILD_RC==0 && failed==0 && count==EXPECTED_TEST_COUNT))
else
  ((BUILD_RC==0 && failed==0 && count==EXPECTED_TEST_COUNT && FORMAL_VALIDATION_RC==0))
fi
