#ifndef GLOBECORE_TILESET3D_TILESET3DMODEL_H
#define GLOBECORE_TILESET3D_TILESET3DMODEL_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "geom/Vec3.h"
#include "gltf/GltfDecoder.h"
#include "tileset3d/B3dmDecoder.h"
#include "tileset3d/Tileset3DTransform.h"

namespace globecore {

/**
 * @Jin 已完成 CPU 坐标转换的 3D Tiles 模型，等待 Renderer 在 GL 线程上传为 VBO/IBO。
 */
struct Tileset3DModelCpu {
    std::string id;
    std::string sourceUrl;
    Vec3 anchorEcef;
    // @Jin 交错顶点：position(3) + normal(3) + color(4)，位置为 anchorEcef 的局部 RTC 坐标。
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    Vec3 boundsCenter;
    double boundsRadius = 0.0;
};

/**
 * @Jin 3D Tiles 图层的 CPU 侧状态；GL 资源仍由 Renderer 持有，避免非 GL 线程直接操作 OpenGL。
 */
class Tileset3DLayer {
public:
    explicit Tileset3DLayer(int layerId) : id(layerId) {}

    int layerId() const { return id; }
    bool visible = true;
    std::atomic<bool> loading{false};
    std::atomic<int> modelCount{0}; // @Jin 累计接收模型数，drain 后不清零，供状态展示与后续 LRU 统计使用。

    // @Jin Provider worker 写入，Renderer GL 线程 drain；只在短临界区内移动 vector，网络/解码不持锁。
    void enqueueModel(Tileset3DModelCpu &&model);
    std::vector<Tileset3DModelCpu> drainPendingModels();
    std::string error() const;
    void setError(const std::string &error);

private:
    int id = -1;
    mutable std::mutex mtx_;
    std::vector<Tileset3DModelCpu> pending_;
    std::string error_;
};

/**
 * @Jin b3dm/GLB 到引擎 RTC 模型的 CPU 转换器。
 */
class Tileset3DModelBuilder {
public:
    // @Jin worldTransform 为 tile 节点累积后的 ECEF 变换；gltfUpAxis=Z 时先做轴向转换。
    static Tileset3DModelCpu build(const std::string &id,
                                   const std::string &sourceUrl,
                                   const B3dmContent &b3dm,
                                   const GltfModel &gltf,
                                   const Tileset3DTransform::Matrix &worldTransform,
                                   const std::string &gltfUpAxis,
                                   const Vec3 &boundsCenter,
                                   double boundsRadius);
};

} // namespace globecore

#endif // GLOBECORE_TILESET3D_TILESET3DMODEL_H

