# DGX dual tcpquic-proxy iperf probe

- Date: 2026-06-15T23:28:21
- Duration: 30s per direction
- Lanes: 2
- Per lane: `--quic-connections 8`, `iperf3 -P 8`
- TCP write max bytes: uncapped
- TCP write burst bytes: uncapped
- Output directory: `/home/jack/src/tcpquic-proxy/docs/dgx-dual-proxy-iperf-probe-20260615-232701`

| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | upload | 18.256 | 18.313 | 36.569 | 36.575 | -0.006 |
| dual-proxy-download-q8p8-plus-q8p8 | download | 12.023 | 11.426 | 23.448 | 22.848 | +0.600 |

## Metrics Delta

| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | client | 63.77 | 0.00 | 6 | 0.2 | 0 | 113.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | server | 0.00 | 63.54 | 618716 | 107.7 | 0 | 0.0 | 0.0 | 0 | 0 | 845 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | client | 63.96 | 0.00 | 6 | 0.2 | 0 | 38.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | server | 0.00 | 63.94 | 624549 | 107.4 | 0 | 0.0 | 0.0 | 0 | 0 | 194 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | client | 0.00 | 37.94 | 274670 | 144.8 | 27497 | 24.0 | 0.0 | 3 | 3 | 35 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | server | 42.00 | 0.00 | 13 | 0.1 | 0 | 116.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | client | 0.00 | 36.07 | 267119 | 141.6 | 24355 | -16.0 | 0.0 | 1 | 1 | 102 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | server | 39.97 | 0.00 | 13 | 0.1 | 0 | 94.0 | 0.0 | 0 | 0 | 0 |

## State Snapshots

| Case | Phase | Lane | Side | active_streams | conn/accepted | relays | tcp_relays | sink_relays | quic_send_relays | pending_quic MiB | pending_quic_q | pending MiB | slots_alloc | slots_free | read_armed | read_disabled | write_armed | closing | read_closed | write_shutdown_q | out_quic_sends | pending_tcp_q | pending_tcp MiB |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane1 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane1 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane1 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 113.0 | 8192 | 8078 | 6 | 2 | 0 | 0 | 8 | 0 | 2 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane1 | server | 3 | 8 | 2 | 2 | 0 | 2 | 0.0 | 0 | 0.0 | 2048 | 2048 | 0 | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane2 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane2 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane2 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 38.0 | 8192 | 8153 | 3 | 5 | 0 | 0 | 8 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane2 | server | 4 | 8 | 3 | 3 | 0 | 3 | 0.0 | 0 | 0.0 | 3072 | 3072 | 0 | 3 | 0 | 0 | 3 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane1 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 81.0 | 8192 | 8119 | 6 | 2 | 0 | 0 | 8 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane1 | server | 3 | 8 | 2 | 2 | 0 | 2 | 0.0 | 0 | 0.0 | 2048 | 2048 | 0 | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane1 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 105.0 | 8192 | 8087 | 6 | 2 | 0 | 0 | 8 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane1 | server | 12 | 8 | 10 | 10 | 0 | 10 | 0.0 | 0 | 116.0 | 10240 | 10129 | 8 | 2 | 0 | 0 | 10 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane2 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 48.0 | 8192 | 8146 | 3 | 5 | 0 | 0 | 8 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane2 | server | 4 | 8 | 3 | 3 | 0 | 3 | 0.0 | 0 | 0.0 | 3072 | 3072 | 0 | 3 | 0 | 0 | 3 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane2 | client | 0 | 8 | 8 | 8 | 0 | 8 | 0.0 | 0 | 32.0 | 8192 | 8157 | 3 | 5 | 0 | 0 | 8 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane2 | server | 13 | 8 | 11 | 11 | 0 | 11 | 0.0 | 0 | 94.0 | 11264 | 11172 | 8 | 3 | 0 | 0 | 11 | 0 | 0 | 0 | 0.0 |
