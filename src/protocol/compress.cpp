#include "compress.h"

#include <algorithm>
#include <cstring>

#ifdef TCPQUIC_HAS_ZSTD
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "relay_alloc.h"
#endif

namespace {

class TqNoneCompressor final : public ITqCompressor {
public:
    bool Compress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, bool /*flush*/) override {
        if (inLen > 0) {
            if (in == nullptr) {
                return false;
            }
            out.insert(out.end(), in, in + inLen);
        }
        return true;
    }

    bool Flush(std::vector<uint8_t>& /*out*/) override {
        return true;
    }

    void Reset() override {}
};

class TqNoneDecompressor final : public ITqDecompressor {
public:
    bool Decompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) override {
        if (inLen > 0) {
            if (in == nullptr) {
                return false;
            }
            out.insert(out.end(), in, in + inLen);
        }
        return true;
    }

    bool DecompressInto(
        const uint8_t* input,
        size_t inputLength,
        uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        if (result == nullptr) {
            return false;
        }
        *result = TqDecompressResult{};
        if (inputLength > 0 && input == nullptr) {
            return false;
        }
        if (outputCapacity > 0 && output == nullptr) {
            return false;
        }
        const size_t n = std::min(inputLength, outputCapacity);
        if (n > 0) {
            std::memcpy(output, input, n);
        }
        result->InputConsumed = n;
        result->OutputProduced = n;
        result->NeedsMoreOutput = n < inputLength;
        result->NeedsMoreInput = n == inputLength;
        return true;
    }

    void Reset() override {}
};

#ifdef TCPQUIC_HAS_ZSTD

void* TqZstdAlloc(void* opaque, size_t size) {
    (void)opaque;
    return TqMalloc(size);
}

void TqZstdFree(void* opaque, void* address) {
    (void)opaque;
    TqFree(address);
}

ZSTD_customMem TqZstdCustomMem() {
    return ZSTD_customMem{TqZstdAlloc, TqZstdFree, nullptr};
}

class TqZstdCompressor final : public ITqCompressor {
public:
    explicit TqZstdCompressor(int level) {
        ctx_ = ZSTD_createCCtx_advanced(TqZstdCustomMem());
        if (ctx_ != nullptr) {
            const int clamped = std::clamp(level, 1, ZSTD_maxCLevel());
            ZSTD_CCtx_setParameter(ctx_, ZSTD_c_compressionLevel, clamped);
        }
    }

    ~TqZstdCompressor() override {
        if (ctx_ != nullptr) {
            ZSTD_freeCCtx(ctx_);
        }
    }

    TqZstdCompressor(const TqZstdCompressor&) = delete;
    TqZstdCompressor& operator=(const TqZstdCompressor&) = delete;

    bool Compress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, bool endStream) override {
        return CompressWithDirective(in, inLen, out, endStream ? ZSTD_e_end : ZSTD_e_continue);
    }

    bool Flush(std::vector<uint8_t>& out) override {
        return CompressWithDirective(nullptr, 0, out, ZSTD_e_flush);
    }

    void Reset() override {
        if (ctx_ != nullptr) {
            ZSTD_CCtx_reset(ctx_, ZSTD_reset_session_only);
        }
    }

private:
    bool CompressWithDirective(
        const uint8_t* in,
        size_t inLen,
        std::vector<uint8_t>& out,
        ZSTD_EndDirective directive) {
        if (ctx_ == nullptr) {
            return false;
        }
        if (inLen > 0 && in == nullptr) {
            return false;
        }

        ZSTD_inBuffer input = {in, inLen, 0};
        std::vector<uint8_t> scratch(ZSTD_CStreamOutSize());

        while (true) {
            ZSTD_outBuffer output = {scratch.data(), scratch.size(), 0};
            const size_t ret = ZSTD_compressStream2(ctx_, &output, &input, directive);
            if (ZSTD_isError(ret)) {
                return false;
            }

            if (output.pos > 0) {
                out.insert(out.end(), scratch.begin(), scratch.begin() + output.pos);
            }

            if (directive == ZSTD_e_continue) {
                if (input.pos == input.size) {
                    break;
                }
            } else if (ret == 0) {
                break;
            }
        }

        return true;
    }

    ZSTD_CCtx* ctx_{nullptr};
};

class TqZstdDecompressor final : public ITqDecompressor {
public:
    TqZstdDecompressor() {
        ctx_ = ZSTD_createDCtx_advanced(TqZstdCustomMem());
    }

    ~TqZstdDecompressor() override {
        if (ctx_ != nullptr) {
            ZSTD_freeDCtx(ctx_);
        }
    }

    TqZstdDecompressor(const TqZstdDecompressor&) = delete;
    TqZstdDecompressor& operator=(const TqZstdDecompressor&) = delete;

    bool Decompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) override {
        if (ctx_ == nullptr) {
            return false;
        }
        if (inLen > 0 && in == nullptr) {
            return false;
        }

        ZSTD_inBuffer input = {in, inLen, 0};
        std::vector<uint8_t> scratch(ZSTD_DStreamOutSize());

        while (input.pos < input.size) {
            ZSTD_outBuffer output = {scratch.data(), scratch.size(), 0};
            const size_t ret = ZSTD_decompressStream(ctx_, &output, &input);
            if (ZSTD_isError(ret)) {
                return false;
            }

            if (output.pos > 0) {
                out.insert(out.end(), scratch.begin(), scratch.begin() + output.pos);
            }
        }

        return true;
    }

    bool DecompressInto(
        const uint8_t* input,
        size_t inputLength,
        uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        if (result == nullptr || ctx_ == nullptr) {
            return false;
        }
        *result = TqDecompressResult{};
        if (inputLength > 0 && input == nullptr) {
            return false;
        }
        if (outputCapacity > 0 && output == nullptr) {
            return false;
        }
        ZSTD_inBuffer inBuffer = {input, inputLength, 0};
        ZSTD_outBuffer outBuffer = {output, outputCapacity, 0};
        const size_t ret = ZSTD_decompressStream(ctx_, &outBuffer, &inBuffer);
        if (ZSTD_isError(ret)) {
            return false;
        }
        result->InputConsumed = inBuffer.pos;
        result->OutputProduced = outBuffer.pos;
        result->NeedsMoreInput = (ret != 0 && inBuffer.pos == inBuffer.size);
        result->NeedsMoreOutput = (outBuffer.pos == outBuffer.size && ret != 0);
        return true;
    }

    void Reset() override {
        if (ctx_ != nullptr) {
            ZSTD_DCtx_reset(ctx_, ZSTD_reset_session_only);
        }
    }

private:
    ZSTD_DCtx* ctx_{nullptr};
};

#endif // TCPQUIC_HAS_ZSTD

} // namespace

std::unique_ptr<ITqCompressor> TqCreateCompressor(TqCompressAlgo algo, int level) {
    switch (algo) {
    case TqCompressAlgo::None:
        return std::make_unique<TqNoneCompressor>();
    case TqCompressAlgo::Zstd:
#ifdef TCPQUIC_HAS_ZSTD
        return std::make_unique<TqZstdCompressor>(level);
#else
        return nullptr;
#endif
    }
    return nullptr;
}

std::unique_ptr<ITqDecompressor> TqCreateDecompressor(TqCompressAlgo algo) {
    switch (algo) {
    case TqCompressAlgo::None:
        return std::make_unique<TqNoneDecompressor>();
    case TqCompressAlgo::Zstd:
#ifdef TCPQUIC_HAS_ZSTD
        return std::make_unique<TqZstdDecompressor>();
#else
        return nullptr;
#endif
    }
    return nullptr;
}
