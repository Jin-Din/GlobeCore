#include "layer/TilesetJson.h"

#include "util/JsonParser.h"
#include "util/Log.h"

#include <functional>

namespace globecore {

namespace {

/// 递归解析 tile 节点
static TilesetNode parseNode(const JsonValue &nodeVal) {
    TilesetNode node;

    // boundingVolume
    const JsonValue &bv = nodeVal.at("boundingVolume");
    if (bv.isObject()) {
        const JsonValue &box = bv.at("box");
        if (box.isArray() && box.arr.size() >= 12) {
            node.hasBox = true;
            node.box.resize(12);
            for (int i = 0; i < 12; ++i) {
                if (box.arr[i].isNumber()) node.box[i] = box.arr[i].numVal;
            }
        }
        const JsonValue &region = bv.at("region");
        if (region.isArray() && region.arr.size() >= 6) {
            node.hasRegion = true;
            for (int i = 0; i < 6; ++i) {
                if (region.arr[i].isNumber()) node.region[i] = region.arr[i].numVal;
            }
        }
        const JsonValue &sphere = bv.at("sphere");
        if (sphere.isArray() && sphere.arr.size() >= 4) {
            node.hasSphere = true;
            for (int i = 0; i < 4; ++i) {
                if (sphere.arr[i].isNumber()) node.sphere[i] = sphere.arr[i].numVal;
            }
        }
    }

    // geometricError
    const JsonValue &ge = nodeVal.at("geometricError");
    if (ge.isNumber()) node.geometricError = ge.numVal;

    // refine
    const JsonValue &refine = nodeVal.at("refine");
    if (refine.isString()) node.refine = refine.strVal;

    // content
    const JsonValue &content = nodeVal.at("content");
    if (content.isObject()) {
        const JsonValue &uri = content.at("uri");
        if (uri.isString()) node.content.uri = uri.strVal;
    }

    // children (recursive)
    const JsonValue &children = nodeVal.at("children");
    if (children.isArray()) {
        for (const auto &child : children.arr) {
            if (child.isObject()) {
                node.children.push_back(parseNode(child));
            }
        }
    }

    return node;
}

} // namespace

TilesetResult parseTileset(const std::string &jsonText) {
    TilesetResult result;
    JsonValue root = parseJson(jsonText);
    if (!root.isObject()) {
        result.error = "JSON 解析失败或非对象";
        return result;
    }

    // asset geometricError (optional, root-level)
    const JsonValue &asset = root.at("asset");
    if (asset.isObject()) {
        const JsonValue &ge = asset.at("geometricError");
        if (ge.isNumber()) result.geometricError = ge.numVal;
    }

    // root node
    const JsonValue &rootNode = root.at("root");
    if (!rootNode.isObject()) {
        result.error = "无 root 节点";
        return result;
    }
    result.root = parseNode(rootNode);
    result.ok = true;

    // 统计 tile 数
    int tileCount = 0;
    std::function<void(const TilesetNode &)> count = [&](const TilesetNode &n) {
        tileCount++;
        for (const auto &c : n.children) count(c);
    };
    count(result.root);

    LOGI("TilesetJson parsed: tiles=%d rootErr=%.1f content=%s",
         tileCount, result.root.geometricError, result.root.content.uri.c_str());
    return result;
}

} // namespace globecore
