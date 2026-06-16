# DGX QUIC 1-RTT encryption disable probe

Date: 2026-06-16

## Change

Added `--quic-disable-1rtt-encryption` for lab-only throughput experiments.

- Client sets `QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION` after `ConnectionOpen`
  and before `ConnectionStart`.
- Server sets the same connection param in `NEW_CONNECTION` before
  `SetConfiguration`.
- The option is intentionally explicit because it uses MsQuic's insecure
  1-RTT encryption disable feature and must not be used in production.

During testing, q16 startup exposed an existing client startup behavior:
`EnsureAnyConnected()` synchronously attempted to start every configured QUIC
slot before opening the local HTTP/SOCKS listeners. With
`--quic-disable-1rtt-encryption`, some `ConnectionStart` calls could take long
enough that the DGX script timed out waiting for the local listener. The client
startup path now returns once any connection is available, while the reconnect
thread continues to fill the rest of the connection pool in the background.

## Final 30s Results

Raw reports:

- Default encryption: `docs/dgx-disable-1rtt-encryption-20260616/default-final.md`
- Disable 1-RTT encryption: `docs/dgx-disable-1rtt-encryption-20260616/disable-1rtt-final.md`

| Case | Default | Disable 1-RTT encryption | Delta |
|---|---:|---:|---:|
| direct TCP curl | 33.694 Gbps | 37.111 Gbps | environment reference |
| secnetperf 1 conn | 16.396 Gbps | 19.616 Gbps | environment reference |
| secnetperf 16 conn | 74.534 Gbps | 82.772 Gbps | environment reference |
| secnetperf 1 conn / 16 streams | 14.389 Gbps | 17.693 Gbps | environment reference |
| tcpquic-proxy 1x1 | 16.862 Gbps | 21.310 Gbps | +26.4% |
| tcpquic-proxy 16x16 | 48.718 Gbps | 72.326 Gbps | +48.5% |
| tcpquic-proxy 4x16 | 55.040 Gbps | 82.853 Gbps | +50.5% |

Notes:

- `PROXY_EXTRA_ARGS=--quic-disable-1rtt-encryption` only affects
  `tcpquic-proxy`; secnetperf rows are same-run environment references, not
  encrypted-vs-unencrypted secnetperf A/B data.
- The 4x16 disabled-encryption run reached 82.853 Gbps, close to the same-run
  secnetperf 16-connection reference of 82.772 Gbps.
- The improvement strongly indicates that 1-RTT packet crypto is a material
  part of the current high-throughput CPU cost, especially once multiple QUIC
  connections/streams keep the relay pipeline full.

## Verification

Commands run:

```bash
cmake --build build --target tcpquic_tuning_test tcpquic-proxy -j$(nproc)
./build/bin/Release/tcpquic_tuning_test
REPORT=docs/dgx-disable-1rtt-encryption-20260616/default-final.md \
  DURATION_SEC=30 ./scripts/dgx-msquic-vs-proxy-bench.sh
REPORT=docs/dgx-disable-1rtt-encryption-20260616/disable-1rtt-final.md \
  DURATION_SEC=30 PROXY_EXTRA_ARGS='--quic-disable-1rtt-encryption' \
  ./scripts/dgx-msquic-vs-proxy-bench.sh
```
