#include "compress.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> CompressZstdPayload(const std::vector<uint8_t>& payload) {
    auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
    if (compressor == nullptr) {
        std::abort();
    }

    std::vector<uint8_t> compressed;
    if (!compressor->Compress(payload.data(), payload.size(), compressed, true)) {
        std::abort();
    }
    return compressed;
}

int main() {
    std::string original;
    const std::string piece = "hello world repeated...";
    for (int i = 0; i < 100; ++i) {
        original += piece;
    }

    auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
    auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
    assert(compressor != nullptr);
    assert(decompressor != nullptr);

    std::vector<std::vector<uint8_t>> compressed_chunks;
    const size_t chunk_size = 64;
    for (size_t off = 0; off < original.size(); off += chunk_size) {
        const size_t len = std::min(chunk_size, original.size() - off);
        const bool flush = (off + len >= original.size());
        std::vector<uint8_t> chunk_out;
        assert(compressor->Compress(
            reinterpret_cast<const uint8_t*>(original.data() + off),
            len,
            chunk_out,
            flush));
        if (!chunk_out.empty()) {
            compressed_chunks.push_back(std::move(chunk_out));
        }
    }
    assert(!compressed_chunks.empty());

    // Multiple continue chunks must round-trip before the final end frame.
    auto streamCompressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
    auto streamDecompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
    assert(streamCompressor != nullptr);
    assert(streamDecompressor != nullptr);
    std::vector<uint8_t> streamCompressed;
    for (size_t off = 0; off < original.size(); off += chunk_size) {
        const size_t len = std::min(chunk_size, original.size() - off);
        const bool endStream = (off + len >= original.size());
        std::vector<uint8_t> chunk_out;
        assert(streamCompressor->Compress(
            reinterpret_cast<const uint8_t*>(original.data() + off),
            len,
            chunk_out,
            endStream));
        streamCompressed.insert(streamCompressed.end(), chunk_out.begin(), chunk_out.end());
    }
    std::vector<uint8_t> streamDecompressed;
    assert(streamDecompressor->Decompress(
        streamCompressed.data(), streamCompressed.size(), streamDecompressed));
    assert(streamDecompressed.size() == original.size());
    assert(std::memcmp(streamDecompressed.data(), original.data(), original.size()) == 0);

    auto relayZstdComp = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
    auto relayZstdDecomp = TqCreateDecompressor(TqCompressAlgo::Zstd);
    assert(relayZstdComp != nullptr);
    assert(relayZstdDecomp != nullptr);
    std::vector<uint8_t> relayZstdFrame;
    for (size_t off = 0; off < original.size(); off += chunk_size) {
        const size_t len = std::min(chunk_size, original.size() - off);
        std::vector<uint8_t> chunk_out;
        assert(relayZstdComp->Compress(
            reinterpret_cast<const uint8_t*>(original.data() + off),
            len,
            chunk_out,
            false));
        relayZstdFrame.insert(relayZstdFrame.end(), chunk_out.begin(), chunk_out.end());
    }
    {
        std::vector<uint8_t> end_out;
        assert(relayZstdComp->Compress(nullptr, 0, end_out, true));
        relayZstdFrame.insert(relayZstdFrame.end(), end_out.begin(), end_out.end());
    }
    std::vector<uint8_t> relayZstdOut;
    for (size_t off = 0; off < relayZstdFrame.size(); ) {
        const size_t take = std::min<size_t>(777, relayZstdFrame.size() - off);
        assert(relayZstdDecomp->Decompress(relayZstdFrame.data() + off, take, relayZstdOut));
        off += take;
    }
    assert(relayZstdOut.size() == original.size());
    assert(std::memcmp(relayZstdOut.data(), original.data(), original.size()) == 0);

    // 32 MiB zeros — relay-sized chunks with arbitrary QUIC-ish receive slices.
    {
        std::vector<uint8_t> zeros32(32u * 1024u * 1024u, 0);
        auto bigComp = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
        auto bigDecomp = TqCreateDecompressor(TqCompressAlgo::Zstd);
        assert(bigComp != nullptr);
        assert(bigDecomp != nullptr);
        std::vector<uint8_t> bigFrame;
        const size_t relayChunk = 256 * 1024;
        for (size_t off = 0; off < zeros32.size(); off += relayChunk) {
            const size_t len = std::min(relayChunk, zeros32.size() - off);
            std::vector<uint8_t> chunkOut;
            assert(bigComp->Compress(zeros32.data() + off, len, chunkOut, false));
            bigFrame.insert(bigFrame.end(), chunkOut.begin(), chunkOut.end());
        }
        {
            std::vector<uint8_t> endOut;
            assert(bigComp->Compress(nullptr, 0, endOut, true));
            bigFrame.insert(bigFrame.end(), endOut.begin(), endOut.end());
        }
        std::vector<uint8_t> bigOut;
        for (size_t off = 0; off < bigFrame.size();) {
            const size_t take = std::min<size_t>(1337, bigFrame.size() - off);
            assert(bigDecomp->Decompress(bigFrame.data() + off, take, bigOut));
            off += take;
        }
        assert(bigOut.size() == zeros32.size());
        assert(std::memcmp(bigOut.data(), zeros32.data(), zeros32.size()) == 0);
    }

    std::vector<uint8_t> decompressed;
    for (const auto& chunk : compressed_chunks) {
        assert(decompressor->Decompress(chunk.data(), chunk.size(), decompressed));
    }

    assert(decompressed.size() == original.size());
    assert(std::memcmp(decompressed.data(), original.data(), original.size()) == 0);

    {
        std::vector<uint8_t> payload(256 * 1024);
        uint32_t x = 0x12345678u;
        for (auto& byte : payload) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            byte = static_cast<uint8_t>(x & 0xffu);
        }
        std::vector<uint8_t> compressed = CompressZstdPayload(payload);
        auto boundedDecompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
        if (boundedDecompressor == nullptr) {
            return 10;
        }

        std::vector<uint8_t> output;
        size_t inputOffset = 0;
        size_t iterations = 0;
        while (output.size() < payload.size()) {
            uint8_t chunk[4096]{};
            TqDecompressResult result{};
            const uint8_t* input = inputOffset < compressed.size() ? compressed.data() + inputOffset : nullptr;
            const size_t inputLength = compressed.size() - inputOffset;
            if (!boundedDecompressor->DecompressInto(
                input,
                inputLength,
                chunk,
                sizeof(chunk),
                &result)) {
                return 11;
            }
            if (result.InputConsumed > inputLength) {
                return 12;
            }
            if (result.OutputProduced > sizeof(chunk)) {
                return 13;
            }
            if (result.InputConsumed == 0 && result.OutputProduced == 0) {
                return result.NeedsMoreInput ? 14 : 15;
            }
            inputOffset += result.InputConsumed;
            output.insert(output.end(), chunk, chunk + result.OutputProduced);
            ++iterations;
            if (iterations > 100000) {
                return 16;
            }
        }

        if (inputOffset != compressed.size()) {
            return 17;
        }
        if (output != payload) {
            return 18;
        }
    }

    // None passthrough sanity check.
    auto none_compressor = TqCreateCompressor(TqCompressAlgo::None, 0);
    auto none_decompressor = TqCreateDecompressor(TqCompressAlgo::None);
    assert(none_compressor != nullptr);
    assert(none_decompressor != nullptr);

    std::vector<uint8_t> none_out;
    assert(none_compressor->Compress(
        reinterpret_cast<const uint8_t*>(original.data()),
        original.size(),
        none_out,
        true));
    assert(none_out.size() == original.size());
    assert(std::memcmp(none_out.data(), original.data(), original.size()) == 0);

    return 0;
}
