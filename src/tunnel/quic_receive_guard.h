#pragma once

#include <msquic.hpp>

#include <cstdint>

inline bool TqIsMsQuicFakeFinReceive(
    uint64_t absoluteOffset,
    uint64_t totalBufferLength,
    uint32_t bufferCount,
    QUIC_RECEIVE_FLAGS flags) {
    return (flags & QUIC_RECEIVE_FLAG_FIN) != 0 &&
           absoluteOffset == UINT64_MAX &&
           totalBufferLength == 0 &&
           bufferCount == 0;
}
