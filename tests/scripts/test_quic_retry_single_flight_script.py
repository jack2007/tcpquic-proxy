import csv
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "scripts" / "run-quic-client-retry-single-flight-test.sh"
K6 = ROOT / "tests" / "k6" / "quic_retry_reconnect.js"


def test_harness_is_process_safe_and_uses_dynamic_ports():
    text = HARNESS.read_text()
    assert 'TMP="$(mktemp -d)"' in text
    assert "PIDS=()" in text
    assert "trap cleanup EXIT INT TERM" in text
    assert 'for pid in "${PIDS[@]:-}"' in text
    assert 'kill "$pid"' in text
    assert "pkill" not in text
    assert "killall" not in text
    assert text.count("alloc_port)") >= 6
    assert text.count('PIDS+=("$!")') >= 3


def test_harness_covers_two_peer_isolation_and_recovery_contract():
    text = HARNESS.read_text()
    assert '"id":"failed"' in text
    assert '"id":"healthy"' in text
    assert "$failed_quic_port" in text
    assert "$healthy_quic_port" in text
    assert "$failed_socks_port" in text
    assert "$healthy_socks_port" in text
    assert "SOAK_SECONDS:-600" in text
    assert "RECOVERY_TIMEOUT_MS:-3500" in text
    assert "math.ceil(duration / 3) + 1" in text
    assert "2900" in text
    assert "healthy_probe_failures" in text
    assert "event=quic_connecting" in text
    assert "connected_connections" in text
    assert '"$BASE_URL/peers"' in text
    assert '"$BASE_URL/metrics"' in text
    assert "pidstat" in text
    assert "/proc/uptime" in text
    assert "--connect-timeout" in text
    assert "--max-time" in text
    assert "Linux" in text
    assert "TRACE_LOG" in text
    assert "extract_trace_evidence" in text
    assert '"$trace_start_bytes"' in text
    assert '"$actual_attempt_window_ms"' in text


