#include "tileset3d/Tileset3DLoader.h"

#include "net/HttpClient.h"
#include "util/Log.h"

namespace globecore {

Tileset3DLoader::TilesetLoadResult Tileset3DLoader::loadTileset(const std::string &url, long timeoutMs) {
    TilesetLoadResult result;
    result.finalUrl = url;
    HttpClient::globalInit();

    std::vector<uint8_t> bytes;
    long responseCode = 0;
    if (!HttpClient::get(url, bytes, timeoutMs, nullptr, {}, 64 * 1024 * 1024, &responseCode)) {
        result.error = "tileset.json 下载失败，HTTP=" + std::to_string(responseCode);
        LOGW("3D Tiles metadata failed: url=%s http=%ld", url.c_str(), responseCode);
        return result;
    }

    const std::string jsonText(bytes.begin(), bytes.end());
    result.tileset = parseTileset(jsonText);
    if (!result.tileset.ok) {
        result.error = "tileset.json 解析失败：" + result.tileset.error;
        return result;
    }
    result.ok = true;
    return result;
}

B3dmContent Tileset3DLoader::loadB3dm(const std::string &url, long timeoutMs) {
    HttpClient::globalInit();
    std::vector<uint8_t> bytes;
    long responseCode = 0;
    if (!HttpClient::get(url, bytes, timeoutMs, nullptr, {}, 128 * 1024 * 1024, &responseCode)) {
        B3dmContent failed;
        failed.error = "b3dm 下载失败，HTTP=" + std::to_string(responseCode);
        LOGW("3D Tiles b3dm failed: url=%s http=%ld", url.c_str(), responseCode);
        return failed;
    }
    return decodeB3dm(bytes);
}

} // namespace globecore
