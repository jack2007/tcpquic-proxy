# DGX dual tcpquic-proxy iperf probe

- Date: 2026-06-15T23:10:35
- Duration: 30s per direction
- Lanes: 2
- Per lane: `--quic-connections 8`, `iperf3 -P 8`
- TCP write max bytes: uncapped
- TCP write burst bytes: uncapped
- Output directory: `/home/jack/src/tcpquic-proxy/docs/dgx-dual-proxy-iperf-probe-20260615-230915`

| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-proxy-download-q8p8-plus-q8p8 | download | 17.981 | 17.406 | 35.387 | 22.848 | +12.539 |
| dual-proxy-upload-q8p8-plus-q8p8 | upload | 18.361 | 17.829 | 36.189 | 36.575 | -0.386 |

## Metrics Delta

| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | client | 0.00 | 58.78 | 432470 | 142.5 | 25229 | 0.0 | 0.0 | 0 | 0 | 415 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | server | 62.82 | 0.00 | 13 | 0.1 | 0 | 174.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | client | 0.00 | 56.72 | 447010 | 133.1 | 13271 | 0.0 | 0.0 | 0 | 0 | 1503 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | server | 60.79 | 0.00 | 13 | 0.1 | 0 | 165.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | client | 64.13 | 0.00 | 6 | 0.2 | 0 | 126.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | server | 0.00 | 63.58 | 586370 | 113.7 | 0 | -29.0 | 0.0 | 0 | 0 | 1003 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | client | 62.27 | 0.00 | 6 | 0.2 | 0 | 45.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | server | 0.00 | 61.90 | 544871 | 119.1 | 0 | -10.0 | 0.0 | 0 | 0 | 125 |
