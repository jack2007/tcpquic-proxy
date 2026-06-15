# DGX dual tcpquic-proxy iperf probe

- Date: 2026-06-15T23:08:35
- Duration: 30s per direction
- Lanes: 2
- Per lane: `--quic-connections 8`, `iperf3 -P 8`
- TCP write max bytes: uncapped
- TCP write burst bytes: uncapped
- Output directory: `/home/jack/src/tcpquic-proxy/docs/dgx-dual-proxy-iperf-probe-20260615-230714`

| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | upload | 18.876 | 18.919 | 37.795 | 36.575 | +1.220 |
| dual-proxy-download-q8p8-plus-q8p8 | download | 11.413 | 11.357 | 22.770 | 22.848 | -0.078 |

## Metrics Delta

| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | client | 65.93 | 0.00 | 5 | 0.3 | 0 | 148.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | server | 0.00 | 65.83 | 643727 | 107.2 | 0 | 0.0 | 0.0 | 0 | 0 | 233 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | client | 66.08 | 0.00 | 5 | 0.3 | 0 | 100.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | server | 0.00 | 65.59 | 629992 | 109.2 | 0 | 0.0 | 0.0 | 0 | 0 | 1731 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | client | 0.00 | 36.11 | 268353 | 141.1 | 7691 | -21.0 | 0.0 | 0 | 0 | 217 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | server | 39.98 | 0.00 | 13 | 0.1 | 0 | 77.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | client | 0.00 | 35.94 | 265963 | 141.7 | 26587 | 57.0 | 0.0 | 2 | 2 | 9 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | server | 39.76 | 0.00 | 14 | 0.1 | 0 | 79.0 | 0.0 | 0 | 0 | 0 |
