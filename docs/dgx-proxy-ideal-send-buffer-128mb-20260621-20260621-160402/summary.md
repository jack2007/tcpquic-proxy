# tcpquic-proxy ideal-send 128MiB initial 1x1 DGX test

- binary: /home/jack/src/tcpquic-proxy/.worktrees/proxy-ideal-send-buffer-20260621/build-plan/bin/Release/tcpquic-proxy
- netem: peer enp1s0f0np0 delay 100/200ms loss 5% limit 5000000
- topology: 1 QUIC connection, 1 stream, 1 HTTP CONNECT download
- initial/fallback ByteCount floor: 128MiB

| case | speed_download(B/s) | Mbps | Gbps | curl_exit |
|---|---:|---:|---:|---:|
| netem-100ms-5loss | 169952580 | 1359.62 | 1.360 | 0 |
| netem-200ms-5loss | 85732152 | 685.86 | 0.686 | 28 |
