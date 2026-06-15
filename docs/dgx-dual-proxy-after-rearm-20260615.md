# DGX dual tcpquic-proxy after re-arm fix

Date: 2026-06-15

## Purpose

Re-test the two-proxy-per-DGX scenario after the Linux relay TCP writable
re-arm fix.

Old dual baseline:

- `docs/dgx-dual-server-udp-socket-probe-20260615-144215/summary.md`
- two remote server processes, two local client processes
- each lane: `--quic-connections 8`, `iperf3 -P 8`

## Test Runs

New script:

```text
scripts/dgx-dual-proxy-iperf-probe.py
```

Runs:

```text
uncapped: docs/dgx-dual-proxy-iperf-probe-20260615-204910/
256 KiB: docs/dgx-dual-proxy-iperf-probe-20260615-205111/
```

Common parameters:

- duration: 30s per direction
- lanes: 2
- per lane: `--quic-connections 8`, `iperf3 -P 8`
- compression: off
- `--linux-relay-read-chunk-size 1048576`
- `--linux-relay-worker-slots 1024`

## Throughput

| Run | Direction | Lane 1 | Lane 2 | Total | Old dual baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| old baseline | upload | 18.330 Gbps | 18.246 Gbps | 36.575 Gbps | - | - |
| old baseline | download | 11.588 Gbps | 11.260 Gbps | 22.848 Gbps | - | - |
| current uncapped | upload | 18.603 Gbps | 18.107 Gbps | 36.710 Gbps | 36.575 Gbps | +0.135 Gbps |
| current uncapped | download | 12.587 Gbps | 11.719 Gbps | 24.306 Gbps | 22.848 Gbps | +1.458 Gbps |
| current 256 KiB cap | upload | 18.021 Gbps | 18.242 Gbps | 36.263 Gbps | 36.575 Gbps | -0.312 Gbps |
| current 256 KiB cap | download | 11.442 Gbps | 11.702 Gbps | 23.144 Gbps | 22.848 Gbps | +0.296 Gbps |

## Metrics Summary

Uncapped download:

| Lane | Client TCP write | Client EAGAIN | Historical max pending | Final pending | Final hot pending | Final hot write armed |
|---|---:|---:|---:|---:|---:|---|
| lane1 | 40.29 GiB | 22,181 | 503.1 MiB | 90.0 MiB | 0.0 MiB | false |
| lane2 | 37.15 GiB | 21,230 | 517.3 MiB | 41.0 MiB | 0.0 MiB | false |

256 KiB cap download:

| Lane | Client TCP write | Client EAGAIN | Historical max pending | Final pending | Final hot pending | Final hot write armed |
|---|---:|---:|---:|---:|---:|---|
| lane1 | 36.06 GiB | 22,837 | 540.2 MiB | 11.0 MiB | 0.0 MiB | false |
| lane2 | 36.97 GiB | 15,320 | 525.1 MiB | 111.0 MiB | 0.0 MiB | false |

Notes:

- Final hot pending is zero in both current download runs, so the previous
  "pending QUIC receive data with TCP write event disarmed" stall was not
  reproduced.
- Historical pending peaks are still about 500 MiB per lane.
- Client-side TCP write still sees tens of thousands of EAGAIN events.

## Interpretation

The re-arm fix improves dual-proxy download modestly in the uncapped run:

- download total increases from 22.848 Gbps to 24.306 Gbps;
- upload is effectively unchanged at about 36.6 Gbps.

This is consistent with the earlier single-stack result: the fix removes a
stall condition but does not remove the main throughput ceiling.

The 256 KiB TCP write cap did not help in this iperf dual-proxy run:

- upload drops slightly from 36.710 Gbps to 36.263 Gbps;
- download drops from 24.306 Gbps to 23.144 Gbps.

So the 256 KiB cap that helped the minimal TCP sink probe should not be treated
as a general improvement for iperf proxy traffic yet. The remaining download
bottleneck is still the local client QUIC receive to TCP write path, with high
loopback TCP EAGAIN and large transient pending receive queues.
