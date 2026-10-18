#ifndef GLOBECORE_LAYER_TILE_CACHE_H
#define GLOBECORE_LAYER_TILE_CACHE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace globecore {

/**
 * 瓦片磁盘缓存：支持文件式（`<baseDir>/<z>/<x>_<y>.tile`）与 MBTiles/SQLite 两种后端。
 *
 * 当 [setBaseDir] 传入的路径以 `.mbtiles` 结尾时启用 SQLite 后端（标准 MBTiles 格式，
 * tiles 表 zoom_level/tile_column/tile_row/tile_data BLOB + metadata 表，另加非标扩展列
 * added_at 记录入库时间 Unix 秒，老库打开时自动 ALTER 补列、旧行为 NULL），单文件聚合
 * 海量小瓦片，B-tree 索引查找 O(log N) 优于文件系统路径拼接+fopen 的 O(1)+inode 开销，
 * 且便于离线地图包整包导入导出。否则保持文件系统后端（向后兼容已有缓存）。
 *
 * 行序为标准 XYZ（y 自北向南），与 [TileMatrix] 计算一致。MBTiles 内部存 TMS y（自下而上），
 * 读写时自动做 `tms_y = (2^z - 1) - xyz_y` 翻转，对外接口统一 XYZ。
 *
 * SQLite 后端并发安全：WAL 模式 + busy_timeout 5s + 内部 mutex 串行化 prepared statement。
 */
class TileCache {
public:
    TileCache() = default;
    explicit TileCache(std::string baseDir);
    ~TileCache();

    TileCache(const TileCache &) = delete;
    TileCache &operator=(const TileCache &) = delete;

    void setBaseDir(std::string baseDir);
    const std::string &baseDir() const { return baseDir_; }
    bool isReady() const { return !baseDir_.empty(); }
    bool useSqlite() const { return useSqlite_; }

    /**
     * 读取瓦片字节。命中返回 true 并填充 outBytes；未命中或读取失败返回 false。
     */
    bool readTile(int z, int x, int y, std::vector<uint8_t> &outBytes) const;

    /**
     * 写入瓦片字节（Phase C 联网下载后落盘）：文件后端自动创建 `<baseDir>/<z>/` 目录，
     * 先写 `.tmp` 再原子 rename（与原 FileTileStore 一致，避免读到半个文件）。
     * SQLite 后端用 INSERT OR REPLACE 写入 tiles 表。多线程对不同瓦片并发安全。
     * @return 成功返回 true；baseDir/bytes 为空或 IO 失败返回 false
     */
    bool writeTile(int z, int x, int y, const std::vector<uint8_t> &bytes) const;

    /**
     * 删除磁盘瓦片文件（坏瓦片自愈：读盘命中的字节解码失败时清除，防坏文件永久占位）。
     * 不存在或删除失败静默返回。
     */
    void removeTile(int z, int x, int y) const;

private:
    std::string baseDir_;
    bool useSqlite_ = false;
    void *db_ = nullptr;            // sqlite3*（void* 避免头文件泄露 sqlite3.h）
    mutable std::mutex dbMtx_;      // SQLite 读写串行化（prepared statement 非线程安全）

    bool openSqlite(const std::string &path);
    void closeSqlite();
    static int tmsY(int z, int xyzY); // MBTiles y 翻转：(2^z - 1) - xyzY
};

} // namespace globecore

#endif // GLOBECORE_LAYER_TILE_CACHE_H
