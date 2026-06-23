#include "relay_alloc.h"

#include <cstdint>
#include <cstring>

int main() {
    auto* bytes = static_cast<std::uint8_t*>(TqMalloc(16));
    if (bytes == nullptr) {
        return 1;
    }
    std::memset(bytes, 0x5a, 16);
    TqFree(bytes);

    auto* zeroed = static_cast<std::uint8_t*>(TqCalloc(8, sizeof(std::uint8_t)));
    if (zeroed == nullptr) {
        return 2;
    }
    for (size_t i = 0; i < 8; ++i) {
        if (zeroed[i] != 0) {
            TqFree(zeroed);
            return 3;
        }
    }

    auto* grown = static_cast<std::uint8_t*>(TqRealloc(zeroed, 32));
    if (grown == nullptr) {
        TqFree(zeroed);
        return 4;
    }
    for (size_t i = 0; i < 8; ++i) {
        if (grown[i] != 0) {
            TqFree(grown);
            return 5;
        }
    }
    std::memset(grown + 8, 0xa5, 24);
    TqFree(grown);

    TqFree(nullptr);
    return 0;
}
