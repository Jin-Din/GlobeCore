#ifndef GLOBECORE_TILESET3D_TILESET3DLOADER_H
#define GLOBECORE_TILESET3D_TILESET3DLOADER_H

#include <string>

#include "layer/TilesetJson.h"
#include "tileset3d/B3dmDecoder.h"

namespace globecore {

/**
 * @Jin P0 3D Tiles 数据加载器：负责联网取 JSON/b3dm，并复用现有 JSON 与 b3dm 解码器。
 * @Jin 本类暂不做 LOD、缓存和 GL 上传，保证实验性能力可独立回退。
 */
class Tileset3DLoader {
public:
    struct TilesetLoadResult {
        bool ok = false;
        std::string error;
        std::string finalUrl;
        TilesetResult tileset;
    };

    // @Jin 下载并解析 tileset.json；外部 tileset 递归由后续 Tileset3DProvider 调度。
    static TilesetLoadResult loadTileset(const std::string &url, long timeoutMs = 20000);

    // @Jin 下载 b3dm 并剥离出内嵌 GLB/RTC_CENTER；调用方仍可通过 URL 自行做磁盘缓存。
    static B3dmContent loadB3dm(const std::string &url, long timeoutMs = 30000);
};

} // namespace globecore

#endif // GLOBECORE_TILESET3D_TILESET3DLOADER_H
