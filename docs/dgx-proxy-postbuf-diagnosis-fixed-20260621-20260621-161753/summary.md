# tcpquic-proxy postbuf diagnosis 1x1 DGX test

- netem: peer `enp1s0f0np0`, `limit 5000000`
- topology: 1 QUIC connection, 1 stream, 1 HTTP CONNECT download
- added trace fields: relay `outstanding_quic_send_bytes`, `ideal_send_threshold_bytes`, `quic_send_ops`, hot relay send/read state

| case | speed_download(B/s) | Mbps | Gbps | curl_exit |
|---|---:|---:|---:|---:|
| netem-100ms-5loss | 151764816 | 1214.12 | 1.214 | 0 |
| netem-200ms-5loss | 88307785 | 706.46 | 0.706 | 28 |

## Diagnosis

The sender is application-side limited by the in-flight send operation cap, not by lack of TCP source data.

- `tcp_read_bytes` keeps increasing, so the HTTP source path can provide data.
- `tcp_read_disabled_relays=1` for most samples while `outstanding_quic_sends=64`, showing TCP reads are paused because the relay reached the send operation count cap.
- `ideal_send_threshold_bytes` is 128MiB and later 184.7MiB in the 200ms case, but the relay often has far less than that posted while still disabled by `outstanding_quic_sends=64`.
- MsQuic `net_stats` shows `ideal` and BBR bandwidth estimates well above observed curl throughput, so the next parameter to test is increasing/removing `RelayMaxInFlightSends`, not increasing `ByteCount` alone.

## Key Samples

100ms + 5%:

- `outstanding_quic_sends=64`, `outstanding_quic_send_bytes=81854850`, `ideal_send_threshold_bytes=134217728`, `tcp_read_disabled_relays=1`
- Later: `outstanding_quic_sends=64`, `outstanding_quic_send_bytes=55788124`, `ideal_send_threshold_bytes=134217728`, `tcp_read_disabled_relays=1`

200ms + 5%:

- `outstanding_quic_sends=64`, `outstanding_quic_send_bytes=79431979`, `ideal_send_threshold_bytes=134217728`, `tcp_read_disabled_relays=1`
- Later: `outstanding_quic_sends=64`, `outstanding_quic_send_bytes=37645941`, `ideal_send_threshold_bytes=193710244`, `tcp_read_disabled_relays=1`

The 200ms client still hit `tcp_write_hard_error errno=32` after receiving about 3.53GB, so the receive-side close/error path remains a separate issue.
