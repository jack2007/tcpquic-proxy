# DGX dual tcpquic-proxy iperf probe

- Date: 2026-06-15T23:38:56
- Duration: 30s per direction
- Lanes: 2
- Per lane: `--quic-connections 8`, `iperf3 -P 8`
- TCP write max bytes: uncapped
- TCP write burst bytes: uncapped
- Output directory: `/home/jack/src/tcpquic-proxy/docs/dgx-dual-proxy-iperf-probe-20260615-233737`

| Case | Direction | Lane 1 Gbps | Lane 2 Gbps | Total Gbps | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | upload | 18.287 | 18.126 | 36.413 | 36.575 | -0.162 |
| dual-proxy-download-q8p8-plus-q8p8 | download | 17.433 | 17.330 | 34.763 | 22.848 | +11.915 |

## Metrics Delta

| Case | Lane | Side | tcp_read GiB | tcp_write GiB | sendmsg | avg write KiB | EAGAIN | pending MiB | hot pending MiB | pause | resume | errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | client | 63.87 | 0.00 | 5 | 0.3 | 0 | 5.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane1 | server | 0.00 | 63.84 | 606855 | 110.3 | 0 | 0.0 | 0.0 | 0 | 0 | 256 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | client | 63.31 | 0.00 | 6 | 0.2 | 0 | 0.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-upload-q8p8-plus-q8p8 | lane2 | server | 0.00 | 63.10 | 606241 | 109.1 | 0 | 0.0 | 0.0 | 0 | 0 | 969 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | client | 0.00 | 56.81 | 435057 | 136.9 | 24466 | 0.0 | 0.0 | 2 | 2 | 1440 |
| dual-proxy-download-q8p8-plus-q8p8 | lane1 | server | 60.94 | 0.00 | 13 | 0.1 | 0 | 0.0 | 0.0 | 0 | 0 | 0 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | client | 0.00 | 56.94 | 420228 | 142.1 | 20497 | 0.0 | 0.0 | 0 | 0 | 8 |
| dual-proxy-download-q8p8-plus-q8p8 | lane2 | server | 60.66 | 0.00 | 13 | 0.1 | 0 | 0.0 | 0.0 | 0 | 0 | 0 |

## State Snapshots

| Case | Phase | Lane | Side | active_streams | conn/accepted | relays | tcp_relays | sink_relays | quic_send_relays | pending_quic MiB | pending_quic_q | pending MiB | slots_alloc | slots_free | read_armed | read_disabled | write_armed | closing | read_closed | write_shutdown_q | out_quic_sends | pending_tcp_q | pending_tcp MiB |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane1 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane1 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane1 | client | 0 | 8 | 9 | 9 | 0 | 9 | 0.0 | 0 | 5.0 | 9216 | 9211 | 4 | 5 | 0 | 0 | 9 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane1 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane2 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | before | lane2 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane2 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-upload-q8p8-plus-q8p8 | after | lane2 | server | 1 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane1 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane1 | server | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane1 | client | 0 | 8 | 2 | 2 | 0 | 2 | 0.0 | 0 | 0.0 | 2048 | 2048 | 0 | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane1 | server | 1 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane2 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | before | lane2 | server | 1 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane2 | client | 0 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| dual-proxy-download-q8p8-plus-q8p8 | after | lane2 | server | 2 | 8 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
