# DGX 10% Loss IO Throughput Validation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Validate tcpquic-proxy IO throughput for 12 network delay points with 10% loss on 2xDGX 200Gbps direct link, using one QUIC connection and one stream.

**Architecture:** Reuse the existing DGX download benchmark path from `scripts/bench-tcpquic-proxy-dgx.sh`: remote DGX runs HTTP source and tcpquic-proxy server, local DGX runs tcpquic-proxy client and one curl over HTTP CONNECT. Apply `tc netem` only on the remote egress interface toward the local DGX, with `limit 5000000`.

**Tech Stack:** Bash, `tc netem`, SSH, curl, tcpquic-proxy, existing DGX 200G direct-link addresses.

---

## Test Design

Primary measurement is HTTP CONNECT download throughput:

```text
remote HTTP source
  -> remote tcpquic-proxy server
  -> QUIC over 200Gbps direct link
  -> local tcpquic-proxy client
  -> local curl
```

This keeps the QUIC sender on the remote peer, matching the send-side egress netem method in `docs/io-throughput-diag_cn.md`.

Fixed test matrix:

```text
10ms  + 10% loss
20ms  + 10% loss
30ms  + 10% loss
40ms  + 10% loss
50ms  + 10% loss
60ms  + 10% loss
70ms  + 10% loss
80ms  + 10% loss
90ms  + 10% loss
150ms + 10% loss
300ms + 10% loss
500ms + 10% loss
```

Fixed runtime parameters:

```bash
MODES="tunnel_off"
NETEM=1
LOSS=10%
NETEM_LIMIT=5000000
RATE=
QUIC_CONNECTIONS=1
SIZE_MB=20000
DURATION_SEC=30
ITERATIONS=3
PAYLOAD_KIND=repeat
WARMUP_MB=0
METRIC_MODE=average
SKIP_APPEND=1
EXTRA_PROXY_ARGS="--diag-stats --diag-stats-interval 1"
```

Rationale:

- `MODES=tunnel_off` disables compression and isolates IO/tunnel throughput.
- `QUIC_CONNECTIONS=1` enforces one peer connection pool entry; one curl produces one HTTP CONNECT stream.
- `SIZE_MB=20000` and `DURATION_SEC=30` match the prior high-BDP validation window.
- `ITERATIONS=3` is the minimum useful repeat count because prior 100/200ms loss tests showed high run-to-run variance.
- `RATE=` is required because `scripts/bench-tcpquic-proxy-dgx.sh` defaults `NETEM=1` to `RATE=1gbit`; this validation must not rate-limit the 200Gbps direct link.
- `--diag-stats` is kept because the doc shows trace logs perturb throughput, while diag-stats gives low-overhead send/BBR/loss state.

## Output Layout

Create one timestamped result root:

```text
docs/dgx-mainline-download-10loss-delay-matrix-YYYYMMDD-HHMMSS/
```

Each delay gets its own directory:

```text
netem-010ms-10loss/
netem-020ms-10loss/
netem-030ms-10loss/
netem-040ms-10loss/
netem-050ms-10loss/
netem-060ms-10loss/
netem-070ms-10loss/
netem-080ms-10loss/
netem-090ms-10loss/
netem-150ms-10loss/
netem-300ms-10loss/
netem-500ms-10loss/
```

Required files per scenario:

```text
run.log
server.stderr
client.stderr
remote-qdisc-before.txt
remote-qdisc-after.txt
remote-qdisc-samples.txt
summary.env
```

Required final files:

```text
summary.csv
summary.md
```

`summary.csv` columns:

```text
delay_ms,loss,iterations,avg_mbps,min_mbps,max_mbps,run1_mbps,run2_mbps,run3_mbps,curl_rcs,qdisc_drop_delta,qdisc_backlog_max_bytes,qdisc_backlog_max_packets,server_srtt_avg_ms,server_srtt_max_ms,server_bbr_bw_avg_mbps,server_bytes_in_flight_max,server_lost_retransmittable_bytes_max,notes
```

## Tasks

### Task 1: Preflight State Capture

**Files:**
- Read: `docs/io-throughput-diag_cn.md`
- Read: `scripts/bench-tcpquic-proxy-dgx.sh`
- Read: `scripts/dgx-interconnect-netem-common.sh`
- Create: result root under `docs/`

- [ ] **Step 1: Verify local binary and source revision**

