#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

enum class TqCompressAlgo { None, Zstd };

struct TqDecompressResult {
    size_t InputConsumed{0};
    size_t OutputProduced{0};
    bool NeedsMoreInput{false};
    bool NeedsMoreOutput{false};
};

struct ITqCompressor {
    virtual ~ITqCompressor() = default;
    // When endStream is false, append compressed output for in using ZSTD_e_continue.
    // When endStream is true, finish the per-stream compression frame (ZSTD_e_end).
    virtual bool Compress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, bool endStream) = 0;
    // Emit buffered ZSTD output without ending the stream (ZSTD_e_flush).
    virtual bool Flush(std::vector<uint8_t>& out) = 0;
    virtual void Reset() = 0;
};

struct ITqDecompressor {
    virtual ~ITqDecompressor() = default;
    virtual bool Decompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) = 0;
    virtual bool DecompressInto(
        const uint8_t* input,
        size_t inputLength,
        uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) = 0;
    virtual void Reset() = 0;
};

std::unique_ptr<ITqCompressor> TqCreateCompressor(TqCompressAlgo algo, int level);
std::unique_ptr<ITqDecompressor> TqCreateDecompressor(TqCompressAlgo algo);
