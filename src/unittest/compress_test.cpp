#include "compress.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

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

#ifdef TCPQUIC_HAS_LZ4
    auto lz4_compressor = TqCreateCompressor(TqCompressAlgo::Lz4, 1);
    auto lz4_decompressor = TqCreateDecompressor(TqCompressAlgo::Lz4);
    assert(lz4_compressor != nullptr);
    assert(lz4_decompressor != nullptr);
    std::vector<uint8_t> lz4Compressed;
    for (size_t off = 0; off < original.size(); off += chunk_size) {
        const size_t len = std::min(chunk_size, original.size() - off);
        const bool endStream = (off + len >= original.size());
        std::vector<uint8_t> chunk_out;
        assert(lz4_compressor->Compress(
            reinterpret_cast<const uint8_t*>(original.data() + off),
            len,
            chunk_out,
            endStream));
        lz4Compressed.insert(lz4Compressed.end(), chunk_out.begin(), chunk_out.end());
    }
    std::vector<uint8_t> lz4Decompressed;
    for (size_t off = 0; off < lz4Compressed.size(); off += chunk_size) {
        const size_t len = std::min(chunk_size, lz4Compressed.size() - off);
        assert(lz4_decompressor->Decompress(
            lz4Compressed.data() + off, len, lz4Decompressed));
    }
    assert(lz4Decompressed.size() == original.size());
    assert(std::memcmp(lz4Decompressed.data(), original.data(), original.size()) == 0);

    // Tiny payload + byte-at-a-time decompress (simulates QUIC chunking).
    const char tiny[] = "ok\n";
    auto tinyComp = TqCreateCompressor(TqCompressAlgo::Lz4, 1);
    auto tinyDecomp = TqCreateDecompressor(TqCompressAlgo::Lz4);
    std::vector<uint8_t> tinyFrame;
    assert(tinyComp->Compress(reinterpret_cast<const uint8_t*>(tiny), sizeof(tiny) - 1, tinyFrame, true));
    assert(!tinyFrame.empty());
    std::vector<uint8_t> tinyOut;
    for (size_t i = 0; i < tinyFrame.size(); ++i) {
        assert(tinyDecomp->Decompress(&tinyFrame[i], 1, tinyOut));
    }
    assert(tinyOut.size() == sizeof(tiny) - 1);
    assert(std::memcmp(tinyOut.data(), tiny, sizeof(tiny) - 1) == 0);

    // Relay-style streaming: many updates with endStream=false, then explicit end.
    auto relayComp = TqCreateCompressor(TqCompressAlgo::Lz4, 1);
    auto relayDecomp = TqCreateDecompressor(TqCompressAlgo::Lz4);
    std::vector<uint8_t> relayFrame;
    const size_t relayChunk = 16 * 1024;
    for (size_t off = 0; off < original.size(); off += relayChunk) {
        const size_t len = std::min(relayChunk, original.size() - off);
        std::vector<uint8_t> chunkOut;
        assert(relayComp->Compress(
            reinterpret_cast<const uint8_t*>(original.data() + off),
            len,
            chunkOut,
            false));
        relayFrame.insert(relayFrame.end(), chunkOut.begin(), chunkOut.end());
    }
    {
        std::vector<uint8_t> endOut;
        assert(relayComp->Compress(nullptr, 0, endOut, true));
        relayFrame.insert(relayFrame.end(), endOut.begin(), endOut.end());
    }
    std::vector<uint8_t> relayOut;
    for (size_t off = 0; off < relayFrame.size(); ) {
        const size_t take = std::min<size_t>(777, relayFrame.size() - off);
        assert(relayDecomp->Decompress(relayFrame.data() + off, take, relayOut));
        off += take;
    }
    assert(relayOut.size() == original.size());
    assert(std::memcmp(relayOut.data(), original.data(), original.size()) == 0);

    std::string largePayload;
    largePayload.reserve(4096 * 13 + 1);
    for (int i = 0; i < 4096; ++i) {
        largePayload += "compress-me-";
    }
    largePayload += '\n';
    auto largeComp = TqCreateCompressor(TqCompressAlgo::Lz4, 1);
    auto largeDecomp = TqCreateDecompressor(TqCompressAlgo::Lz4);
    std::vector<uint8_t> largeFrame;
    for (size_t off = 0; off < largePayload.size(); off += relayChunk) {
        const size_t len = std::min(relayChunk, largePayload.size() - off);
        const bool end = (off + len >= largePayload.size());
        std::vector<uint8_t> chunkOut;
        assert(largeComp->Compress(
            reinterpret_cast<const uint8_t*>(largePayload.data() + off),
            len,
            chunkOut,
            end));
        largeFrame.insert(largeFrame.end(), chunkOut.begin(), chunkOut.end());
    }
    std::vector<uint8_t> largeOut;
    for (size_t off = 0; off < largeFrame.size(); ) {
        const size_t take = std::min<size_t>(777, largeFrame.size() - off);
        assert(largeDecomp->Decompress(largeFrame.data() + off, take, largeOut));
        off += take;
    }
    assert(largeOut.size() == largePayload.size());
    assert(largeOut == largePayload);

    // Smoke-sized payload: one TCP read + optional flush + explicit end (relay EOF).
    std::string smokePayload;
    smokePayload.reserve(4096 * 13 + 1);
    for (int i = 0; i < 4096; ++i) {
        smokePayload += "compress-me-";
    }
    smokePayload += '\n';
    auto smokeComp = TqCreateCompressor(TqCompressAlgo::Lz4, 1);
    auto smokeDecomp = TqCreateDecompressor(TqCompressAlgo::Lz4);
    std::vector<uint8_t> smokeFrame;
    std::vector<uint8_t> smokeChunk;
    assert(smokeComp->Compress(
        reinterpret_cast<const uint8_t*>(smokePayload.data()),
        smokePayload.size(),
        smokeChunk,
        false));
    if (smokeChunk.empty()) {
        assert(smokeComp->Flush(smokeChunk));
    }
    smokeFrame.insert(smokeFrame.end(), smokeChunk.begin(), smokeChunk.end());
    smokeChunk.clear();
    assert(smokeComp->Compress(nullptr, 0, smokeChunk, true));
    smokeFrame.insert(smokeFrame.end(), smokeChunk.begin(), smokeChunk.end());
    std::vector<uint8_t> smokeOut;
    for (size_t off = 0; off < smokeFrame.size(); ) {
        const size_t take = std::min<size_t>(512, smokeFrame.size() - off);
        assert(smokeDecomp->Decompress(smokeFrame.data() + off, take, smokeOut));
        off += take;
    }
    assert(smokeOut == smokePayload);

    // 32 MiB zeros — per-chunk LZ4 frames (current relay compressor behavior).
    {
        std::vector<uint8_t> zeros32(32u * 1024u * 1024u, 0);
        auto bigComp = TqCreateCompressor(TqCompressAlgo::Lz4, 1);
        auto bigDecomp = TqCreateDecompressor(TqCompressAlgo::Lz4);
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
#else
    assert(TqCreateCompressor(TqCompressAlgo::Lz4, 1) == nullptr);
    assert(TqCreateDecompressor(TqCompressAlgo::Lz4) == nullptr);
#endif

    return 0;
}
