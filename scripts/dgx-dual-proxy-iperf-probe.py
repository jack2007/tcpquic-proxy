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
IPERF = os.environ.get("IPERF3", "/home/jack/src/iperf/src/iperf3")
DURATION = int(os.environ.get("DURATION_SEC", "30"))
LANES = int(os.environ.get("LANES", "2"))
Q_PER_LANE = int(os.environ.get("Q_PER_LANE", "8"))
P_PER_LANE = int(os.environ.get("P_PER_LANE", "8"))
TCP_WRITE_MAX_BYTES = int(os.environ.get("TCP_WRITE_MAX_BYTES", "0"))
TCP_WRITE_BURST_BYTES = int(os.environ.get("TCP_WRITE_BURST_BYTES", "0"))
DIRECTIONS = [item.strip() for item in os.environ.get("DIRECTIONS", "upload,download").split(",") if item.strip()]
TS = datetime.now().strftime("%Y%m%d-%H%M%S")
OUT = ROOT / "docs" / f"dgx-dual-proxy-iperf-probe-{TS}"
CERT_SRC = sorted((ROOT / "docs").glob("dgx-iperf-proxy-bottleneck-*/certs"))[-1]
BASE = 42000 + (int(time.time()) % 3000)

remote_pids = []
client_procs = []


def log(msg):
    print(f"[dual-proxy] {msg}", flush=True)


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
    for proc in reversed(client_procs):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
    client_procs.clear()
    for pid in reversed(remote_pids):
        ssh(
            f"kill -TERM {pid} 2>/dev/null || true; "
            f"sleep 0.2; kill -KILL {pid} 2>/dev/null || true",
            check=False,
        )
    remote_pids.clear()


