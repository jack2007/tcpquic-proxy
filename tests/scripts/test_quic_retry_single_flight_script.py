from pathlib import Path


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
    assert "201" in text
    assert "2900" in text
    assert "healthy_probe_failures" in text
    assert "event=quic_connecting" in text
    assert "connected_connections" in text
    assert '"$BASE_URL/peers"' in text
    assert '"$BASE_URL/metrics"' in text
    assert "pidstat" in text


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