Run:

```bash
rtk git rev-parse HEAD
rtk git submodule status third_party/msquic
rtk test -x build/bin/Release/tcpquic-proxy
```

Expected: a commit hash, msquic submodule status, and `test` exit code 0.

- [ ] **Step 2: Verify peer reachability and route**

Run:

```bash
rtk ssh -o BatchMode=yes jack@169.254.59.196 'hostname; ip route get 169.254.250.230'
rtk ip route get 169.254.59.196
```

Expected: peer route uses the 200G direct-link interface, historically `enp1s0f0np0`.

- [ ] **Step 3: Clear stale processes and netem**

Run:

```bash
rtk ssh -o BatchMode=yes jack@169.254.59.196 'killall -9 tcpquic-proxy 2>/dev/null; fuser -k 16001/tcp 2>/dev/null; sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null; true'
rtk pkill -9 -f 'tcpquic-proxy client'
rtk sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null || true
```

Expected: commands complete; local interface has no netem.

### Task 2: Baseline Sanity Runs

**Files:**
- Create: `<result-root>/baseline-direct/run.log`
- Create: `<result-root>/baseline-proxy-no-netem/run.log`

- [ ] **Step 1: Run direct TCP baseline without netem**

Run:

```bash
rtk env MODES=direct NETEM=0 QUIC_CONNECTIONS=1 SIZE_MB=20000 DURATION_SEC=30 ITERATIONS=1 PAYLOAD_KIND=repeat WARMUP_MB=0 SKIP_APPEND=1 \
  ./scripts/bench-tcpquic-proxy-dgx.sh 2>&1 | tee <result-root>/baseline-direct/run.log
```

Expected: direct curl throughput is high enough to show the HTTP source and 200G path are not the bottleneck.

- [ ] **Step 2: Run proxy baseline without netem**

Run:

```bash
rtk env MODES=tunnel_off NETEM=0 QUIC_CONNECTIONS=1 SIZE_MB=20000 DURATION_SEC=30 ITERATIONS=1 PAYLOAD_KIND=repeat WARMUP_MB=0 SKIP_APPEND=1 \
  EXTRA_PROXY_ARGS="--diag-stats --diag-stats-interval 1" \
  ./scripts/bench-tcpquic-proxy-dgx.sh 2>&1 | tee <result-root>/baseline-proxy-no-netem/run.log
```

Expected: tcpquic-proxy starts, one curl stream runs, and logs contain diag-stats output.

### Task 3: Run 10% Loss Delay Matrix

**Files:**
- Create: `<result-root>/netem-<delay>ms-10loss/run.log`
- Create: `<result-root>/netem-<delay>ms-10loss/server.stderr`
- Create: `<result-root>/netem-<delay>ms-10loss/client.stderr`
- Create: `<result-root>/netem-<delay>ms-10loss/remote-qdisc-before.txt`
- Create: `<result-root>/netem-<delay>ms-10loss/remote-qdisc-after.txt`
- Create: `<result-root>/netem-<delay>ms-10loss/remote-qdisc-samples.txt`

- [ ] **Step 1: For each delay, run exactly 3 iterations**

Use this delay list:

```bash
DELAYS=(10 20 30 40 50 60 70 80 90 150 300 500)
```

For each `d`, run:

```bash
case_dir="<result-root>/netem-$(printf '%03d' "$d")ms-10loss"
rtk mkdir -p "$case_dir"

rtk ssh -o BatchMode=yes jack@169.254.59.196 "sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null; true"
rtk ssh -o BatchMode=yes jack@169.254.59.196 "tc -s qdisc show dev enp1s0f0np0" > "$case_dir/remote-qdisc-before.txt"

(
  while true; do
    date -Iseconds
    ssh -o BatchMode=yes jack@169.254.59.196 "tc -s qdisc show dev enp1s0f0np0" || true
    sleep 5
  done
) > "$case_dir/remote-qdisc-samples.txt" 2>&1 &
sampler_pid=$!

rtk env MODES=tunnel_off NETEM=1 DELAY="${d}ms" LOSS=10% NETEM_LIMIT=5000000 RATE= \
  QUIC_CONNECTIONS=1 SIZE_MB=20000 DURATION_SEC=30 ITERATIONS=3 PAYLOAD_KIND=repeat WARMUP_MB=0 SKIP_APPEND=1 \
  EXTRA_PROXY_ARGS="--diag-stats --diag-stats-interval 1" \
  ./scripts/bench-tcpquic-proxy-dgx.sh > "$case_dir/run.log" 2>&1
bench_rc=$?

kill "$sampler_pid" 2>/dev/null || true
wait "$sampler_pid" 2>/dev/null || true

rtk ssh -o BatchMode=yes jack@169.254.59.196 "tc -s qdisc show dev enp1s0f0np0" > "$case_dir/remote-qdisc-after.txt"
rtk scp -q jack@169.254.59.196:/tmp/tcpquic-dgx-server.log "$case_dir/server.stderr" 2>/dev/null || true
rtk cp /tmp/tcpquic-dgx-client.log "$case_dir/client.stderr" 2>/dev/null || true

exit "$bench_rc"
```

