# DGX dual tcpquic-proxy iperf probe

- Date: 2026-06-15T23:22:52
- Duration: 30s per direction
- Lanes: 2
- Per lane: `--quic-connections 8`, `iperf3 -P 8`
- TCP write max bytes: uncapped
- TCP write burst bytes: uncapped
- Output directory: `/home/jack/src/tcpquic-proxy/docs/dgx-dual-proxy-iperf-probe-20260615-232132`

| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | upload | 18.006 | 19.092 | 37.098 | 36.575 | +0.523 |
| dual-proxy-download-q8p8-plus-q8p8 | download | 12.073 | 11.205 | 23.278 | 22.848 | +0.430 |

## Metrics Delta

| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | client | 62.89 | 0.00 | 6 | 0.2 | 0 | 222.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | server | 0.00 | 62.63 | 610594 | 107.6 | 0 | 0.0 | 0.0 | 0 | 0 | 828 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | client | 66.68 | 0.00 | 6 | 0.2 | 0 | 98.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | server | 0.00 | 66.54 | 636370 | 109.6 | 0 | 0.0 | 0.0 | 0 | 0 | 1063 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | client | 0.00 | 38.28 | 285925 | 140.4 | 20012 | -30.0 | 0.0 | 0 | 0 | 403 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | server | 42.18 | 0.00 | 13 | 0.1 | 0 | 83.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | client | 0.00 | 35.33 | 250487 | 147.9 | 20170 | 9.0 | 0.0 | 0 | 0 | 215 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | server | 39.14 | 0.00 | 13 | 0.1 | 0 | 86.0 | 0.0 | 0 | 0 | 0 |

## State Snapshots

| Case | Phase | Lane | Side | active_streams | conn/accepted | relays | tcp_relays | sink_relays | quic_send_relays | pending_quic MiB | pending_quic_q | pending MiB | slots_alloc | slots_free | read_armed | read_disabled | write_armed |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane1 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane1 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane1 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 222.0 | 8192 | 7945 | 6 | 2 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane1 | server | 3 | 8 | 2 | 2 | 0 | 2 | 0.0 | 0 | 0.0 | 2048 | 2048 | 0 | 2 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane2 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane2 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane2 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 98.0 | 8192 | 8094 | 5 | 3 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane2 | server | 4 | 8 | 3 | 3 | 0 | 3 | 0.0 | 0 | 0.0 | 3072 | 3072 | 0 | 3 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane1 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 145.0 | 8192 | 8053 | 6 | 2 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane1 | server | 3 | 8 | 2 | 2 | 0 | 2 | 0.0 | 0 | 0.0 | 2048 | 2048 | 0 | 2 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane1 | client | 0 | 8 | 9 | 9 | 0 | 9 | 0.0 | 0 | 115.0 | 9216 | 9101 | 7 | 2 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane1 | server | 12 | 8 | 10 | 10 | 0 | 10 | 0.0 | 0 | 83.0 | 10240 | 10158 | 8 | 2 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane2 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 64.0 | 8192 | 8128 | 5 | 3 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane2 | server | 4 | 8 | 3 | 3 | 0 | 3 | 0.0 | 0 | 0.0 | 3072 | 3072 | 0 | 3 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane2 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 73.0 | 8192 | 8121 | 5 | 3 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane2 | server | 13 | 8 | 11 | 11 | 0 | 11 | 0.0 | 0 | 86.0 | 11264 | 11179 | 8 | 3 | 0 |
