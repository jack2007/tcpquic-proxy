#!/usr/bin/env python3
import json
import os
import subprocess
import time
from datetime import datetime
from pathlib import Path

ROOT = Path("/home/jack/src/tcpquic-proxy")
BIN = ROOT / "build/bin/Release/tcpquic-proxy"
REMOTE = "jack@169.254.59.196"
TARGET = "169.254.59.196"
BIND = "169.254.250.230"
IFACE = "enp1s0f0np0"
REMOTE_DIR = "/home/jack/tcpquic-dgx-bin"
REMOTE_BIN = f"{REMOTE_DIR}/tcpquic-proxy"
IPERF = os.environ.get("IPERF3", "iperf3")
DURATION = int(os.environ.get("DURATION_SEC", "30"))
TCP_WRITE_MAX_BYTES = int(os.environ.get("TCP_WRITE_MAX_BYTES", "0"))
TCP_WRITE_BURST_BYTES = int(os.environ.get("TCP_WRITE_BURST_BYTES", "0"))
CAPTURE_PERF = os.environ.get("CAPTURE_PERF", "1") != "0"
TS = datetime.now().strftime("%Y%m%d-%H%M%S")
OUT = ROOT / "docs" / f"dgx-download-slow-diagnose-{TS}"
CERT_SRC = sorted((ROOT / "docs").glob("dgx-iperf-proxy-bottleneck-*/certs"))[-1]
BASE = 34000 + (int(time.time()) % 4000)
IPERF_PORT = BASE
QUIC_PORT = BASE + 1
PROXY_PORT = BASE + 2
CLIENT_ADMIN = BASE + 3
SERVER_ADMIN = BASE + 4

remote_pids = []
client_proc = None


def log(msg):
    print(f"[download-diagnose] {msg}", flush=True)


