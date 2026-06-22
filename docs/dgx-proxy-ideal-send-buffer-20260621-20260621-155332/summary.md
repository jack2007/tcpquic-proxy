# tcpquic-proxy ideal-send 1x1 DGX test

- binary: /home/jack/src/tcpquic-proxy/.worktrees/proxy-ideal-send-buffer-20260621/build-plan/bin/Release/tcpquic-proxy
- netem: peer enp1s0f0np0 delay 100/200ms loss 5% limit 5000000
- topology: 1 QUIC connection, 1 stream, 1 HTTP CONNECT download

| case | speed_download(B/s) | Mbps | Gbps | curl_exit |
|---|---:|---:|---:|---:|
| netem-100ms-5loss | 97059177 | 776.47 | 0.776 | 28 |
| netem-200ms-5loss | 51328831 | 410.63 | 0.411 | 28 |
