# QUIC MTU Probe - 2026-06-20

## Code Change

- Added `event=quic_mtu` on QUIC connected trace output.
- Added derived UDP payload sizes to QUIC metrics:
  - `udp_payload_ipv4 = mtu - 20 - 8`
  - `udp_payload_ipv6 = mtu - 40 - 8`

## Scenario 1: Local DGX Cluster Peer

- Peer: `169.254.59.196:4433`
- Client local address: `169.254.250.230:54224`
- Initial connected MTU: `1248`
- Stable probed MTU from later stats/final metrics: `1500`
- Derived UDP payload:
  - IPv4: `1472`
  - IPv6: `1452`
- Speed-test result: `19.324 Gbps`

Key trace lines:

```text
event=quic_mtu role=client conn=1 slot=1 local=169.254.250.230:54224 peer=169.254.59.196:4433 mtu=1248 udp_payload_ipv4=1220 udp_payload_ipv6=1200
congestion_events=0 cwnd=5888 mtu=1500 udp_payload_ipv4=1472 udp_payload_ipv6=1452
```

## Scenario 2: Remote Server

- Peer: `52.74.45.234:8443`
- Client local address: `172.16.10.80:55248`
- Initial connected MTU: `1248`
- Stable probed MTU from stats/final metrics: `1280`
- Derived UDP payload:
  - IPv4: `1252`
  - IPv6: `1232`

Notes:

- Temporary local test certificates were rejected before `quic_connected`; that run only reported initial final metrics and was not used as the actual remote MTU result.
- The remote result above used the repository default certificates under `cert/`.
- The QUIC connection was established, but the built-in speed-test ingress tunnel failed with `failed to read HTTP CONNECT response`; MTU stats were still collected from the established QUIC connection.

Key trace lines:

```text
event=quic_mtu role=client conn=1 slot=1 local=172.16.10.80:55248 peer=52.74.45.234:8443 mtu=1248 udp_payload_ipv4=1220 udp_payload_ipv6=1200
congestion_events=0 cwnd=2403513 mtu=1280 udp_payload_ipv4=1252 udp_payload_ipv6=1232
```