def run(cmd, check=True, timeout=None):
    log("+ " + " ".join(str(x) for x in cmd))
    p = subprocess.run(
        [str(x) for x in cmd],
        cwd=str(ROOT),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    if check and p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise RuntimeError("failed: " + " ".join(str(x) for x in cmd))
    return p


def ssh(script, check=True, timeout=None):
    return run(["ssh", "-o", "BatchMode=yes", REMOTE, script], check=check, timeout=timeout)


def remote_bg(name, script):
    path = f"/tmp/{name}-{TS}.log"
    p = ssh(f"nohup bash -lc {json.dumps(script)} >{path} 2>&1 & echo $!")
    pid = p.stdout.strip().splitlines()[-1]
    remote_pids.append(pid)
    log(f"remote {name} pid={pid} log={path}")
    return pid, path


def cleanup():
    global client_proc
    if client_proc is not None and client_proc.poll() is None:
        client_proc.terminate()
        try:
            client_proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            client_proc.kill()
            client_proc.wait(timeout=5)
    client_proc = None
    for pid in reversed(remote_pids):
        ssh(f"kill -TERM {pid} 2>/dev/null || true; sleep 0.2; kill -KILL {pid} 2>/dev/null || true", check=False)
    remote_pids.clear()


def wait_remote_log(path, needle, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if ssh(f"grep -q {needle!r} {path} 2>/dev/null", check=False).returncode == 0:
            return True
        time.sleep(0.3)
    return False


def wait_local_log(path, needle, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if path.exists() and needle in path.read_text(errors="ignore"):
            return True
        if client_proc is not None and client_proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def proxy_args(q):
    args = [
        "--tuning", "wan",
        "--iw", "4000",
        "--initrtt-ms", "100",
        "--relay-io-size", "1048576",
        "--connections", str(q),
        "--compress", "off",
        "--linux-relay-read-chunk-size", "1048576",
        "--linux-relay-worker-slots", "1024",
    ]
    if TCP_WRITE_MAX_BYTES > 0:
        args += ["--linux-relay-tcp-write-max-bytes", str(TCP_WRITE_MAX_BYTES)]
    if TCP_WRITE_BURST_BYTES > 0:
        args += ["--linux-relay-tcp-write-burst-bytes", str(TCP_WRITE_BURST_BYTES)]
    return args


def metrics(port, remote=False):
    url = f"http://127.0.0.1:{port}/metrics"
    p = ssh(f"curl -fsS --max-time 3 {url}", check=False) if remote else run(["curl", "-fsS", "--max-time", "3", url], check=False)
    if p.returncode != 0:
        return {}
    try:
        return json.loads(p.stdout)
    except Exception:
        return {}


def diff(before, after):
    out = {}
    for key, av in after.items():
        bv = before.get(key)
        if isinstance(av, (int, float)) and isinstance(bv, (int, float)):
            out[key] = av - bv
    return out


def bps_from_iperf(text):
    try:
        data = json.loads(text)
    except Exception:
        return 0.0
    vals = []
    end = data.get("end", {})
    for key in ("sum_sent", "sum_received", "sum"):
        value = end.get(key)
        if isinstance(value, dict) and "bits_per_second" in value:
            vals.append(float(value["bits_per_second"]))
    return max(vals) if vals else 0.0


def start_stack(q):
    global client_proc
    run(["rsync", "-az", str(CERT_SRC) + "/", f"{REMOTE}:/home/jack/tcpquic-dgx-certs/"])
    run(["sudo", "ip", "route", "replace", TARGET, "dev", IFACE, "src", BIND], check=False)
    ssh(f"sudo ip route replace {BIND} dev {IFACE} src {TARGET} 2>/dev/null; true", check=False)
    remote_bg("iperf3-download", f"iperf3 -s -B {TARGET} -p {IPERF_PORT}")
    deadline = time.time() + 10
    while time.time() < deadline:
        if ssh(f"ss -lnt | grep -q ':{IPERF_PORT} '", check=False).returncode == 0:
            break
        time.sleep(0.3)
    args = " ".join(proxy_args(q))
    _, server_log = remote_bg(
        "tcpquic-server-download",
        f"LD_LIBRARY_PATH={REMOTE_DIR} {REMOTE_BIN} server "
        f"--listen {TARGET}:{QUIC_PORT} "
        f"--allow-targets {TARGET}/32,127.0.0.0/8 "
        f"--admin-listen 127.0.0.1:{SERVER_ADMIN} "
        f"--cert /home/jack/tcpquic-dgx-certs/server.crt "
        f"--key /home/jack/tcpquic-dgx-certs/server.key "
        f"--ca /home/jack/tcpquic-dgx-certs/ca.crt "
        f"{args}",
    )
    if not wait_remote_log(server_log, "QUIC server listening"):
        raise RuntimeError(ssh(f"tail -100 {server_log}", check=False).stdout)

    client_log = OUT / "proxy-client.log"
    f = client_log.open("w")
    cmd = [
        BIN, "client",
        "--peer", f"{TARGET}:{QUIC_PORT}",
        "--http-listen", f"127.0.0.1:{PROXY_PORT}",
        "--socks-listen", f"127.0.0.1:{PROXY_PORT + 1000}",
        "--admin-listen", f"127.0.0.1:{CLIENT_ADMIN}",
        "--cert", CERT_SRC / "client.crt",
        "--key", CERT_SRC / "client.key",
        "--ca", CERT_SRC / "ca.crt",
    ] + proxy_args(q)
    log("+ " + " ".join(str(x) for x in cmd))
    client_proc = subprocess.Popen([str(x) for x in cmd], cwd=str(ROOT), stdout=f, stderr=subprocess.STDOUT)
    if not wait_local_log(client_log, "HTTP CONNECT listening"):
        raise RuntimeError(client_log.read_text(errors="ignore")[-4000:])


def start_ss_sampler(case_dir):
    ss_path = case_dir / "ss-loopback-samples.txt"
    script = (
        f"for i in $(seq 1 {DURATION + 4}); do "
        f"echo '=== sample '$i' '$(date +%s.%N)' ==='; "
        f"ss -tinmp '( sport = :{PROXY_PORT} or dport = :{PROXY_PORT} )' || true; "
        f"sleep 1; "
        f"done"
    )
    f = ss_path.open("w")
    return subprocess.Popen(["bash", "-lc", script], cwd=str(ROOT), stdout=f, stderr=subprocess.STDOUT)


def run_download():
    case_dir = OUT / "proxy-download-q16-P16"
    case_dir.mkdir(parents=True, exist_ok=True)
    cb = metrics(CLIENT_ADMIN)
    sb = metrics(SERVER_ADMIN, remote=True)
    perf = None
    if CAPTURE_PERF:
        perf = subprocess.Popen([
            "sudo", "perf", "record", "-F", "999", "-g", "--call-graph", "dwarf,16384",
            "-p", str(client_proc.pid), "-o", str(case_dir / "client.perf.data"),
            "--", "sleep", str(DURATION + 2),
        ], cwd=str(ROOT), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ss_sampler = start_ss_sampler(case_dir)
    time.sleep(1)
    cmd = [
        IPERF, "-c", TARGET, "-B", BIND, "-p", str(IPERF_PORT),
        "-P", "16", "-t", str(DURATION), "--json", "--connect-timeout", "5000",
        "--proxy", f"http://127.0.0.1:{PROXY_PORT}", "--reverse",
    ]
    p = run(cmd, check=False, timeout=DURATION + 45)
    perf_out, perf_err = "", ""
    if perf is not None:
        perf_out, perf_err = perf.communicate(timeout=DURATION + 30)
    ss_sampler.wait(timeout=10)
    ca = metrics(CLIENT_ADMIN)
    sa = metrics(SERVER_ADMIN, remote=True)
    (case_dir / "iperf.stdout.json").write_text(p.stdout)
    (case_dir / "iperf.stderr.txt").write_text(p.stderr)
    (case_dir / "client.metrics.before.json").write_text(json.dumps(cb, indent=2, sort_keys=True))
    (case_dir / "client.metrics.after.json").write_text(json.dumps(ca, indent=2, sort_keys=True))
    (case_dir / "server.metrics.before.json").write_text(json.dumps(sb, indent=2, sort_keys=True))
    (case_dir / "server.metrics.after.json").write_text(json.dumps(sa, indent=2, sort_keys=True))
    (case_dir / "client.metrics.delta.json").write_text(json.dumps(diff(cb, ca), indent=2, sort_keys=True))
    (case_dir / "server.metrics.delta.json").write_text(json.dumps(diff(sb, sa), indent=2, sort_keys=True))
    (case_dir / "client.perf.record.out").write_text((perf_out or "") + (perf_err or ""))
    if CAPTURE_PERF:
        top = run([
            "sudo", "perf", "report", "--stdio", "-i", case_dir / "client.perf.data",
            "--no-children", "--sort", "comm,dso,symbol",
        ], check=False, timeout=90)
        (case_dir / "client.top.txt").write_text((top.stdout or "") + (top.stderr or ""))
    row = {
        "case": "proxy-download-q16-P16",
        "exit": p.returncode,
        "gbps": bps_from_iperf(p.stdout) / 1e9,
        "client_delta": diff(cb, ca),
        "server_delta": diff(sb, sa),
    }
    return row


def summarize(row):
    c = row["client_delta"]
    s = row["server_delta"]
    calls = c.get("linux_relay_tcp_write_sendmsg_calls", 0)
    avg = c.get("linux_relay_tcp_write_bytes", 0) / calls / 1024 if calls else 0
    lines = [
        "# DGX download slow diagnosis",
        "",
        f"- Date: {datetime.now().isoformat(timespec='seconds')}",
        f"- Duration: {DURATION}s",
        f"- TCP write max bytes: {TCP_WRITE_MAX_BYTES if TCP_WRITE_MAX_BYTES > 0 else 'uncapped'}",
        f"- TCP write burst bytes: {TCP_WRITE_BURST_BYTES if TCP_WRITE_BURST_BYTES > 0 else 'uncapped'}",
        f"- Perf capture: {'on' if CAPTURE_PERF else 'off'}",
        "- Case: `iperf3 --reverse -P 16` through one tcpquic-proxy client/server pair, `--connections 16`.",
        f"- Output directory: `{OUT}`",
        "",
        "| Case | Exit | Gbps |",
        "|---|---:|---:|",
        f"| {row['case']} | {row['exit']} | {row['gbps']:.3f} |",
        "",
        "| Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | eagain | partial | burst stops | max pending MiB | max queue | pause | resume | errors |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for side, d in (("client", c), ("server", s)):
        side_calls = d.get("linux_relay_tcp_write_sendmsg_calls", 0)
        side_avg = d.get("linux_relay_tcp_write_bytes", 0) / side_calls / 1024 if side_calls else 0
        lines.append(
            f"| {side} | {d.get('linux_relay_tcp_read_bytes', 0)/1024**3:.2f} | "
            f"{d.get('linux_relay_tcp_write_bytes', 0)/1024**3:.2f} | {side_calls} | {side_avg:.1f} | "
            f"{d.get('linux_relay_tcp_write_eagain_count', 0)} | "
            f"{d.get('linux_relay_tcp_write_partial_count', 0)} | "
            f"{d.get('linux_relay_tcp_write_burst_stops', 0)} | "
            f"{d.get('linux_relay_max_pending_quic_receive_bytes', 0)/1024**2:.1f} | "
            f"{d.get('linux_relay_max_pending_quic_receive_queue', 0)} | "
            f"{d.get('linux_relay_quic_receive_paused_count', 0)} | "
            f"{d.get('linux_relay_quic_receive_resumed_count', 0)} | "
            f"{d.get('linux_relay_errors', 0)} |"
        )
    lines += [
        "",
        "## Captured Evidence",
        "",
        f"- Client perf data: `{OUT / 'proxy-download-q16-P16/client.perf.data'}`" if CAPTURE_PERF else "- Client perf data: not captured",
        f"- Client perf top: `{OUT / 'proxy-download-q16-P16/client.top.txt'}`" if CAPTURE_PERF else "- Client perf top: not captured",
        f"- Loopback socket samples: `{OUT / 'proxy-download-q16-P16/ss-loopback-samples.txt'}`",
        "",
        "## Immediate Reading",
        "",
        f"- Client TCP write avg size: {avg:.1f} KiB.",
        f"- Client TCP write EAGAIN count: {c.get('linux_relay_tcp_write_eagain_count', 0)}.",
        f"- Client max pending QUIC receive: {c.get('linux_relay_max_pending_quic_receive_bytes', 0)/1024**2:.1f} MiB.",
    ]
    (OUT / "summary.md").write_text("\n".join(lines) + "\n")
    (OUT / "results.json").write_text(json.dumps(row, indent=2, sort_keys=True))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    start_stack(16)
    row = run_download()
    summarize(row)
    log(f"summary: {OUT / 'summary.md'}")


if __name__ == "__main__":
    try:
        main()
    finally:
        cleanup()