def _write_fixture(path, *, missing=False, bad_timestamp=False, rising=False, warmup_plateau=False,
                   queue_rising=False, cpu_hot=False, log_rising=False,
                   log_jitter=False, log_slow_acceleration=False,
                   reactor_bad=False, close_timestamps=False, nonmonotonic=False,
                   late_timestamps=False, burst_before_ready=False, over_limit=False):
    (path / "admin").mkdir()
    metrics = {
        "ingress_delayed_task_queue_depth": 1,
        "ingress_delayed_task_queue_depth_max": 2,
        "ingress_reactor_timeout_overshoot_max_us": 200,
        "ingress_reactor_timeout_overshoot_p95_us": 100,
        "ingress_reactor_timeout_overshoot_p99_us": 150,
        "ingress_reactor_timeout_overshoot_samples": 5,
        "peers": [{
            "peer_id": "failed", "retry_scheduled_total": 3,
            "retry_executed_total": 2, "retry_stale_dropped_total": 1,
            "retry_schedule_failed_total": 0,
        }],
    }
    for i in range(4):
        sample = json.loads(json.dumps(metrics))
        if queue_rising:
            sample["ingress_delayed_task_queue_depth"] = i + 1
        if reactor_bad:
            sample["ingress_reactor_timeout_overshoot_p99_us"] = 500001
        if missing:
            del sample["ingress_delayed_task_queue_depth"]
        (path / "admin" / f"metrics-{i * 10000}.json").write_text(json.dumps(sample))
    values = [100, 100, 100, 100, 100]
    if rising:
        values = [10000] * 6 + [10000, 10200, 10100, 10500, 10400, 10600, 10900, 10800, 11000]
    if warmup_plateau:
        values = [10000, 12000, 14000, 16000, 18000, 20000] + [20000, 20100, 19900] * 3
    if log_rising or log_jitter or log_slow_acceleration:
        values = [10000] * 15
    log_values = [100] * len(values)
    if log_rising:
        rates = [500] * 6 + [500, 520, 480, 700, 720, 680, 900, 920]
        log_values = [100]
        for rate in rates:
            log_values.append(log_values[-1] + rate * 10)
    if log_jitter:
        rates = [470, 810, 470, 470, 810, 470, 640, 644, 470, 640, 644, 470, 810, 470]
        log_values = [100]
        for rate in rates:
            log_values.append(log_values[-1] + rate * 10)
    if log_slow_acceleration:
        rates = [500] * 6 + [500, 500, 500, 531, 531, 531, 562, 562]
        log_values = [100]
        for rate in rates:
            log_values.append(log_values[-1] + rate * 10)
    with (path / "resources.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(("elapsed_ms", "cpu_ticks", "clock_ticks_per_second", "rss_kb", "trace_log_bytes"))
        for i, value in enumerate(values):
            ticks = i * (100 if cpu_hot else 1)
            writer.writerow((i * 10000, ticks, 100, value, log_values[i]))
    timestamp = "not-a-time" if bad_timestamp else "2026-07-12 10:00:00.000"
    lines = [f"[{timestamp}] [info] event=quic_connecting peer=127.0.0.1:12345\n"]
    if close_timestamps:
        lines.append("[2026-07-12 10:00:01.000] [info] event=quic_connecting peer=127.0.0.1:12345\n")
    if nonmonotonic:
        lines.append("[2026-07-12 09:59:59.000] [info] event=quic_connecting peer=127.0.0.1:12345\n")
    if late_timestamps:
        lines.append("[2026-07-12 10:00:04.000] [info] event=quic_connecting peer=127.0.0.1:12345\n")
    if burst_before_ready:
        (path / "healthy-ready-ms.txt").write_text("999999\n")
        lines.append("[2026-07-12 10:00:00.100] [info] event=quic_connecting peer=127.0.0.1:12345\n")
    if over_limit:
        lines = [
            f"[2026-07-12 10:00:{i * 3:02d}.000] [info] event=quic_connecting peer=127.0.0.1:12345\n"
            for i in range(16)
        ]
    (path / "client-trace.log").write_text("".join(lines))


def _analyze(path, *, soak_seconds=40, attempt_window_ms=None):
    env = os.environ.copy()
    env.update(ANALYZE_FIXTURE=str(path), FAILED_QUIC_PORT="12345", SOAK_SECONDS=str(soak_seconds))
    if attempt_window_ms is not None:
        env["ANALYZE_ATTEMPT_WINDOW_MS"] = str(attempt_window_ms)
    return subprocess.run([str(HARNESS)], env=env, text=True, capture_output=True)


def test_fixture_analysis_reads_real_metric_shape(tmp_path):
    _write_fixture(tmp_path)
    result = _analyze(tmp_path)
    assert result.returncode == 0, result.stderr
    row = list(csv.DictReader((tmp_path / "retry-metrics.csv").open()))[0]
    assert row["retry_scheduled_total"] == "3"
    assert row["ingress_delayed_task_queue_depth"] == "1"


def test_fixture_analysis_rejects_missing_metrics(tmp_path):
    _write_fixture(tmp_path, missing=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "missing metric" in result.stderr


def test_fixture_analysis_rejects_bad_connecting_timestamp(tmp_path):
    _write_fixture(tmp_path, bad_timestamp=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "timestamp" in result.stderr


def test_fixture_analysis_rejects_three_rising_resource_windows(tmp_path):
    _write_fixture(tmp_path, rising=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "monotonic rise" in result.stderr


def test_fixture_analysis_accepts_allocator_warmup_that_reaches_plateau(tmp_path):
    _write_fixture(tmp_path, warmup_plateau=True)
    result = _analyze(tmp_path)
    assert result.returncode == 0, result.stderr


def test_fixture_analysis_accepts_rss_growth_just_below_tolerance(tmp_path):
    _write_fixture(tmp_path)
    values = [10000] * 6 + [10000] * 3 + [10250] * 3 + [10500] * 3
    with (tmp_path / "resources.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(("elapsed_ms", "cpu_ticks", "clock_ticks_per_second", "rss_kb", "trace_log_bytes"))
        for i, value in enumerate(values):
            writer.writerow((i * 10000, i, 100, value, 100))
    result = _analyze(tmp_path, soak_seconds=140)
    assert result.returncode == 0, result.stderr


def test_fixture_analysis_rejects_empty_rss_time_window(tmp_path):
    _write_fixture(tmp_path)
    with (tmp_path / "resources.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(("elapsed_ms", "cpu_ticks", "clock_ticks_per_second", "rss_kb", "trace_log_bytes"))
        writer.writerows(((0, 0, 100, 10000, 100), (100000, 1, 100, 10000, 100), (110000, 2, 100, 10000, 100)))
    result = _analyze(tmp_path, soak_seconds=110)
    assert result.returncode != 0
    assert "rss time window has no samples" in result.stderr


def test_fixture_cpu_final_window_uses_soak_not_attempt_duration(tmp_path):
    _write_fixture(tmp_path)
    with (tmp_path / "resources.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(("elapsed_ms", "cpu_ticks", "clock_ticks_per_second", "rss_kb", "trace_log_bytes"))
        writer.writerows(((0, 0, 100, 10000, 100), (30000, 0, 100, 10000, 100),
                          (60000, 1800, 100, 10000, 100), (90000, 1800, 100, 10000, 100),
                          (120000, 1800, 100, 10000, 100), (150000, 1800, 100, 10000, 100),
                          (180000, 1800, 100, 10000, 100)))
    result = _analyze(tmp_path, soak_seconds=180, attempt_window_ms=240000)
    assert result.returncode != 0
    assert "final CPU mean" in result.stderr


def test_fixture_cpu_empty_interval_fallback_uses_soak_end(tmp_path):
    _write_fixture(tmp_path)
    with (tmp_path / "resources.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(("elapsed_ms", "cpu_ticks", "clock_ticks_per_second", "rss_kb", "trace_log_bytes"))
        writer.writerow((180000, 0, 100, 10000, 100))
    result = _analyze(tmp_path, soak_seconds=180, attempt_window_ms=240000)
    assert result.returncode != 0
    assert "rss requires at least three samples" in result.stderr
    # The CPU fallback is established before RSS validation and must use the
    # soak-relative endpoint, never the longer attempt window.
    assert "cpu_elapsed=[soak_ms]" in HARNESS.read_text()


def test_fixture_analysis_rejects_fewer_than_three_rss_samples(tmp_path):
    for count in (1, 2):
        case = tmp_path / str(count)
        case.mkdir()
        _write_fixture(case)
        with (case / "resources.csv").open("w", newline="") as out:
            writer = csv.writer(out)
            writer.writerow(("elapsed_ms", "cpu_ticks", "clock_ticks_per_second", "rss_kb", "trace_log_bytes"))
            for i in range(count):
                writer.writerow((i * 10000, i, 100, 10000, 100))
        result = _analyze(case)
        assert result.returncode != 0
        assert "rss requires at least three samples" in result.stderr


def test_harness_trace_extraction_keeps_startup_burst(tmp_path):
    _write_fixture(tmp_path, burst_before_ready=True)
    raw = tmp_path / "raw-client-trace.log"
    (tmp_path / "client-trace.log").rename(raw)
    env = os.environ.copy()
    env.update(
        ANALYZE_FIXTURE=str(tmp_path), FAILED_QUIC_PORT="12345", SOAK_SECONDS="40",
        EXTRACT_TRACE_SOURCE=str(raw), TRACE_START_BYTES="0",
    )
    result = subprocess.run([str(HARNESS)], env=env, text=True, capture_output=True)
    assert result.returncode != 0
    assert "below 2900ms" in result.stderr


def test_fixture_analysis_rejects_nonmonotonic_connecting_timestamps(tmp_path):
    _write_fixture(tmp_path, nonmonotonic=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "strictly monotonic" in result.stderr


def test_fixture_analysis_rejects_early_retry_interval(tmp_path):
    _write_fixture(tmp_path, close_timestamps=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "below 2900ms" in result.stderr


def test_fixture_analysis_rejects_hot_cpu(tmp_path):
    _write_fixture(tmp_path, cpu_hot=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "CPU mean" in result.stderr


def test_fixture_analysis_rejects_queue_growth(tmp_path):
    _write_fixture(tmp_path, queue_rising=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "delayed queue depth monotonic rise" in result.stderr


def test_fixture_analysis_rejects_trace_log_growth(tmp_path):
    _write_fixture(tmp_path, log_rising=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "trace log growth rate monotonic rise" in result.stderr


def test_fixture_analysis_accepts_constant_trace_rate_with_periodic_jitter(tmp_path):
    _write_fixture(tmp_path, log_jitter=True)
    result = _analyze(tmp_path, soak_seconds=140)
    assert result.returncode == 0, result.stderr


def test_fixture_analysis_rejects_two_small_compounding_log_rate_increases(tmp_path):
    _write_fixture(tmp_path, log_slow_acceleration=True)
    result = _analyze(tmp_path, soak_seconds=140)
    assert result.returncode != 0
    assert "trace log growth rate monotonic rise" in result.stderr


def test_fixture_analysis_splits_log_intervals_at_window_boundaries(tmp_path):
    _write_fixture(tmp_path)
    with (tmp_path / "resources.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(("elapsed_ms", "cpu_ticks", "clock_ticks_per_second", "rss_kb", "trace_log_bytes"))
        # Final 60% is [60000,150000], with boundaries at 90000 and 120000.
        # Rates around the boundaries are 400, 500, 600, 470 B/s. Assigning
        # each whole interval by t1 falsely yields 400 -> 500 -> 535 B/s;
        # proportional splitting yields about 450 -> 567 -> 492 B/s.
        elapsed_values = (0, 50000, 75000, 100000, 125000, 150000)
        rates = (400, 400, 500, 600, 470)
        size = 100
        writer.writerow((elapsed_values[0], 0, 100, 10000, size))
        for before, elapsed, rate in zip(elapsed_values, elapsed_values[1:], rates):
            size += rate * (elapsed - before) // 1000
            writer.writerow((elapsed, 0, 100, 10000, size))
    result = _analyze(tmp_path, soak_seconds=150)
    assert result.returncode == 0, result.stderr


def test_fixture_analysis_rejects_reactor_delay(tmp_path):
    _write_fixture(tmp_path, reactor_bad=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "reactor p99 delay" in result.stderr


def test_fixture_analysis_rejects_readiness_preceding_burst(tmp_path):
    _write_fixture(tmp_path, burst_before_ready=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "below 2900ms" in result.stderr


def test_fixture_analysis_rejects_retry_later_than_3500ms(tmp_path):
    _write_fixture(tmp_path, late_timestamps=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "above 3500ms" in result.stderr


def test_fixture_analysis_rejects_attempts_over_dynamic_limit(tmp_path):
    _write_fixture(tmp_path, over_limit=True)
    result = _analyze(tmp_path)
    assert result.returncode != 0
    assert "exceed 15" in result.stderr


def test_harness_supports_k6_handoff():
    text = HARNESS.read_text()
    assert "HOLD_FOR_K6_SECONDS:-0" in text
    assert "ENV_OUT" in text
    for name in ("TOKEN", "BASE_URL", "CLIENT_PID", "RESULT_ROOT"):
        assert f"{name}=" in text


def test_k6_reconnect_contract():
    text = K6.read_text()
    assert "constant-arrival-rate" in text
    assert "ADMIN_TOKEN" in text
    assert "BASE_URL" in text
    assert "PEER_ID" in text
    assert "/connections/conn-0:reconnect" in text
    assert "r.status === 202" in text
    for scenario in ("baseline", "peak", "spike", "soak", "breaking_point"):
        assert scenario in text
    for threshold in (
        "http_req_failed: ['rate==0']",
        "checks: ['rate==1']",
        "dropped_iterations: ['count==0']",
        "http_req_duration: ['p(95)<200', 'p(99)<500']",
    ):
        assert threshold in text
