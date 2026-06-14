# DGX 2机 1x1 吞吐对比: off vs zstd

- Date: 2026-06-14
- Branch: `linux-unified-pending-zstd`
- Binary: `build/bin/Release/tcpquic-proxy`, deployed to `jack@169.254.59.196:~/tcpquic-dgx-bin/tcpquic-proxy`
- Topology: client `169.254.250.230` -> server `169.254.59.196`
- Test: external curl through HTTP CONNECT tunnel, 1 QUIC connection, 1 curl stream
- Payload: `PAYLOAD_KIND=text`, bumped by script to 4500 MB for `DURATION_SEC=30`
- Tuning: `TUNING_20GBPS=1`

## Final Comparison

| Mode | Runs | Average |
|------|------|---------|
| `tunnel_off` | 17.238, 14.258, 17.664 Gbps | 16.387 Gbps |
| `tunnel_zstd` | 49.976, 50.322, 49.531 Gbps | 49.943 Gbps |

zstd application-layer throughput is about 3.05x the uncompressed tunnel on this highly compressible text payload.

## Notes

An earlier combined run produced an intermittent zstd timeout:

| Mode | Runs | Average | curl exits |
|------|------|---------|------------|
| `tunnel_zstd` | 48.512, 0.081, 50.515 Gbps | 33.036 Gbps | 0,28,0 |

The follow-up zstd run with admin metrics enabled completed all three 30s-sized payload runs successfully, so the timeout was not reproduced in the final run. Because zstd completed very quickly, sampled admin metrics mostly captured the post-transfer healthy idle state rather than active relay counters.

## Logs

- Initial combined run: `bench.log`
- Final off rerun: `off-3x-rerun.log`
- Final zstd rerun: `zstd3-metrics-rerun/run.log`
- Intermittent zstd rerun: `zstd-3x-rerun.log`
