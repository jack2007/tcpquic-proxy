# DGX dual tcpquic-proxy iperf probe

- Date: 2026-06-15T23:24:46
- Duration: 30s per direction
- Lanes: 2
- Per lane: `--quic-connections 8`, `iperf3 -P 8`
- TCP write max bytes: uncapped
- TCP write burst bytes: uncapped
- Output directory: `/home/jack/src/tcpquic-proxy/docs/dgx-dual-proxy-iperf-probe-20260615-232326`

| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-proxy-download-q8p8-plus-q8p8 | download | 16.574 | 17.412 | 33.986 | 22.848 | +11.138 |
| dual-proxy-upload-q8p8-plus-q8p8 | upload | 18.781 | 17.716 | 36.497 | 36.575 | -0.078 |

## Metrics Delta

| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | client | 0.00 | 53.61 | 391362 | 143.6 | 31072 | 0.0 | 0.0 | 6 | 6 | 700 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | server | 57.89 | 0.00 | 13 | 0.1 | 0 | 114.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | client | 0.00 | 56.57 | 400935 | 147.9 | 31359 | 0.0 | 0.0 | 7 | 7 | 15 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | server | 60.82 | 0.00 | 13 | 0.1 | 0 | 140.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | client | 65.60 | 0.00 | 6 | 0.2 | 0 | 62.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | server | 0.00 | 65.56 | 587517 | 117.0 | 0 | 7.0 | 0.0 | 0 | 0 | 264 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | client | 61.88 | 0.00 | 6 | 0.2 | 0 | 13.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | server | 0.00 | 61.88 | 555459 | 116.8 | 0 | -7.0 | 0.0 | 0 | 0 | 0 |

## State Snapshots

| Case | Phase | Lane | Side | active_streams | conn/accepted | relays | tcp_relays | sink_relays | quic_send_relays | pending_quic MiB | pending_quic_q | pending MiB | slots_alloc | slots_free | read_armed | read_disabled | write_armed |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-download-q8p8-plus-q8p8 | before | lane1 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane1 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane1 | client | 0 | 8 | 1 | 1 | 0 | 1 | 0.0 | 0 | 0.0 | 1024 | 1024 | 0 | 1 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane1 | server | 9 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 114.0 | 8192 | 8075 | 8 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane2 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane2 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane2 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane2 | server | 9 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 140.0 | 8192 | 8059 | 8 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane1 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane1 | server | 9 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 111.0 | 8192 | 8061 | 8 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane1 | client | 0 | 8 | 9 | 9 | 0 | 9 | 0.0 | 0 | 62.0 | 9216 | 9154 | 4 | 5 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane1 | server | 15 | 8 | 13 | 13 | 0 | 13 | 0.0 | 0 | 118.0 | 13312 | 13192 | 8 | 5 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane2 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane2 | server | 9 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 154.0 | 8192 | 8050 | 8 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane2 | client | 0 | 8 | 9 | 9 | 0 | 9 | 0.0 | 0 | 13.0 | 9216 | 9202 | 1 | 8 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane2 | server | 18 | 8 | 16 | 16 | 0 | 16 | 0.0 | 0 | 147.0 | 16384 | 16237 | 8 | 8 | 0 |
