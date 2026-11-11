#ifndef GLOBECORE_TILESET3D_TILESET3DURL_H
#define GLOBECORE_TILESET3D_TILESET3DURL_H

#include <string>

namespace globecore {

/**
 * @Jin 3D Tiles 相对资源 URL 工具。
 * @Jin tileset.json 的 content.uri 可能是 ./top/a.b3dm、../x.json 或同级 Tile 路径，需要按当前 JSON 地址解析。
 */
class Tileset3DUrl {
public:
    // @Jin 按 RFC 3986 的基本规则解析相对路径；支持 http/https、绝对路径、./ 和 ../。
    static std::string resolve(const std::string &baseUrl, const std::string &reference);

    // @Jin 判断外部 tileset JSON；测试数据中根 tileset 会递归引用 0/root.json。
    static bool isTilesetJson(const std::string &url);

    // @Jin 判断 b3dm 内容；后续 i3dm/pnts/cmpt 再扩展独立解码器。
    static bool isB3dm(const std::string &url);
};

} // namespace globecore

#endif // GLOBECORE_TILESET3D_TILESET3DURL_H
