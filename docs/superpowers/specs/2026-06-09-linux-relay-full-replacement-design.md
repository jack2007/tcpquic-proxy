# Linux Relay Full Replacement Design

## Decision

On Linux, the worker relay is the only production relay backend. The old blocking relay is retained only as demo/legacy comparison code and must not be linked into `tcpquic-proxy`.

## Production Data Path

- TCP to QUIC: worker-owned `epoll` readiness, `readv`, optional compression, multi-buffer `StreamSend`.
- QUIC to TCP: MsQuic callback copies receive data into worker-owned buffers, optional decompression, worker-owned `writev`.
- Shutdown: `TqRelayStop()` unregisters the worker relay and never deletes a blocking relay object.

## Compatibility

Compressed tunnels are supported in the worker path. Compression is inline on the owner worker in this replacement step.

## Legacy Demo

The old blocking relay is named `TqBlockingDemoRelay` and is available only through demo/test targets.
