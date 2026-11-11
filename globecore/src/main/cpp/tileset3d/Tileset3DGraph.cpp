#include "tileset3d/Tileset3DGraph.h"

#include "tileset3d/Tileset3DUrl.h"
#include "util/Log.h"

#include <algorithm>
#include <cmath>

namespace globecore {

Tileset3DGraph Tileset3DGraphBuilder::build(const std::string &baseUrl, const TilesetResult &tileset,
                                      const Tileset3DTransform::Matrix &baseTransform) {
    Tileset3DGraph graph;
    graph.baseUrl = baseUrl;
    // @Jin 堆上持有 TilesetResult 拷贝：runtime 节点的 source 裸指针全部指向这份堆对象内部，
    // 图后续无论返回值移动还是 Provider move 赋值，堆对象地址不变，根节点指针也不会悬空。
    graph.tileset = std::make_unique<TilesetResult>(tileset);
    // @Jin 入参引用 tileset 在 build 返回后即销毁，source 必须锚定 graph 自持有的堆拷贝（graph.tileset->root 及其子树），
    // 否则 Provider 侧解引用 content.uri 会读到已释放内存，表现为 URL 乱码、b3dm 下载失败。
    graph.rootId = addNode(graph, graph.tileset->root, baseTransform, -1);
    LOGI("Tileset3D graph built: nodes=%zu root=%d", graph.nodes.size(), graph.rootId);
    return graph;
}

int Tileset3DGraphBuilder::addNode(Tileset3DGraph &graph, const TilesetNode &node,
                                   const Tileset3DTransform::Matrix &parentWorld,
                                   int parentId) {
    Tileset3DRuntimeNode runtime;
    runtime.id = static_cast<int>(graph.nodes.size());
    runtime.parentId = parentId;
    runtime.source = &node;
    runtime.worldTransform = Tileset3DTransform::multiply(parentWorld, node.transform);
    runtime.worldBounds = worldBounds(node, runtime.worldTransform);

    const int id = runtime.id;
    graph.nodes.push_back(runtime);
    for (const TilesetNode &child : node.children) {
        const int childId = addNode(graph, child, graph.nodes[id].worldTransform, id);
        graph.nodes[id].childIds.push_back(childId);
    }
    return id;
}

Tileset3DBounds Tileset3DGraphBuilder::worldBounds(const TilesetNode &node,
                                                   const Tileset3DTransform::Matrix &world) {
    Tileset3DBounds bounds;
    if (node.hasBox && node.box.size() >= 12) {
        // @Jin box 布局为 center(3) + xHalfAxis(3) + yHalfAxis(3) + zHalfAxis(3)。
        const Vec3 localCenter(node.box[0], node.box[1], node.box[2]);
        bounds.center = Tileset3DTransform::transformPoint(world, localCenter);
        const Vec3 ax = Tileset3DTransform::transformDirection(world, Vec3(node.box[3], node.box[4], node.box[5]));
        const Vec3 ay = Tileset3DTransform::transformDirection(world, Vec3(node.box[6], node.box[7], node.box[8]));
        const Vec3 az = Tileset3DTransform::transformDirection(world, Vec3(node.box[9], node.box[10], node.box[11]));
        // @Jin 保守外接球半径取三个半轴长度之和，保证旋转视角下不会错误裁剪。
        bounds.radius = ax.length() + ay.length() + az.length();
        bounds.valid = true;
        return bounds;
    }

    if (node.hasSphere) {
        // @Jin sphere 的中心做点变换，半径按最大缩放轴保守放大；当前 ECEF 刚体变换基本无缩放。
        const Vec3 localCenter(node.sphere[0], node.sphere[1], node.sphere[2]);
        bounds.center = Tileset3DTransform::transformPoint(world, localCenter);
        const Vec3 sx = Tileset3DTransform::transformDirection(world, Vec3(1.0, 0.0, 0.0));
        const Vec3 sy = Tileset3DTransform::transformDirection(world, Vec3(0.0, 1.0, 0.0));
        const Vec3 sz = Tileset3DTransform::transformDirection(world, Vec3(0.0, 0.0, 1.0));
        const double scale = std::max({sx.length(), sy.length(), sz.length()});
        bounds.radius = node.sphere[3] * scale;
        bounds.valid = true;
        return bounds;
    }

    // @Jin region 包围体需要经纬度+高度转 ECEF，P1 数据全部为 box，先显式标记无效避免误裁剪。
    LOGW("Tileset3D node has unsupported/empty bounding volume; skip culling bounds");
    return bounds;
}

std::vector<Tileset3DSelection> Tileset3DGraphBuilder::select(
        const Tileset3DGraph &graph, const Vec3 &eyeEcef, int viewportHeightPx,
        double fovYRad, double maximumScreenSpaceError,
        const std::array<FrustumPlane, 6> *frustumPlanes) {
    std::vector<Tileset3DSelection> selections;
    if (graph.rootId < 0) return selections;
    traverse(graph, graph.rootId, eyeEcef, viewportHeightPx, fovYRad,
             maximumScreenSpaceError, frustumPlanes, selections);
    return selections;
}

void Tileset3DGraphBuilder::traverse(const Tileset3DGraph &graph, int nodeId,
                                     const Vec3 &eyeEcef, int viewportHeightPx,
                                     double fovYRad, double maximumScreenSpaceError,
                                     const std::array<FrustumPlane, 6> *frustumPlanes,
                                     std::vector<Tileset3DSelection> &out) {
    if (nodeId < 0 || nodeId >= static_cast<int>(graph.nodes.size())) return;
    const Tileset3DRuntimeNode &node = graph.nodes[nodeId];
    if (node.source == nullptr) return;

    if (node.worldBounds.valid && frustumPlanes != nullptr) {
        for (const FrustumPlane &plane : *frustumPlanes) {
            const double distance = plane.distanceTo(node.worldBounds.center.x,
                                                      node.worldBounds.center.y,
                                                      node.worldBounds.center.z);
            if (distance < -node.worldBounds.radius) {
                return; // @Jin 只要完全落在任一视锥平面外侧即剔除；相交时保留以避免裁剪闪烁。
            }
        }
    }

    const Vec3 toCenter = node.worldBounds.center - eyeEcef;
    const double centerDistance = toCenter.length();
    const double surfaceDistance = std::max(1.0, centerDistance - node.worldBounds.radius);
    const double tanHalfFov = std::max(1e-6, std::tan(fovYRad * 0.5));
    const double sse = node.source->geometricError * static_cast<double>(viewportHeightPx) /
                       (surfaceDistance * 2.0 * tanHalfFov);

    const bool leaf = node.source->children.empty();
    const bool meetsError = node.source->geometricError <= 0.0 || sse <= maximumScreenSpaceError;
    const bool hasContent = !node.source->content.uri.empty();
    if (hasContent && (meetsError || leaf)) {
        Tileset3DSelection item;
        item.worldTransform = node.worldTransform;
        item.nodeId = nodeId;
        item.contentUrl = Tileset3DUrl::resolve(graph.baseUrl, node.source->content.uri);
        item.refine = node.source->refine;
        item.geometricError = node.source->geometricError;
        item.boundsCenter = node.worldBounds.center;
        item.boundsRadius = node.worldBounds.radius;
        item.isB3dm = Tileset3DUrl::isB3dm(item.contentUrl);
        item.isExternalTileset = Tileset3DUrl::isTilesetJson(item.contentUrl);
        out.push_back(item);
        return; // @Jin P1 先按 REPLACE 语义处理，选中内容后不继续向下重复选择。
    }

    for (int childId : node.childIds) {
        traverse(graph, childId, eyeEcef, viewportHeightPx, fovYRad,
                 maximumScreenSpaceError, frustumPlanes, out);
    }
}

} // namespace globecore


