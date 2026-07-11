# Relay runtime snapshot lab scripts

## k6 (`tests/k6/relay_workers.js`)

```bash
k6 run -e SCENARIO=admin_mixed -e ADMIN_TOKEN=$TOKEN \
  -e BASE_URL=http://127.0.0.1:8080/api/v1 \
  -e WORKER_IDS=windows-0 -e DURATION=30s \
  tests/k6/relay_workers.js
```

Scenarios: `console_baseline`, `console_peak`, `admin_mixed`, `admin_spike`,
`admin_soak`, `breaking_point`.

## Recovery harness

```bash
python tests/scripts/run_relay_runtime_snapshot_recovery.py --help
python tests/scripts/test_run_relay_runtime_snapshot_recovery.py
```

## Release-lab manual gates (not PR CI)

These remain a release prerequisite on Linux / macOS / Windows:

- 4,096 active relays under Admin workers polling
- k6 baseline / peak / breaking point
- data-plane throughput and payload correctness vs baseline
- worker freeze under console polling
- SIGKILL / TerminateProcess + supervisor restart within T0+90s

See `.github/workflows/relay-runtime-snapshot-nightly.yml`.
