#include "tileset3d/Tileset3DModel.h"

#include "util/Log.h"

#include <algorithm>
#include <atomic>

namespace globecore {

void Tileset3DLayer::enqueueModel(Tileset3DModelCpu &&model) {
    std::lock_guard<std::mutex> lock(mtx_);
    pending_.push_back(std::move(model));
    modelCount.store(static_cast<int>(pending_.size()));
}

std::vector<Tileset3DModelCpu> Tileset3DLayer::drainPendingModels() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<Tileset3DModelCpu> result;
    result.swap(pending_);
    return result;
}

std::string Tileset3DLayer::error() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return error_;
}

void Tileset3DLayer::setError(const std::string &error) {
    std::lock_guard<std::mutex> lock(mtx_);
    error_ = error;
}

// @Jin REPLACE 选择集：worker 线程每轮选择后整体替换，GL 线程按 URL 查表，短临界区不涉及网络/GL 调用。
void Tileset3DLayer::setActiveUrls(std::unordered_set<std::string> urls) {
    std::lock_guard<std::mutex> lock(activeMtx_);
    activeUrls_ = std::move(urls);
}

bool Tileset3DLayer::isUrlActive(const std::string &url) const {
    std::lock_guard<std::mutex> lock(activeMtx_);
    return activeUrls_.find(url) != activeUrls_.end();
}

bool Tileset3DLayer::hasActiveUrls() const {
    std::lock_guard<std::mutex> lock(activeMtx_);
    return !activeUrls_.empty();
}

