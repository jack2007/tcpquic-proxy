# DGX dual tcpquic-proxy iperf probe

- Date: 2026-06-15T22:51:42
- Duration: 30s per direction
- Lanes: 2
- Per lane: `--quic-connections 8`, `iperf3 -P 8`
- TCP write max bytes: uncapped
- TCP write burst bytes: uncapped
- Output directory: `/home/jack/src/tcpquic-proxy/docs/dgx-dual-proxy-iperf-probe-20260615-225100`

| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-proxy-download-q8p8-plus-q8p8 | download | 16.281 | 17.767 | 34.048 | 22.848 | +11.200 |

## Metrics Delta

| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | client | 0.00 | 52.80 | 398906 | 138.8 | 27608 | 0.0 | 0.0 | 6 | 6 | 750 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | server | 56.87 | 0.00 | 13 | 0.1 | 0 | 169.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | client | 0.00 | 57.86 | 447683 | 135.5 | 33655 | 0.0 | 0.0 | 7 | 7 | 15 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | server | 62.06 | 0.00 | 13 | 0.1 | 0 | 113.0 | 0.0 | 0 | 0 | 0 |
