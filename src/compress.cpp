#include "compress.h"

#include <algorithm>
#include <cstring>

#ifdef TCPQUIC_HAS_ZSTD
#include <zstd.h>
#endif

#ifdef TCPQUIC_HAS_LZ4
#include <lz4frame.h>
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

    void Reset() override {}
};

#ifdef TCPQUIC_HAS_ZSTD

class TqZstdCompressor final : public ITqCompressor {
public:
    explicit TqZstdCompressor(int level) {
        ctx_ = ZSTD_createCCtx();
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
        ctx_ = ZSTD_createDCtx();
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

    void Reset() override {
        if (ctx_ != nullptr) {
            ZSTD_DCtx_reset(ctx_, ZSTD_reset_session_only);
        }
    }

private:
    ZSTD_DCtx* ctx_{nullptr};
};

#endif // TCPQUIC_HAS_ZSTD

#ifdef TCPQUIC_HAS_LZ4

class TqLz4Compressor final : public ITqCompressor {
public:
    explicit TqLz4Compressor(int level) {
        prefs_ = LZ4F_preferences_t{};
        prefs_.compressionLevel = std::clamp(level, 0, 12);
        if (LZ4F_createCompressionContext(&ctx_, LZ4F_VERSION) != 0) {
            ctx_ = nullptr;
        }
    }

    ~TqLz4Compressor() override {
        if (ctx_ != nullptr) {
            LZ4F_freeCompressionContext(ctx_);
        }
    }

    TqLz4Compressor(const TqLz4Compressor&) = delete;
    TqLz4Compressor& operator=(const TqLz4Compressor&) = delete;

    bool Compress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, bool endStream) override {
        if (ctx_ == nullptr) {
            return false;
        }
        if (inLen > 0 && in == nullptr) {
            return false;
        }

        if (inLen > 0) {
            if (!started_) {
                const size_t bound = LZ4F_compressBound(0, &prefs_);
                std::vector<uint8_t> header(bound);
                const size_t headerSize = LZ4F_compressBegin(
                    ctx_, header.data(), header.size(), &prefs_);
                if (LZ4F_isError(headerSize)) {
                    return false;
                }
                out.insert(out.end(), header.begin(), header.begin() + headerSize);
                started_ = true;
            }

            const size_t bound = LZ4F_compressBound(inLen, &prefs_);
            std::vector<uint8_t> chunk(bound);
            const size_t written = LZ4F_compressUpdate(
                ctx_, chunk.data(), chunk.size(), in, inLen, nullptr);
            if (LZ4F_isError(written)) {
                return false;
            }
            if (written > 0) {
                out.insert(out.end(), chunk.begin(), chunk.begin() + written);
            }

            const size_t endBound = LZ4F_compressBound(0, &prefs_);
            std::vector<uint8_t> tail(endBound);
            const size_t endSize = LZ4F_compressEnd(ctx_, tail.data(), tail.size(), nullptr);
            if (LZ4F_isError(endSize)) {
                return false;
            }
            if (endSize > 0) {
                out.insert(out.end(), tail.begin(), tail.begin() + endSize);
            }
            started_ = false;
            return true;
        }

        if (endStream && started_) {
            const size_t bound = LZ4F_compressBound(0, &prefs_);
            std::vector<uint8_t> tail(bound);
            const size_t endSize = LZ4F_compressEnd(ctx_, tail.data(), tail.size(), nullptr);
            if (LZ4F_isError(endSize)) {
                return false;
            }
            if (endSize > 0) {
                out.insert(out.end(), tail.begin(), tail.begin() + endSize);
            }
            started_ = false;
        }

        return true;
    }

    bool Flush(std::vector<uint8_t>& out) override {
        if (ctx_ == nullptr || !started_) {
            return true;
        }
        const size_t bound = LZ4F_compressBound(0, &prefs_);
        if (bound == 0) {
            return true;
        }
        std::vector<uint8_t> chunk(bound);
        const size_t written = LZ4F_flush(ctx_, chunk.data(), chunk.size(), nullptr);
        if (LZ4F_isError(written)) {
            return false;
        }
        if (written > 0) {
            out.insert(out.end(), chunk.begin(), chunk.begin() + written);
        }
        return true;
    }

    void Reset() override {
        if (ctx_ != nullptr) {
            LZ4F_freeCompressionContext(ctx_);
            ctx_ = nullptr;
        }
        started_ = false;
        if (LZ4F_createCompressionContext(&ctx_, LZ4F_VERSION) != 0) {
            ctx_ = nullptr;
        }
    }

private:
    LZ4F_compressionContext_t ctx_{nullptr};
    LZ4F_preferences_t prefs_{};
    bool started_{false};
};

class TqLz4Decompressor final : public ITqDecompressor {
public:
    TqLz4Decompressor() {
        if (LZ4F_createDecompressionContext(&ctx_, LZ4F_VERSION) != 0) {
            ctx_ = nullptr;
        }
    }

    ~TqLz4Decompressor() override {
        if (ctx_ != nullptr) {
            LZ4F_freeDecompressionContext(ctx_);
        }
    }

    TqLz4Decompressor(const TqLz4Decompressor&) = delete;
    TqLz4Decompressor& operator=(const TqLz4Decompressor&) = delete;

    bool Decompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) override {
        if (ctx_ == nullptr) {
            return false;
        }
        if (inLen > 0 && in == nullptr) {
            return false;
        }
        if (inLen > 0) {
            pending_.insert(pending_.end(), in, in + inLen);
        }

        size_t srcPos = 0;
        while (srcPos < pending_.size()) {
            std::vector<uint8_t> scratch(256 * 1024);
            size_t dstSize = scratch.size();
            size_t srcSize = pending_.size() - srcPos;
            const size_t ret = LZ4F_decompress(
                ctx_,
                scratch.data(),
                &dstSize,
                pending_.data() + srcPos,
                &srcSize,
                nullptr);
            if (LZ4F_isError(ret)) {
                return false;
            }
            if (dstSize > 0) {
                out.insert(out.end(), scratch.begin(), scratch.begin() + dstSize);
            }
            if (srcSize == 0) {
                break;
            }
            srcPos += srcSize;
            if (ret == 0) {
                LZ4F_resetDecompressionContext(ctx_);
            }
        }

        if (srcPos > 0) {
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(srcPos));
        }
        return true;
    }

    void Reset() override {
        pending_.clear();
        if (ctx_ != nullptr) {
            LZ4F_freeDecompressionContext(ctx_);
            ctx_ = nullptr;
        }
        if (LZ4F_createDecompressionContext(&ctx_, LZ4F_VERSION) != 0) {
            ctx_ = nullptr;
        }
    }

private:
    LZ4F_decompressionContext_t ctx_{nullptr};
    std::vector<uint8_t> pending_;
};

#endif // TCPQUIC_HAS_LZ4

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
    case TqCompressAlgo::Lz4:
#ifdef TCPQUIC_HAS_LZ4
        return std::make_unique<TqLz4Compressor>(level);
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
    case TqCompressAlgo::Lz4:
#ifdef TCPQUIC_HAS_LZ4
        return std::make_unique<TqLz4Decompressor>();
#else
        return nullptr;
#endif
    }
    return nullptr;
}
