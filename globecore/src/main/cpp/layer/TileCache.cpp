#include "layer/TileCache.h"

#include "util/Log.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <utility>

#include "sqlite/sqlite3.h"

namespace globecore {

namespace {
/// 逐级创建目录（POSIX mkdir 单级；baseDir 可为多层深如 tiles/raster-jni/<hash>，
/// 单层 mkdir 因父目录缺失 ENOENT 静默失败 → 瓦片永不落盘、每次重生成）
void MkdirRecursive(const std::string &dir) {
    for (size_t pos = dir.find('/', 1); pos != std::string::npos; pos = dir.find('/', pos + 1)) {
        mkdir(dir.substr(0, pos).c_str(), 0777); // 已存在 EEXIST 忽略即可
    }
    mkdir(dir.c_str(), 0777);
}

/// 判断路径是否以 .mbtiles 结尾（大小写不敏感）
bool endsWithMbtiles(const std::string &path) {
    if (path.size() < 9) return false;
    std::string ext = path.substr(path.size() - 8);
    for (char &c : ext) { if (c >= 'A' && c <= 'Z') c += 32; }
    return ext == ".mbtiles";
}
} // namespace

TileCache::TileCache(std::string baseDir) {
    setBaseDir(std::move(baseDir));
}

TileCache::~TileCache() {
    closeSqlite();
}

void TileCache::setBaseDir(std::string baseDir) {
    closeSqlite();
    baseDir_ = std::move(baseDir);
    if (!baseDir_.empty() && endsWithMbtiles(baseDir_)) {
        useSqlite_ = openSqlite(baseDir_);
    }
    LOGI("TileCache baseDir=%s sqlite=%d", baseDir_.c_str(), useSqlite_ ? 1 : 0);
}

// ── SQLite MBTiles 后端 ──

bool TileCache::openSqlite(const std::string &path) {
    sqlite3 *db = nullptr;
    // 建父目录（mbtiles 文件可能位于尚不存在的目录）
    auto slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        MkdirRecursive(path.substr(0, slash));
    }
    int rc = sqlite3_open_v2(path.c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        LOGW("TileCache sqlite_open failed rc=%d msg=%s", rc, db ? sqlite3_errmsg(db) : "null");
        if (db) sqlite3_close(db);
        return false;
    }
    // WAL 模式：读不阻塞写、写不阻塞读，并发取瓦片不互斥
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    sqlite3_busy_timeout(db, 5000); // 遇锁忙等 5s
    // 建标准 MBTiles 表结构（幂等）
    const char *kCreateTiles =
        "CREATE TABLE IF NOT EXISTS tiles ("
        "  zoom_level INTEGER,"
        "  tile_column INTEGER,"
        "  tile_row INTEGER,"
        "  tile_data BLOB,"
        "  PRIMARY KEY (zoom_level, tile_column, tile_row))";
    const char *kCreateMetadata =
        "CREATE TABLE IF NOT EXISTS metadata (name TEXT, value TEXT)";
    const char *kCreateIdx =
        "CREATE INDEX IF NOT EXISTS idx_tiles ON tiles (zoom_level, tile_column, tile_row)";
    char *err = nullptr;
    if (sqlite3_exec(db, kCreateTiles, nullptr, nullptr, &err) != SQLITE_OK) {
        LOGW("TileCache sqlite create tiles failed: %s", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(db);
        return false;
    }
    sqlite3_exec(db, kCreateMetadata, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    sqlite3_exec(db, kCreateIdx, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    db_ = db;
    return true;
}

void TileCache::closeSqlite() {
    if (db_) {
        sqlite3_close(static_cast<sqlite3 *>(db_));
        db_ = nullptr;
    }
    useSqlite_ = false;
}

int TileCache::tmsY(int z, int xyzY) {
    // (2^z - 1) - xyzY：XYZ→TMS 行翻转
    int rows = (1 << z); // z<=30 时不溢出 int
    return rows - 1 - xyzY;
}

// ── 读写分发（SQLite vs 文件系统）──

bool TileCache::readTile(int z, int x, int y, std::vector<uint8_t> &outBytes) const {
    if (baseDir_.empty()) return false;

    if (useSqlite_ && db_) {
        std::lock_guard<std::mutex> lk(dbMtx_);
        sqlite3 *db = static_cast<sqlite3 *>(db_);
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            LOGW("TileCache sqlite prepare(read) failed: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_int(stmt, 1, z);
        sqlite3_bind_int(stmt, 2, x);
        sqlite3_bind_int(stmt, 3, tmsY(z, y));
        bool hit = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const void *blob = sqlite3_column_blob(stmt, 0);
            int n = sqlite3_column_bytes(stmt, 0);
            if (blob && n > 0) {
                outBytes.resize(static_cast<size_t>(n));
                std::memcpy(outBytes.data(), blob, static_cast<size_t>(n));
                hit = true;
            }
        }
        sqlite3_finalize(stmt);
        return hit;
    }

    // 文件系统后端
    char path[2048];
    const int written = snprintf(path, sizeof(path), "%s/%d/%d_%d.tile", baseDir_.c_str(), z, x, y);
    if (written <= 0 || written >= static_cast<int>(sizeof(path))) return false;

    FILE *f = fopen(path, "rb");
    if (f == nullptr) return false;

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return false;
    }

    outBytes.resize(static_cast<size_t>(size));
    const size_t read = fread(outBytes.data(), 1, static_cast<size_t>(size), f);
    fclose(f);

    if (read != static_cast<size_t>(size)) {
        outBytes.clear();
        return false;
    }
    return true;
}

bool TileCache::writeTile(int z, int x, int y, const std::vector<uint8_t> &bytes) const {
    if (baseDir_.empty() || bytes.empty()) return false;

    if (useSqlite_ && db_) {
        std::lock_guard<std::mutex> lk(dbMtx_);
        sqlite3 *db = static_cast<sqlite3 *>(db_);
        sqlite3_stmt *stmt = nullptr;
        const char *sql =
            "INSERT OR REPLACE INTO tiles (zoom_level, tile_column, tile_row, tile_data) VALUES (?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            LOGW("TileCache sqlite prepare(write) failed: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_int(stmt, 1, z);
        sqlite3_bind_int(stmt, 2, x);
        sqlite3_bind_int(stmt, 3, tmsY(z, y));
        sqlite3_bind_blob(stmt, 4, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }

    // 文件系统后端
    MkdirRecursive(baseDir_);
    char dir[2048];
    int n = snprintf(dir, sizeof(dir), "%s/%d", baseDir_.c_str(), z);
    if (n <= 0 || n >= static_cast<int>(sizeof(dir))) return false;
    mkdir(dir, 0777);

    char path[2048];
    n = snprintf(path, sizeof(path), "%s/%d/%d_%d.tile", baseDir_.c_str(), z, x, y);
    if (n <= 0 || n >= static_cast<int>(sizeof(path))) return false;
    char tmp[2112];
    n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || n >= static_cast<int>(sizeof(tmp))) return false;

    FILE *f = fopen(tmp, "wb");
    if (f == nullptr) return false;
    const size_t wrote = fwrite(bytes.data(), 1, bytes.size(), f);
    fflush(f);
    fclose(f);
    if (wrote != bytes.size()) {
        remove(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

void TileCache::removeTile(int z, int x, int y) const {
    if (baseDir_.empty()) return;

    if (useSqlite_ && db_) {
        std::lock_guard<std::mutex> lk(dbMtx_);
        sqlite3 *db = static_cast<sqlite3 *>(db_);
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "DELETE FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, z);
            sqlite3_bind_int(stmt, 2, x);
            sqlite3_bind_int(stmt, 3, tmsY(z, y));
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return;
    }

    char path[2048];
    const int written = snprintf(path, sizeof(path), "%s/%d/%d_%d.tile", baseDir_.c_str(), z, x, y);
    if (written <= 0 || written >= static_cast<int>(sizeof(path))) return;
    remove(path);
}

} // namespace globecore