Expected:

- `run.log` contains `peer netem: delay <d>ms loss 10% limit 5000000`.
- `run.log` contains `mode=tunnel_off ... quic-connections=1`.
- The CSV line has 3 curl return codes.
- Netem is cleaned by the benchmark trap after each run; verify before moving to the next delay.

### Task 4: Parse and Summarize Results

**Files:**
- Create: `<result-root>/summary.csv`
- Create: `<result-root>/summary.md`

- [ ] **Step 1: Extract throughput and curl status**

Parse each `run.log` line matching:

```text
tunnel_off,<avg_bytes_per_sec>,<avg_mbps>,<curl_rcs>
```

Also parse individual lines:

```text
run N/3: <mbps> Mbps
run N/3: <mbps> Mbps (curl exit <rc>)
```

Expected: every delay has 3 run-level Mbps values and 3 curl statuses.

- [ ] **Step 2: Extract diag summaries**

From `server.stderr`, compute:

```text
srtt avg/max
bbr_bw_mbps avg
bytes_in_flight_max max
lost_retransmittable_bytes max
event_queue_full_errors max
flush_scheduling final
flush_pacing_delayed final
flush_cc_blocked final
```

Expected: high-delay cases still have diag samples; if a run fails before samples appear, mark `notes=no-server-diag`.

- [ ] **Step 3: Extract qdisc deltas**

From `remote-qdisc-before.txt`, `remote-qdisc-after.txt`, and `remote-qdisc-samples.txt`, compute:

```text
dropped_after - dropped_before
max backlog bytes
max backlog packets
```

Expected: qdisc output includes netem stats; if missing, mark `notes=qdisc-missing`.

- [ ] **Step 4: Generate final markdown table**

`summary.md` must include:

```markdown
| Delay | Loss | Avg Mbps | Min Mbps | Max Mbps | Curl RCs | qdisc drop delta | qdisc backlog max | srtt avg/max | bytes_in_flight_max | lost bytes max | Notes |
```

Expected: all 12 requested delay + loss combinations are present.

### Task 5: Validation Checks Before Reporting

**Files:**
- Read: `<result-root>/summary.csv`
- Read: `<result-root>/summary.md`

- [ ] **Step 1: Confirm matrix completeness**

Expected rows:

```text
10,20,30,40,50,60,70,80,90,150,300,500
```

Each row must have `loss=10%`, `iterations=3`, `NETEM_LIMIT=5000000`, and `QUIC_CONNECTIONS=1`.

- [ ] **Step 2: Confirm no accidental rate limit**

For every `run.log`, verify the netem command does not include `rate 1gbit`.

Expected:

```text
peer netem: delay <d>ms loss 10% limit 5000000
```

- [ ] **Step 3: Confirm local interface remained clean**

Run:

```bash
rtk tc qdisc show dev enp1s0f0np0
rtk ssh -o BatchMode=yes jack@169.254.59.196 "tc qdisc show dev enp1s0f0np0"
```

Expected: local side has no netem. Remote side has no netem after cleanup, unless a run is actively in progress.

## Reporting Rules

Final report should lead with the throughput matrix, then list caveats:

- Curl exit `28` means the 20GB payload did not finish within 30s; keep the measured `speed_download` but mark the run as timeout.
- Do not average timeout and success silently; show `curl_rcs` beside the average.
- Treat qdisc backlog as netem delay-queue backlog, not proof of physical NIC congestion.
- Compare with prior 5% data only as context; this run's required deliverable is the 10% loss matrix.
