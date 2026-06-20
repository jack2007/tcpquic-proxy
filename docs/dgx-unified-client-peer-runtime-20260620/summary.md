# Unified Client Peer Runtime DGX Smoke

- Date: 2026-06-20
- Binary: build-regression/bin/Release/tcpquic-proxy
- Scope: unified client peer runtime smoke after single-peer and multi-peer orchestration merge.

## Single-Peer 1x1

Command:

```bash
rtk env BIN=/home/jack/src/tcpquic-proxy/build-regression/bin/Release/tcpquic-proxy REPORT=/home/jack/src/tcpquic-proxy/docs/dgx-unified-client-peer-runtime-20260620/single-1x1.md PROXY_CASES='proxy-1x1:1:1:1' RUN_SECNETPERF=0 PROXY_START_WAIT_SEC=60 DURATION_SEC=10 scripts/dgx-msquic-vs-proxy-bench.sh
```

Result: PASS. Report: [single-1x1.md](single-1x1.md). Observed proxy 1x1 throughput: 20.559 Gbps.

## Multi-Peer Listener Gating

Command summary:

```bash
rtk timeout 20s build-regression/bin/Release/tcpquic-proxy client --client-config /tmp/tcpquic-unified-runtime/client.json --cert /tmp/tcpquic-unified-runtime/certs/client.crt --key /tmp/tcpquic-unified-runtime/certs/client.key --ca /tmp/tcpquic-unified-runtime/certs/ca.crt
rtk rg -n "peer peer-a HTTP CONNECT listening|peer peer-b HTTP CONNECT listening" docs/dgx-unified-client-peer-runtime-20260620/multi-peer-smoke.log
```

Result: PASS. The client timed out with rc=124 as expected because it stayed running, and both listener lines were present:

```text
9:tcpquic-proxy: peer peer-a HTTP CONNECT listening on 127.0.0.1:18181
13:tcpquic-proxy: peer peer-b HTTP CONNECT listening on 127.0.0.1:18182
```

Full log: [multi-peer-smoke.log](multi-peer-smoke.log).
