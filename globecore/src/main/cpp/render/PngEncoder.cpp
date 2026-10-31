#include "render/PngEncoder.h"

#include <zlib.h>

#include <cstring>

namespace globecore {

namespace {

// 写一个 PNG 数据块（长度 + 类型 + 数据 + CRC32）。
void writeChunk(std::vector<uint8_t> &out, const char type[4], const uint8_t *data, size_t len) {
    auto pushBE32 = [&out](uint32_t v) {
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    pushBE32(static_cast<uint32_t>(len));
    const size_t crcStart = out.size();
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(type), reinterpret_cast<const uint8_t *>(type) + 4);
    if (len > 0 && data != nullptr) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + crcStart, static_cast<uInt>(4 + len));
    pushBE32(static_cast<uint32_t>(crc));
}

} // namespace

// @Jin 交错 RGBA → 带逐行 filter(0) 的原始流 → zlib compress2 → PNG（IHDR/IDAT/IEND）。
bool PngEncoder::encodeRGBA(int width, int height, const uint8_t *rgba, std::vector<uint8_t> &outPng) {
    if (width <= 0 || height <= 0 || rgba == nullptr) return false;
    const size_t stride = static_cast<size_t>(width) * 4u;
    const size_t rawLen = static_cast<size_t>(height) * (stride + 1u);

    std::vector<uint8_t> raw;
    raw.resize(rawLen);
    for (int y = 0; y < height; ++y) {
        const size_t row = static_cast<size_t>(y) * (stride + 1u);
        raw[row] = 0; // filter type None
        std::memcpy(raw.data() + row + 1u, rgba + static_cast<size_t>(y) * stride, stride);
    }

    uLongf compressedBound = compressBound(static_cast<uLong>(rawLen));
    std::vector<uint8_t> compressed(compressedBound);
    if (compress2(compressed.data(), &compressedBound, raw.data(), static_cast<uLong>(rawLen),
                  Z_BEST_SPEED) != Z_OK) {
        return false;
    }
    compressed.resize(compressedBound);

    static const uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    outPng.clear();
    outPng.insert(outPng.end(), kPngSignature, kPngSignature + 8);

    uint8_t ihdr[13];
    auto putBE32 = [](uint8_t *p, uint32_t v) {
        p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[3] = static_cast<uint8_t>(v & 0xFF);
    };
    putBE32(ihdr, static_cast<uint32_t>(width));
    putBE32(ihdr + 4, static_cast<uint32_t>(height));
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // color type RGBA
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    writeChunk(outPng, "IHDR", ihdr, sizeof(ihdr));
    writeChunk(outPng, "IDAT", compressed.data(), compressed.size());
    writeChunk(outPng, "IEND", nullptr, 0);
    return true;
}

} // namespace globecore