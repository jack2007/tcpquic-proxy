# DGX dual tcpquic-proxy iperf probe

- Date: 2026-06-15T22:50:49
- Duration: 30s per direction
- Lanes: 2
- Per lane: `--quic-connections 8`, `iperf3 -P 8`
- TCP write max bytes: uncapped
- TCP write burst bytes: uncapped
- Output directory: `/home/jack/src/tcpquic-proxy/docs/dgx-dual-proxy-iperf-probe-20260615-225008`

| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | upload | 18.621 | 19.131 | 37.751 | 36.575 | +1.176 |

## Metrics Delta

| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | client | 65.04 | 0.00 | 5 | 0.3 | 0 | 21.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | server | 0.00 | 65.04 | 639440 | 106.7 | 0 | 0.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | client | 66.82 | 0.00 | 5 | 0.3 | 0 | 74.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | server | 0.00 | 66.80 | 652538 | 107.3 | 0 | 0.0 | 0.0 | 0 | 0 | 177 |
