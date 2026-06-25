# MsQuic secnetperf netem comparison

- Date: 2026-06-21
- Tool: MsQuic `secnetperf`
- Build: `build-msquic-perf/msquic/bin/Release/secnetperf`
- Direction: download
- Connection count: 1
- Test duration: `-down:30s`
- Netem location: peer egress `enp1s0f0np0`
- Netem loss: `5%`
- Netem limit: `5000000`

## Results

| Congestion control | Netem delay | Command options | Result |
| --- | ---: | --- | ---: |
| default cubic | 100ms | `-exec:maxtput -conns:1 -down:30s -ptput:1` | 1,450 kbps = 1.450 Mbps |
| default cubic | 200ms | `-exec:maxtput -conns:1 -down:30s -ptput:1` | 1,044 kbps = 1.044 Mbps |
| BBR | 100ms | `-exec:maxtput -cc:bbr -conns:1 -down:30s -ptput:1 -pconn:1` | 509,621 kbps = 509.621 Mbps |
| BBR | 200ms | `-exec:maxtput -cc:bbr -conns:1 -down:30s -ptput:1 -pconn:1` | 245,944 kbps = 245.944 Mbps |

## Notes

- `secnetperf` defaults to CUBIC. Under 5% random loss it drops to about 1 Mbps.
- With `-cc:bbr`, throughput at 200ms is about 48.3% of the 100ms result.
- Client-side `-pconn:1` confirms RTT:
  - 100ms case: RTT about 101.998 ms, MinRTT about 100.291 ms.
  - 200ms case: RTT about 201.581 ms, MinRTT about 200.605 ms.
- The client-side connection stats show `SendSuspectedLostPackets=0` because this is a download test and the client is primarily receiving data; sender-side loss counters are on the server side.

## Artifacts

- Default CUBIC logs: `docs/dgx-msquic-secnetperf-netem-100ms-200ms-5loss-limit5000000-20260621/`
- BBR logs: `docs/dgx-msquic-secnetperf-netem-100ms-200ms-5loss-limit5000000-bbr-20260621/`

## Cleanup

After the tests, peer qdisc was restored to `qdisc mq 0: root`.