def wait_remote_log(path, needle, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if ssh(f"grep -q {needle!r} {path} 2>/dev/null", check=False).returncode == 0:
            return True
        time.sleep(0.3)
    return False


def metrics(port, remote=False):
    url = f"http://127.0.0.1:{port}/metrics"
    p = ssh(f"curl -fsS --max-time 3 {url}", check=False) if remote else run(
        ["curl", "-fsS", "--max-time", "3", url],
        check=False,
    )
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


def proxy_args():
    args = [
        "--tuning", "custom",
        "--quic-fcw", "1073741824",
        "--quic-srw", "1073741824",
        "--quic-iw", "4000",
        "--quic-initrtt-ms", "1",
        "--relay-io-size", "1048576",
        "--relay-inflight-bytes", "1073741824",
        "--max-memory-mb", "8192",
        "--quic-connections", str(Q_PER_LANE),
        "--compress", "off",
        "--linux-relay-read-chunk-size", "1048576",
        "--linux-relay-worker-slots", "1024",
    ]
    if TCP_WRITE_MAX_BYTES > 0:
        args += ["--linux-relay-tcp-write-max-bytes", str(TCP_WRITE_MAX_BYTES)]
    if TCP_WRITE_BURST_BYTES > 0:
        args += ["--linux-relay-tcp-write-burst-bytes", str(TCP_WRITE_BURST_BYTES)]
    return args


def lane_ports(index):
    lane_base = BASE + index * 20
    return {
        "iperf": lane_base,
        "quic": lane_base + 1,
        "http": lane_base + 2,
        "socks": lane_base + 3,
        "client_admin": lane_base + 4,
        "server_admin": lane_base + 5,
    }


def start_lane(index):
    ports = lane_ports(index)
    remote_bg(
        f"iperf3-lane{index + 1}",
        f"iperf3 -s -B {TARGET} -p {ports['iperf']}",
    )
    deadline = time.time() + 10
    while time.time() < deadline:
        if ssh(f"ss -lnt | grep -q ':{ports['iperf']} '", check=False).returncode == 0:
            break
        time.sleep(0.2)

    cert_remote = "/home/jack/tcpquic-dgx-certs"
    server_cmd = " ".join([
        REMOTE_BIN,
        "server",
        "--quic-listen", f"{TARGET}:{ports['quic']}",
        "--allow-targets", f"{TARGET}/32,127.0.0.0/8",
        "--admin-listen", f"127.0.0.1:{ports['server_admin']}",
        "--quic-cert", f"{cert_remote}/server.crt",
        "--quic-key", f"{cert_remote}/server.key",
        "--quic-ca", f"{cert_remote}/ca.crt",
        *proxy_args(),
    ])
    _, server_log = remote_bg(
        f"tcpquic-server-lane{index + 1}",
        f"LD_LIBRARY_PATH={REMOTE_DIR} {server_cmd}",
    )
    if not wait_remote_log(server_log, "QUIC server listening"):
        raise RuntimeError(ssh(f"tail -100 {server_log}", check=False).stdout)

    client_log = OUT / f"proxy-client-lane{index + 1}.log"
    cmd = [
        str(BIN),
        "client",
        "--quic-peer", f"{TARGET}:{ports['quic']}",
        "--http-listen", f"127.0.0.1:{ports['http']}",
        "--socks-listen", f"127.0.0.1:{ports['socks']}",
        "--admin-listen", f"127.0.0.1:{ports['client_admin']}",
        "--quic-cert", str(CERT_SRC / "client.crt"),
        "--quic-key", str(CERT_SRC / "client.key"),
        "--quic-ca", str(CERT_SRC / "ca.crt"),
        *proxy_args(),
    ]
    log("+ " + " ".join(cmd))
    proc = subprocess.Popen(
        cmd,
        cwd=str(ROOT),
        stdout=client_log.open("w"),
        stderr=subprocess.STDOUT,
        text=True,
    )
    client_procs.append(proc)
    deadline = time.time() + 15
    while time.time() < deadline:
        if metrics(ports["client_admin"]):
            return ports
        if proc.poll() is not None:
            raise RuntimeError(client_log.read_text(errors="ignore")[-4000:])
        time.sleep(0.2)
    raise RuntimeError(client_log.read_text(errors="ignore")[-4000:])


def run_iperf_lane(case_dir, lane_index, ports, reverse):
    args = [
        IPERF,
        "-c", TARGET,
        "-B", BIND,
        "-p", str(ports["iperf"]),
        "-P", str(P_PER_LANE),
        "-t", str(DURATION),
        "--json",
        "--connect-timeout", "5000",
        "--proxy", f"http://127.0.0.1:{ports['http']}",
    ]
    if reverse:
        args.append("--reverse")
    log("+ " + " ".join(str(x) for x in args))
    out = (case_dir / f"lane{lane_index + 1}.iperf.stdout.json").open("w")
    err = (case_dir / f"lane{lane_index + 1}.iperf.stderr.txt").open("w")
    return subprocess.Popen(
        [str(x) for x in args],
        cwd=str(ROOT),
        text=True,
        stdout=out,
        stderr=err,
    )


def read_proc_result(case_dir, lane_index, proc):
    try:
        rc = proc.wait(timeout=DURATION + 45)
    except subprocess.TimeoutExpired:
        proc.kill()
        rc = proc.wait(timeout=5)
    stdout_path = case_dir / f"lane{lane_index + 1}.iperf.stdout.json"
    stderr_path = case_dir / f"lane{lane_index + 1}.iperf.stderr.txt"
    stdout = stdout_path.read_text(errors="ignore") if stdout_path.exists() else ""
    stderr = stderr_path.read_text(errors="ignore") if stderr_path.exists() else ""
    return {
        "exit": rc,
        "gbps": bps_from_iperf(stdout) / 1e9,
        "stderr": stderr[-2000:],
    }


def run_case(name, reverse, lanes):
    case_dir = OUT / name
    case_dir.mkdir(parents=True, exist_ok=True)
    before = []
    for ports in lanes:
        before.append({
            "client": metrics(ports["client_admin"]),
            "server": metrics(ports["server_admin"], remote=True),
        })
    procs = [run_iperf_lane(case_dir, i, ports, reverse) for i, ports in enumerate(lanes)]
    results = [read_proc_result(case_dir, i, proc) for i, proc in enumerate(procs)]
    after = []
    for ports in lanes:
        after.append({
            "client": metrics(ports["client_admin"]),
            "server": metrics(ports["server_admin"], remote=True),
        })
    lane_rows = []
    for i, result in enumerate(results):
        lane_dir = case_dir / f"lane{i + 1}"
        lane_dir.mkdir(exist_ok=True)
        (lane_dir / "client.metrics.before.json").write_text(
            json.dumps(before[i]["client"], indent=2, sort_keys=True))
        (lane_dir / "client.metrics.after.json").write_text(
            json.dumps(after[i]["client"], indent=2, sort_keys=True))
        (lane_dir / "server.metrics.before.json").write_text(
            json.dumps(before[i]["server"], indent=2, sort_keys=True))
        (lane_dir / "server.metrics.after.json").write_text(
            json.dumps(after[i]["server"], indent=2, sort_keys=True))
        client_delta = diff(before[i]["client"], after[i]["client"])
        server_delta = diff(before[i]["server"], after[i]["server"])
        (lane_dir / "client.metrics.delta.json").write_text(
            json.dumps(client_delta, indent=2, sort_keys=True))
        (lane_dir / "server.metrics.delta.json").write_text(
            json.dumps(server_delta, indent=2, sort_keys=True))
        lane_rows.append({
            "lane": i + 1,
            "exit": result["exit"],
            "gbps": result["gbps"],
            "client_before": before[i]["client"],
            "server_before": before[i]["server"],
            "client_after": after[i]["client"],
            "server_after": after[i]["server"],
            "client_delta": client_delta,
            "server_delta": server_delta,
        })
    return {
        "case": name,
        "direction": "download" if reverse else "upload",
        "lanes": lane_rows,
        "total_gbps": sum(row["gbps"] for row in lane_rows),
    }


def metric_row(case, lane, side, data):
    calls = data.get("linux_relay_tcp_write_sendmsg_calls", 0)
    avg = data.get("linux_relay_tcp_write_bytes", 0) / calls / 1024 if calls else 0
    return (
        f"| {case} | lane{lane} | {side} | "
        f"{data.get('linux_relay_tcp_read_bytes', 0)/1024**3:.2f} | "
        f"{data.get('linux_relay_tcp_write_bytes', 0)/1024**3:.2f} | "
        f"{calls} | {avg:.1f} | "
        f"{data.get('linux_relay_tcp_write_eagain_count', 0)} | "
        f"{data.get('linux_relay_pending_bytes', 0)/1024**2:.1f} | "
        f"{data.get('linux_relay_max_relay_pending_quic_receive_bytes', 0)/1024**2:.1f} | "
        f"{data.get('linux_relay_quic_receive_paused_count', 0)} | "
        f"{data.get('linux_relay_quic_receive_resumed_count', 0)} | "
        f"{data.get('linux_relay_errors', 0)} |"
    )


def state_row(case, phase, lane, side, data):
    return (
        f"| {case} | {phase} | lane{lane} | {side} | "
        f"{data.get('active_streams', 0)} | "
        f"{data.get('accepted_connections', data.get('connection_count', 0))} | "
        f"{data.get('linux_relay_active_relays', 0)} | "
        f"{data.get('linux_relay_active_tcp_relays', 0)} | "
        f"{data.get('linux_relay_active_sink_relays', 0)} | "
        f"{data.get('linux_relay_active_quic_send_relays', 0)} | "
        f"{data.get('linux_relay_current_pending_quic_receive_bytes', 0)/1024**2:.1f} | "
        f"{data.get('linux_relay_current_pending_quic_receive_queue', 0)} | "
        f"{data.get('linux_relay_pending_bytes', 0)/1024**2:.1f} | "
        f"{data.get('linux_relay_worker_slots_allocated', 0)} | "
        f"{data.get('linux_relay_worker_slots_free', 0)} | "
        f"{data.get('linux_relay_tcp_read_armed_relays', 0)} | "
        f"{data.get('linux_relay_tcp_read_disabled_relays', 0)} | "
        f"{data.get('linux_relay_tcp_write_armed_relays', 0)} | "
        f"{data.get('linux_relay_closing_relays', 0)} | "
        f"{data.get('linux_relay_tcp_read_closed_relays', 0)} | "
        f"{data.get('linux_relay_tcp_write_shutdown_queued_relays', 0)} | "
        f"{data.get('linux_relay_outstanding_quic_sends', 0)} | "
        f"{data.get('linux_relay_pending_tcp_write_queue', 0)} | "
        f"{data.get('linux_relay_pending_tcp_write_bytes', 0)/1024**2:.1f} |"
    )


def summarize(rows):
    lines = [
        "# DGX dual tcpquic-proxy iperf probe",
        "",
        f"- Date: {datetime.now().isoformat(timespec='seconds')}",
        f"- Duration: {DURATION}s per direction",
        f"- Lanes: {LANES}",
        f"- Per lane: `--quic-connections {Q_PER_LANE}`, `iperf3 -P {P_PER_LANE}`",
        f"- TCP write max bytes: {TCP_WRITE_MAX_BYTES if TCP_WRITE_MAX_BYTES > 0 else 'uncapped'}",
        f"- TCP write burst bytes: {TCP_WRITE_BURST_BYTES if TCP_WRITE_BURST_BYTES > 0 else 'uncapped'}",
        f"- Output directory: `{OUT}`",
        "",
        "| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |",
        "|---|---|---:|---:|---:|---:|---:|",
    ]
    old = {"upload": 36.575, "download": 22.848}
    for row in rows:
        lane_gbps = [lane["gbps"] for lane in row["lanes"]]
        baseline = old[row["direction"]]
        lines.append(
            f"| {row['case']} | {row['direction']} | {lane_gbps[0]:.3f} | "
            f"{lane_gbps[1]:.3f} | {row['total_gbps']:.3f} | "
            f"{baseline:.3f} | {row['total_gbps'] - baseline:+.3f} |"
        )
    lines += [
        "",
        "## Metrics Delta",
        "",
        "| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |",
        "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        for lane in row["lanes"]:
            lines.append(metric_row(row["case"], lane["lane"], "client", lane["client_delta"]))
            lines.append(metric_row(row["case"], lane["lane"], "server", lane["server_delta"]))
    lines += [
        "",
        "## State Snapshots",
        "",
        "| Case | Phase | Lane | Side | active_streams | conn/accepted | relays | tcp_relays | sink_relays | quic_send_relays | pending_quic MiB | pending_quic_q | pending MiB | slots_alloc | slots_free | read_armed | read_disabled | write_armed | closing | read_closed | write_shutdown_q | out_quic_sends | pending_tcp_q | pending_tcp MiB |",
        "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        for lane in row["lanes"]:
            lines.append(state_row(row["case"], "before", lane["lane"], "client", lane["client_before"]))
            lines.append(state_row(row["case"], "before", lane["lane"], "server", lane["server_before"]))
            lines.append(state_row(row["case"], "after", lane["lane"], "client", lane["client_after"]))
            lines.append(state_row(row["case"], "after", lane["lane"], "server", lane["server_after"]))
    (OUT / "summary.md").write_text("\n".join(lines) + "\n")
    (OUT / "results.json").write_text(json.dumps(rows, indent=2, sort_keys=True))
    log(f"summary: {OUT / 'summary.md'}")


def main():
    for direction in DIRECTIONS:
        if direction not in ("upload", "download"):
            raise RuntimeError(f"invalid DIRECTIONS entry: {direction}")
    OUT.mkdir(parents=True, exist_ok=True)
    run(["rsync", "-az", str(BIN), f"{REMOTE}:{REMOTE_BIN}"])
    run(["rsync", "-az", str(CERT_SRC) + "/", f"{REMOTE}:/home/jack/tcpquic-dgx-certs/"])
    run(["sudo", "ip", "route", "replace", TARGET, "dev", IFACE, "src", BIND], check=False)
    ssh(f"sudo ip route replace {BIND} dev {IFACE} src {TARGET} 2>/dev/null; true", check=False)
    lanes = [start_lane(i) for i in range(LANES)]
    rows = []
    for direction in DIRECTIONS:
        if direction == "upload":
            rows.append(run_case("dual-proxy-upload-q8p8-plus-q8p8", False, lanes))
        elif direction == "download":
            rows.append(run_case("dual-proxy-download-q8p8-plus-q8p8", True, lanes))
    summarize(rows)


if __name__ == "__main__":
    try:
        main()
    finally:
        cleanup()
