#ifndef GLOBECORE_TILESET3D_TILESET3DMODEL_H
#define GLOBECORE_TILESET3D_TILESET3DMODEL_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set> // @Jin setActiveUrls 使用 unordered_set，头文件自包含
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
    // @Jin 交错顶点：position(3) + normal(3) + uv(2) + color(4)，每顶点 12 float；位置为 anchorEcef 的局部 RTC 坐标。
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    Vec3 boundsCenter;
    double boundsRadius = 0.0;
    // @Jin 内嵌纹理（JPEG/PNG 编码字节），由 GL 线程 ImageDecoder 解码上传；为空表示纯色渲染。
    std::vector<uint8_t> textureBytes;
    std::string textureMime;
    // @Jin KHR_materials_unlit：为 true 时片元直接输出纹理色，跳过 Lambert 光照。
    bool unlit = false;
};

/**
 * @Jin 3D Tiles 图层的 CPU 侧状态；GL 资源仍由 Renderer 持有，避免非 GL 线程直接操作 OpenGL。
 */
class Tileset3DLayer {
public:
    explicit Tileset3DLayer(int layerId) : id(layerId) {}

    int layerId() const { return id; }
    bool visible = true;
    // @Jin 实验性垂直偏移（米）：沿锚点 ECEF 天顶方向整体平移图层，用于校准倾斜模型与地形的高程基准差（椭球高/正高、DEM 综合误差）。
    // @Jin 默认 0 与旧行为完全一致；只改绘制 uniform 不重建模型，运行时可调。回退方式：setter 恒传 0 或删除本字段与 Renderer 注入点。
    std::atomic<double> heightOffset{0.0};
    void setHeightOffset(double meters) { heightOffset.store(meters); }
    double getHeightOffset() const { return heightOffset.load(); }
    std::atomic<bool> loading{false};
    std::atomic<int> modelCount{0}; // @Jin 累计接收模型数，drain 后不清零，供状态展示与后续 LRU 统计使用。

    // @Jin Provider worker 写入，Renderer GL 线程 drain；只在短临界区内移动 vector，网络/解码不持锁。
    void enqueueModel(Tileset3DModelCpu &&model);
    std::vector<Tileset3DModelCpu> drainPendingModels();
    std::string error() const;
    void setError(const std::string &error);

    // @Jin REPLACE 语义支持：Provider 每轮把当前视距下应显示的 b3dm URL 集合发布到这里；
    // Renderer 仅绘制集合内的驻留模型，历史 LOD 即使 GPU 资源仍缓存也不再上屏（等价运行时淘汰，省显存淘汰后续再做）。
    void setActiveUrls(std::unordered_set<std::string> urls);
    bool isUrlActive(const std::string &url) const;
    bool hasActiveUrls() const;

private:
    int id = -1;
    mutable std::mutex mtx_;
    std::vector<Tileset3DModelCpu> pending_;
    std::string error_;
    // @Jin 当前选择集（REPLACE：子块选中后父块 URL 不在集合内即跳过绘制）。
    mutable std::mutex activeMtx_;
    std::unordered_set<std::string> activeUrls_;
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

