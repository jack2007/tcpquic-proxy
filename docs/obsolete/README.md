# Obsolete Docs

This directory stores documents for approaches that were tested or analyzed but
are no longer part of the active direction.

Current decision:

- Pure or hybrid sync-write / inline callback write is not the current relay
  direction.
- The active relay direction is unified pending receive: all QUIC receive
  callbacks stay lightweight, return pending, and let relay workers handle TCP
  writes and zstd decompression.