Tileset3DModelCpu Tileset3DModelBuilder::build(
        const std::string &id, const std::string &sourceUrl,
        const B3dmContent &b3dm, const GltfModel &gltf,
        const Tileset3DTransform::Matrix &worldTransform,
        const std::string &gltfUpAxis, const Vec3 &boundsCenter,
        double boundsRadius) {
    Tileset3DModelCpu out;
    out.id = id;
    out.sourceUrl = sourceUrl;
    out.boundsCenter = boundsCenter;
    out.boundsRadius = boundsRadius;

    // @Jin 轴向处理：标准 Cesium 3D Tiles 的 tile 帧为 Y-up，gltfUpAxis=Z 时需把内容换到 Y-up；
    // 但本实验数据（mars3d osgb2tiles3）的 tile 帧本身是 Z-up ENU（见下方 if 内 ENU 验证结论），因此当前取 identity。
    Tileset3DTransform::Matrix axis = Tileset3DTransform::identity();
    const bool zUp = gltfUpAxis == "Z" || gltfUpAxis == "z";
    if (zUp) {
        // @Jin 不做换轴（identity）——经真机数据 ENU 数学验证的结论（2026-11-10）：
        // mars3d osgb2tiles3 虽声明 gltfUpAxis="Z"，但其 tile transform 的局部帧本身就是 Z-up 的 ENU 帧
        // （实测 localX→东、localY→北、localZ→天，点积均为 ±1.0000），而 b3dm 内容同样是 Z-up，
        // 二者天然对齐：内容 +z（建筑高度）经 identity 后仍指向 ENU Up。此前套用 Cesium 标准的
        // Z-up→Y-up 换轴（无论 (x,z,-y) 还是 (x,-z,y)）都会把高度轴转到局部 ±Y=南北水平方向，
        // 真机表现为整块模型相对地表旋转 90°、垂直立起或侧插。后续若接入局部帧为 Y-up 的标准
        // Cesium 数据，应先按包围盒/帧基向量检测上轴再决定是否换轴，而不是无条件 identity。
        axis = Tileset3DTransform::identity();
    }
    const Tileset3DTransform::Matrix localToWorld =
            Tileset3DTransform::multiply(worldTransform, axis);

    const Vec3 rtc(b3dm.hasRtcCenter ? b3dm.rtcCenter[0] : 0.0,
                   b3dm.hasRtcCenter ? b3dm.rtcCenter[1] : 0.0,
                   b3dm.hasRtcCenter ? b3dm.rtcCenter[2] : 0.0);
    // @Jin RTC_CENTER 与顶点同属 glTF 局部坐标，先经过同一轴向矩阵再进入 tile ECEF transform。
    const Vec3 rtcAxis = Tileset3DTransform::transformPoint(axis, rtc);
    out.anchorEcef = Tileset3DTransform::transformPoint(worldTransform, rtcAxis);

    uint32_t vertexOffset = 0;
    for (const GltfPrimitive &prim : gltf.primitives) {
        const size_t vertexCount = prim.positions.size() / 3;
        if (vertexCount == 0) continue;

        // @Jin 自动补法向：cesiumlab 等工具产出的倾斜摄影 b3dm 常只给 POSITION+TEXCOORD、无 NORMAL，
        // 若沿用默认 (0,0,1)，所有顶点法向一致，Lambert 光照均匀，建筑屋顶与立面无法区分、画面退化为平灰板。
        // 这里按三角形叉积做面积加权累加生成平滑顶点法向；后续与自带 NORMAL 走同一 localToWorld 变换通路。
        std::vector<float> generatedNormals;
        const std::vector<float> *normalSource = &prim.normals;
        if (prim.normals.size() < vertexCount * 3) {
            generatedNormals.assign(vertexCount * 3, 0.0f);
            std::vector<Vec3> normalAcc(vertexCount, Vec3(0.0, 0.0, 0.0));
            const auto positionAt = [&](uint32_t idx) -> Vec3 {
                return Vec3(prim.positions[static_cast<size_t>(idx) * 3],
                            prim.positions[static_cast<size_t>(idx) * 3 + 1],
                            prim.positions[static_cast<size_t>(idx) * 3 + 2]);
            };
            // @Jin 叉积模长等于 2 倍三角形面积，直接累加即面积加权；退化三角形叉积为零，自然无贡献。
            const auto accumulateTriangle = [&](uint32_t ia, uint32_t ib, uint32_t ic) {
                if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount) return;
                const Vec3 faceNormal = (positionAt(ib) - positionAt(ia)).cross(positionAt(ic) - positionAt(ia));
                normalAcc[ia] += faceNormal;
                normalAcc[ib] += faceNormal;
                normalAcc[ic] += faceNormal;
            };
            if (prim.use32) {
                for (size_t t = 0; t + 2 < prim.indices32.size(); t += 3) {
                    accumulateTriangle(prim.indices32[t], prim.indices32[t + 1], prim.indices32[t + 2]);
                }
            } else if (!prim.indices16.empty()) {
                for (size_t t = 0; t + 2 < prim.indices16.size(); t += 3) {
                    accumulateTriangle(prim.indices16[t], prim.indices16[t + 1], prim.indices16[t + 2]);
                }
            } else {
                // @Jin 非索引绘制：顶点按连续三元组组成三角形。
                for (uint32_t t = 0; t + 2 < vertexCount; t += 3) {
                    accumulateTriangle(t, t + 1, t + 2);
                }
            }
            for (size_t v = 0; v < vertexCount; ++v) {
                // @Jin 仅参与退化三角形的顶点累加结果为零向量，回退到 glTF 局部 Z 轴（Z-up 数据的朝上方向）。
                const Vec3 n = normalAcc[v].lengthSquared() > 1e-20
                                       ? normalAcc[v].normalized() : Vec3(0.0, 0.0, 1.0);
                generatedNormals[v * 3] = static_cast<float>(n.x);
                generatedNormals[v * 3 + 1] = static_cast<float>(n.y);
                generatedNormals[v * 3 + 2] = static_cast<float>(n.z);
            }
            normalSource = &generatedNormals;
        }

        // @Jin 交错步长 12 float：position(3)+normal(3)+uv(2)+color(4)。
        const size_t baseIndex = out.vertices.size() / 12;
        out.vertices.reserve(out.vertices.size() + vertexCount * 12);
        for (size_t i = 0; i < vertexCount; ++i) {
            const Vec3 local(prim.positions[i * 3],
                             prim.positions[i * 3 + 1],
                             prim.positions[i * 3 + 2]);
            const Vec3 withRtc(local.x + rtc.x, local.y + rtc.y, local.z + rtc.z);
            const Vec3 axisPoint = Tileset3DTransform::transformPoint(axis, withRtc);
            const Vec3 world = Tileset3DTransform::transformPoint(worldTransform, axisPoint);
            const Vec3 rtcModel = world - out.anchorEcef;

            // @Jin 优先用 glTF 自带 NORMAL；缺失时使用上方按三角形叉积生成的面积加权法向。
            Vec3 normal(0.0, 0.0, 1.0);
            if (normalSource->size() >= (i + 1) * 3) {
                normal = Vec3((*normalSource)[i * 3],
                              (*normalSource)[i * 3 + 1],
                              (*normalSource)[i * 3 + 2]).normalized();
            }
            const Vec3 worldNormal =
                    Tileset3DTransform::transformDirection(localToWorld, normal).normalized();

            // @Jin TEXCOORD_0：缺纹理坐标时给 (0,0)；glTF 的 V 原点在左上，stb 解码行序也是从上到下，
            // 而 GL 纹理采样原点在左下，这里做 V=1-v 翻转，避免贴图上下颠倒。
            float u = 0.0f;
            float v = 0.0f;
            if (prim.texCoords.size() >= (i + 1) * 2) {
                u = prim.texCoords[i * 2];
                // @Jin 直接使用 glTF 原始 V，不做 1-v 翻转：stb 解码出的像素首行即图像顶部，glTexImage2D 按原行序上传后
                // GL 纹理坐标 t=0 采样的正是首行；而 glTF 规范 UV(0,0) 同样对应图像左上角，两者天然一致，再翻转反而会把三角形
                // 镜像采样到纹理图集的黑色填充区（osgb2tiles 纹理常见留白），真机表现为黑灰碎片/椒盐噪点。
                v = prim.texCoords[i * 2 + 1];
            }

            out.vertices.insert(out.vertices.end(), {
                static_cast<float>(rtcModel.x), static_cast<float>(rtcModel.y),
                static_cast<float>(rtcModel.z),
                static_cast<float>(worldNormal.x), static_cast<float>(worldNormal.y),
                static_cast<float>(worldNormal.z),
                u, v,
                prim.baseColor[0], prim.baseColor[1], prim.baseColor[2], prim.baseColor[3]
            });
        }

        const auto appendIndex = [&](uint32_t localIndex) {
            out.indices.push_back(static_cast<uint32_t>(baseIndex) + localIndex);
        };
        if (prim.use32) {
            for (uint32_t index : prim.indices32) appendIndex(index);
        } else {
            for (uint16_t index : prim.indices16) appendIndex(static_cast<uint32_t>(index));
        }
        vertexOffset += static_cast<uint32_t>(vertexCount);
    }

    // @Jin 纹理搬运：当前模型合并为单 VBO 单 draw，取第一个有效 baseColorTexture；
    // 多 primitive 多纹理的数据暂不支持（实际 qx-dyt 每个 b3dm 均为 1 primitive 1 纹理），遇到时记录警告。
    for (const GltfPrimitive &prim : gltf.primitives) {
        if (prim.unlit) out.unlit = true;
        if (out.textureBytes.empty() && prim.imageIndex >= 0 &&
            static_cast<size_t>(prim.imageIndex) < gltf.images.size()) {
            const GltfEmbeddedImage &image = gltf.images[static_cast<size_t>(prim.imageIndex)];
            if (!image.bytes.empty()) {
                out.textureBytes = image.bytes;
                out.textureMime = image.mimeType;
            }
        }
    }
    if (gltf.images.size() > 1) {
        LOGW("Tileset3D multi-image glb not fully supported: images=%zu url=%s", gltf.images.size(), sourceUrl.c_str());
    }
    if (!out.indices.empty()) {
        uint32_t maxIndex = 0;
        for (uint32_t index : out.indices) maxIndex = std::max(maxIndex, index);
        if (maxIndex >= 65535u) {
            // @Jin GLES2 核心不保证 UNSIGNED_INT 索引；超限时展开为非索引三角形，兼容老设备并保持 P2 渲染闭环。
            std::vector<float> expanded;
            expanded.reserve(out.indices.size() * 12); // @Jin 交错步长随 UV 扩展为 12 float
            for (uint32_t index : out.indices) {
                const size_t src = static_cast<size_t>(index) * 12;
                if (src + 12 > out.vertices.size()) continue;
                expanded.insert(expanded.end(), out.vertices.begin() + static_cast<std::ptrdiff_t>(src),
                                out.vertices.begin() + static_cast<std::ptrdiff_t>(src + 12));
            }
            out.vertices.swap(expanded);
            out.indices.clear();
        }
    }

    // @Jin 追加 genNormal 标识：1 表示源 glTF 无法线、由叉积自动生成，便于真机排查明暗问题。
    const bool generatedNormal = std::any_of(gltf.primitives.begin(), gltf.primitives.end(),
                                              [](const GltfPrimitive &p) { return p.normals.empty(); });
    LOGI("Tileset3D model built: id=%s vertices=%zu indices=%zu zUp=%d rtc=%d genNormal=%d tex=%d unlit=%d",
         id.c_str(), out.vertices.size() / 12, out.indices.size(), zUp ? 1 : 0,
         b3dm.hasRtcCenter ? 1 : 0, generatedNormal ? 1 : 0,
         out.textureBytes.empty() ? 0 : 1, out.unlit ? 1 : 0);
    return out;
}

} // namespace globecore

