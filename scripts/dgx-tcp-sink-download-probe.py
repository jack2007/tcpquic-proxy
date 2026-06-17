#!/usr/bin/env python3
import json
import os
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path("/home/jack/src/tcpquic-proxy")
REMOTE = "jack@169.254.59.196"
TARGET = "169.254.59.196"
BIND = "169.254.250.230"
IFACE = "enp1s0f0np0"
REMOTE_DIR = "/home/jack/tcpquic-dgx-bin"
PROXY_BIN = ROOT / "build/bin/Release/tcpquic-proxy"
SINK_BIN = ROOT / "build/bin/Release/tcpquic_tcp_sink_bench"
REMOTE_PROXY_BIN = f"{REMOTE_DIR}/tcpquic-proxy"
REMOTE_SINK_BIN = f"{REMOTE_DIR}/tcpquic_tcp_sink_bench"
STREAMS = int(os.environ.get("STREAMS", "16"))
DURATION = int(os.environ.get("DURATION_SEC", "30"))
SERVER_DURATION = int(os.environ.get("SERVER_DURATION_SEC", str(DURATION + 15)))
CAPTURE_PERF = os.environ.get("CAPTURE_PERF", "0") != "0"
TCP_WRITE_MAX_BYTES = int(os.environ.get("TCP_WRITE_MAX_BYTES", "4194304"))
TCP_WRITE_BURST_BYTES = int(os.environ.get("TCP_WRITE_BURST_BYTES", "0"))
TS = datetime.now().strftime("%Y%m%d-%H%M%S")
OUT = ROOT / "docs" / f"dgx-tcp-sink-download-probe-{TS}"
CERT_SRC = sorted((ROOT / "docs").glob("dgx-iperf-proxy-bottleneck-*/certs"))[-1]
BASE = 38000 + (int(time.time()) % 4000)

remote_pids = []
client_proc = None


def log(msg):
    print(f"[tcp-sink-probe] {msg}", flush=True)


