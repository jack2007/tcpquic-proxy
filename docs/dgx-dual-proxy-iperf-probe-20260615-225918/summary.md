# DGX dual tcpquic-proxy iperf probe

- Date: 2026-06-15T23:00:39
- Duration: 30s per direction
- Lanes: 2
- Per lane: `--quic-connections 8`, `iperf3 -P 8`
- TCP write max bytes: uncapped
- TCP write burst bytes: uncapped
- Output directory: `/home/jack/src/tcpquic-proxy/docs/dgx-dual-proxy-iperf-probe-20260615-225918`

| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | upload | 19.126 | 19.248 | 38.375 | 36.575 | +1.800 |
| dual-proxy-download-q8p8-plus-q8p8 | download | 10.586 | 11.148 | 21.734 | 22.848 | -1.114 |

## Metrics Delta

| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | client | 66.80 | 0.00 | 6 | 0.2 | 0 | 80.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | server | 0.00 | 66.72 | 660799 | 105.9 | 0 | 0.0 | 0.0 | 0 | 0 | 727 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | client | 67.23 | 0.00 | 5 | 0.3 | 0 | 15.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | server | 0.00 | 67.23 | 661119 | 106.6 | 0 | 0.0 | 0.0 | 0 | 0 | 31 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | client | 0.00 | 33.25 | 252037 | 138.3 | 25037 | 6.0 | 0.0 | 1 | 1 | 390 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | server | 36.98 | 0.00 | 13 | 0.1 | 0 | 41.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | client | 0.00 | 35.29 | 264055 | 140.1 | 22057 | 5.0 | 0.0 | 1 | 1 | 107 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | server | 39.04 | 0.00 | 13 | 0.1 | 0 | 32.0 | 0.0 | 0 | 0 | 0 |
