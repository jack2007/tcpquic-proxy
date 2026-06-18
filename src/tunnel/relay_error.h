#pragma once

enum class TqRelayCloseMode {
    GracefulDrain,
    AbortReset,
};

enum class TqRelayFailureClass {
    Backpressure,
    Fatal,
};

inline const char* TqRelayCloseModeName(TqRelayCloseMode mode) {
    switch (mode) {
    case TqRelayCloseMode::GracefulDrain:
        return "graceful_drain";
    case TqRelayCloseMode::AbortReset:
        return "abort_reset";
    }
    return "unknown";
}