def run(cmd, check=True, timeout=None, cwd=ROOT):
    log("+ " + " ".join(str(x) for x in cmd))
    p = subprocess.run(
        [str(x) for x in cmd],
        cwd=str(cwd),
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


def wait_remote_log(path, needle, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if ssh(f"grep -q {needle!r} {path} 2>/dev/null", check=False).returncode == 0:
            return True
        time.sleep(0.2)
    return False


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


def parse_json_line(text):
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            return json.loads(line)
    raise RuntimeError("no JSON result found")


def endpoint_port(value):
    if not isinstance(value, str) or ":" not in value:
        return None
    try:
        return int(value.rsplit(":", 1)[1])
    except ValueError:
        return None


def sample_hot_relay_socket(client_admin, output_path, stop_event):
    seen = set()
    with output_path.open("w") as out:
        while not stop_event.is_set():
            snapshot = metrics(client_admin)
            local = snapshot.get("linux_relay_hot_relay_local", "")
            peer = snapshot.get("linux_relay_hot_relay_peer", "")
            local_port = endpoint_port(local)
            peer_port = endpoint_port(peer)
            now = datetime.now().isoformat(timespec="milliseconds")
            out.write(
                f"\n=== {now} hot_relay={snapshot.get('linux_relay_hot_relay_id', 0)} "
                f"worker={snapshot.get('linux_relay_hot_relay_worker_index', 0)} "
                f"fd={snapshot.get('linux_relay_hot_relay_tcp_fd', -1)} "
                f"local={local} peer={peer} "
                f"pending={snapshot.get('linux_relay_hot_relay_pending_quic_receive_bytes', 0)} "
                f"queue={snapshot.get('linux_relay_hot_relay_pending_quic_receive_queue', 0)} "
                f"write_bytes={snapshot.get('linux_relay_hot_relay_tcp_write_bytes', 0)} "
                f"eagain={snapshot.get('linux_relay_hot_relay_tcp_write_eagain_count', 0)} "
                f"epollout={snapshot.get('linux_relay_hot_relay_epollout_events', 0)} "
                f"write_armed={snapshot.get('linux_relay_hot_relay_tcp_write_armed', False)} ===\n")
            if local_port is not None and peer_port is not None:
                key = (local_port, peer_port)
                seen.add(key)
                cmd = [
                    "ss", "-tinm",
                    "sport", "=", f":{local_port}",
                    "and", "dport", "=", f":{peer_port}",
                ]
                p = run(cmd, check=False)
                out.write(p.stdout or "")
                out.write(p.stderr or "")
            out.flush()
            stop_event.wait(1.0)
        if seen:
            out.write("\n=== sampled tuples ===\n")
            for local_port, peer_port in sorted(seen):
                out.write(f"local_port={local_port} peer_port={peer_port}\n")


def proxy_args(q):
    args = [
        "--tuning", "custom",
        "--fcw", "1073741824",
        "--srw", "1073741824",
        "--iw", "4000",
        "--initrtt-ms", "1",
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


def start_remote_sink(port):
    return remote_bg(
        "tcp-sink-server",
        f"{REMOTE_SINK_BIN} server --listen {TARGET}:{port} --streams {STREAMS} --duration {SERVER_DURATION}",
    )


def run_sink_client(name, port, proxy_port=None, client_admin=None):
    case_dir = OUT / name
    case_dir.mkdir(parents=True, exist_ok=True)
    args = [
        str(SINK_BIN), "client",
        "--connect", f"{TARGET}:{port}",
        "--streams", str(STREAMS),
        "--duration", str(DURATION),
    ]
    if proxy_port is not None:
        args += ["--proxy", f"http://127.0.0.1:{proxy_port}"]

    perf = None
    if CAPTURE_PERF and proxy_port is not None:
        perf = subprocess.Popen([
            "sudo", "perf", "record", "-F", "99", "-g", "-p", str(client_proc.pid),
            "-o", str(case_dir / "client.perf.data"), "--", "sleep", str(DURATION)
        ], cwd=str(ROOT), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.5)

    sampler_stop = threading.Event()
    sampler = None
    if proxy_port is not None and client_admin is not None:
        sampler = threading.Thread(
            target=sample_hot_relay_socket,
            args=(client_admin, case_dir / "hot-relay-ss-samples.txt", sampler_stop),
            daemon=True,
        )
        sampler.start()

    p = run(args, check=False, timeout=DURATION + 30)
    if sampler is not None:
        sampler_stop.set()
        sampler.join(timeout=5)
    (case_dir / "sink-client.stdout.json").write_text(p.stdout)
    (case_dir / "sink-client.stderr.txt").write_text(p.stderr)
    perf_out = perf_err = ""
    if perf is not None:
        perf_out, perf_err = perf.communicate(timeout=DURATION + 20)
        (case_dir / "client.perf.record.out").write_text((perf_out or "") + (perf_err or ""))
        top = run([
            "sudo", "perf", "report", "--stdio", "-i", case_dir / "client.perf.data",
            "--no-children", "--sort", "comm,dso,symbol",
        ], check=False, timeout=90)
        (case_dir / "client.top.txt").write_text((top.stdout or "") + (top.stderr or ""))
    return {"exit": p.returncode, "result": parse_json_line(p.stdout)}


def start_proxy(quic_port, proxy_port, client_admin, server_admin):
    global client_proc
    cert_remote = "/home/jack/tcpquic-dgx-certs"
    server_args = " ".join([
        REMOTE_PROXY_BIN,
        "server",
        "--listen", f"{TARGET}:{quic_port}",
        "--allow-targets", f"{TARGET}/32,127.0.0.0/8",
        "--admin-listen", f"127.0.0.1:{server_admin}",
        "--cert", f"{cert_remote}/server.crt",
        "--key", f"{cert_remote}/server.key",
        "--ca", f"{cert_remote}/ca.crt",
        *proxy_args(STREAMS),
    ])
    _, server_log = remote_bg("tcpquic-server-sink", f"LD_LIBRARY_PATH={REMOTE_DIR} {server_args}")
    if not wait_remote_log(server_log, "QUIC server listening", timeout=15):
        raise RuntimeError("server did not start")

    client_args = [
        str(PROXY_BIN),
        "client",
        "--peer", f"{TARGET}:{quic_port}",
        "--http-listen", f"127.0.0.1:{proxy_port}",
        "--socks-listen", f"127.0.0.1:{proxy_port + 1000}",
        "--admin-listen", f"127.0.0.1:{client_admin}",
        "--cert", str(CERT_SRC / "client.crt"),
        "--key", str(CERT_SRC / "client.key"),
        "--ca", str(CERT_SRC / "ca.crt"),
        *proxy_args(STREAMS),
    ]
    log("+ " + " ".join(client_args))
    client_proc = subprocess.Popen(
        client_args,
        cwd=str(ROOT),
        stdout=(OUT / "proxy-client.log").open("w"),
        stderr=subprocess.STDOUT,
        text=True,
    )
    end = time.time() + 15
    while time.time() < end:
        if metrics(client_admin):
            return
        if client_proc.poll() is not None:
            raise RuntimeError("client exited early")
        time.sleep(0.2)
    raise RuntimeError("client admin did not start")


def summarize(rows):
    lines = [
        "# DGX tcp sink download probe",
        "",
        f"- Date: {datetime.now().isoformat(timespec='seconds')}",
        f"- Streams: {STREAMS}",
        f"- Duration: {DURATION}s",
        f"- TCP write max bytes: {TCP_WRITE_MAX_BYTES if TCP_WRITE_MAX_BYTES > 0 else 'uncapped'}",
        f"- TCP write burst bytes: {TCP_WRITE_BURST_BYTES if TCP_WRITE_BURST_BYTES > 0 else 'uncapped'}",
        f"- Perf capture: {'on' if CAPTURE_PERF else 'off'}",
        f"- Output directory: `{OUT}`",
        "",
        "| Case | Exit | Gbps | Bytes GiB |",
        "|---|---:|---:|---:|",
    ]
    for row in rows:
        result = row["result"]
        lines.append(
            f"| {row['case']} | {row['exit']} | {result.get('gbps', 0):.3f} | "
            f"{result.get('bytes', 0)/1024**3:.2f} |"
        )
    proxy = next((row for row in rows if row["case"] == "proxy-http-connect"), None)
    if proxy is not None:
        c = proxy.get("client_delta", {})
        s = proxy.get("server_delta", {})
        client_after = proxy.get("client_after", {})
        server_after = proxy.get("server_after", {})
        lines += [
            "",
            "| Side | relays | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | current pending MiB | max worker pending MiB | max relay pending MiB | max relay queue | max relay EAGAIN | errors |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
        for side, d in (("client", c), ("server", s)):
            calls = d.get("linux_relay_tcp_write_sendmsg_calls", 0)
            avg = d.get("linux_relay_tcp_write_bytes", 0) / calls / 1024 if calls else 0
            lines.append(
                f"| {side} | {d.get('linux_relay_active_relays', 0)} | "
                f"{d.get('linux_relay_tcp_read_bytes', 0)/1024**3:.2f} | "
                f"{d.get('linux_relay_tcp_write_bytes', 0)/1024**3:.2f} | {calls} | "
                f"{avg:.1f} | {d.get('linux_relay_tcp_write_eagain_count', 0)} | "
                f"{d.get('linux_relay_pending_bytes', 0)/1024**2:.1f} | "
                f"{d.get('linux_relay_max_worker_pending_bytes', 0)/1024**2:.1f} | "
                f"{d.get('linux_relay_max_relay_pending_quic_receive_bytes', 0)/1024**2:.1f} | "
                f"{d.get('linux_relay_max_relay_pending_quic_receive_queue', 0)} | "
                f"{d.get('linux_relay_max_relay_tcp_write_eagain_count', 0)} | "
                f"{d.get('linux_relay_errors', 0)} |"
            )
        lines += [
            "",
            "| Side | hot relay | worker | fd | local | peer | hot pending MiB | hot queue | hot write GiB | hot EAGAIN | EPOLLOUT | write armed |",
            "|---|---:|---:|---:|---|---|---:|---:|---:|---:|---:|---|",
        ]
        for side, d in (("client", client_after), ("server", server_after)):
            lines.append(
                f"| {side} | {d.get('linux_relay_hot_relay_id', 0)} | "
                f"{d.get('linux_relay_hot_relay_worker_index', 0)} | "
                f"{d.get('linux_relay_hot_relay_tcp_fd', -1)} | "
                f"`{d.get('linux_relay_hot_relay_local', '')}` | "
                f"`{d.get('linux_relay_hot_relay_peer', '')}` | "
                f"{d.get('linux_relay_hot_relay_pending_quic_receive_bytes', 0)/1024**2:.1f} | "
                f"{d.get('linux_relay_hot_relay_pending_quic_receive_queue', 0)} | "
                f"{d.get('linux_relay_hot_relay_tcp_write_bytes', 0)/1024**3:.2f} | "
                f"{d.get('linux_relay_hot_relay_tcp_write_eagain_count', 0)} | "
                f"{d.get('linux_relay_hot_relay_epollout_events', 0)} | "
                f"{d.get('linux_relay_hot_relay_tcp_write_armed', False)} |"
            )
        sample_path = OUT / "proxy-http-connect" / "hot-relay-ss-samples.txt"
        lines += [
            "",
            "## Hot Relay Socket Samples",
            "",
            f"- `{sample_path}`",
        ]
    (OUT / "summary.md").write_text("\n".join(lines) + "\n")
    (OUT / "results.json").write_text(json.dumps(rows, indent=2, sort_keys=True))
    log(f"summary: {OUT / 'summary.md'}")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    run(["rsync", "-az", str(PROXY_BIN), f"{REMOTE}:{REMOTE_PROXY_BIN}"])
    run(["rsync", "-az", str(SINK_BIN), f"{REMOTE}:{REMOTE_SINK_BIN}"])
    run(["rsync", "-az", f"{CERT_SRC}/", f"{REMOTE}:/home/jack/tcpquic-dgx-certs/"])
    run(["sudo", "ip", "route", "replace", TARGET, "dev", IFACE, "src", BIND], check=False)
    ssh(f"sudo ip route replace {BIND} dev {IFACE} src {TARGET} 2>/dev/null; true", check=False)

    rows = []
    try:
        direct_port = BASE
        start_remote_sink(direct_port)
        time.sleep(0.5)
        direct = run_sink_client("direct-tcp-sink", direct_port)
        direct["case"] = "direct-tcp-sink"
        rows.append(direct)
        cleanup()

        sink_port = BASE + 10
        quic_port = BASE + 11
        proxy_port = BASE + 12
        client_admin = BASE + 13
        server_admin = BASE + 14
        start_remote_sink(sink_port)
        start_proxy(quic_port, proxy_port, client_admin, server_admin)
        cb = metrics(client_admin)
        sb = metrics(server_admin, remote=True)
        proxy = run_sink_client(
            "proxy-http-connect",
            sink_port,
            proxy_port=proxy_port,
            client_admin=client_admin)
        ca = metrics(client_admin)
        sa = metrics(server_admin, remote=True)
        proxy["case"] = "proxy-http-connect"
        proxy["client_delta"] = diff(cb, ca)
        proxy["server_delta"] = diff(sb, sa)
        proxy["client_after"] = ca
        proxy["server_after"] = sa
        case_dir = OUT / "proxy-http-connect"
        (case_dir / "client.metrics.before.json").write_text(json.dumps(cb, indent=2, sort_keys=True))
        (case_dir / "client.metrics.after.json").write_text(json.dumps(ca, indent=2, sort_keys=True))
        (case_dir / "client.metrics.delta.json").write_text(json.dumps(proxy["client_delta"], indent=2, sort_keys=True))
        (case_dir / "server.metrics.before.json").write_text(json.dumps(sb, indent=2, sort_keys=True))
        (case_dir / "server.metrics.after.json").write_text(json.dumps(sa, indent=2, sort_keys=True))
        (case_dir / "server.metrics.delta.json").write_text(json.dumps(proxy["server_delta"], indent=2, sort_keys=True))
        rows.append(proxy)
        summarize(rows)
    finally:
        cleanup()


if __name__ == "__main__":
    main()
