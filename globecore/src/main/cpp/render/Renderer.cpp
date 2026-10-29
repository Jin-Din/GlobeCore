#include "render/Renderer.h"

#include "geom/Matrix4.h"
#include "globe/MercatorProjection.h"
#include "globe/Tessellator.h"
#include "globe/GeographicQuadtree.h"
#include "globe/TileMatrix.h"
#include "globe/Wgs84Globe.h"
#include "render/FontAtlas.h"
#include "render/ImageDecoder.h"
#include "net/HttpClient.h"
#include "rhi/RHIFactory.h"
#include "util/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace globecore {

// ── 构建核对 ID（手工维护，用于判别 HAR 是否真正重建并生效）────────────────
// 每次准备重建 HAR 前，把 GLOBECORE_BUILD_ID 改成一个全新的唯一串（例如末尾序号 +1，或带上你自定义的标记）。
// 装机日志出现该串 = 跑的确实是你这次改过、并重新构建 + 重新链接的 HAR；
// 未出现 / 仍是旧串 = app 还在用旧 HAR（引擎源码根本没进设备）。改完务必：重建 HAR → app Sync/ohpm 重新链接 → 卸载重装。
#define GLOBECORE_BUILD_ID "v0.1.3-20261029-local-quick-summit-label04" // @Jin 构建核对：快捷定位 POI 与文字拆层、显式加载 Harmony 中文字体，并放大山顶文字。

// 祖先链请求深度帽：叶子只向上请求 4 级。更深的祖先拉伸 >16× 已无兜底价值，而链条请求数是
// 数量倍增器；尤其无金字塔栅格源，低级别粗瓦源窗口≈整幅（单块秒级～十几秒），砍深度直接减掉
// 每片叶子的重复全幅读。配合 TileLoader「最细级优先出队」：可见级先出图，祖先垫后且仅 4 级。
// @Jin: 祖先瓦片只兜底最近 2 级，保留缩放过渡期垫图，同时减少首次浏览时的额外请求量。
constexpr int kAncestorChainDepth = 2;

// @Jin 实验开关：交互期共享纹理上传预算；效果不达标时可连同本次实验提交整体 revert。
constexpr bool kExperimentalAdaptiveUploadBudget = true;
// @Jin 实验开关：在线 GeoJSON 普通 VectorLayer 在 3D 下也走 DEM 贴地烘焙；效果不佳可改 false 回退到椭球面路径。
constexpr bool kExperimentalRemoteVectorTerrainDrape = true;
constexpr double kRemoteDrapeMaxCameraAltMeters = 500000.0; // @Jin drape23 高空只走平面矢量，低于 500km 再贴地重建，避免全球视角为整省大面同步采 DEM。
// @Jin 单帧上传上限按“张数 + RGBA 字节”双约束，所有瓦片层共用，而不是每层各自 32 张。
constexpr int kInteractiveTextureUploadTiles = 4; // @Jin exp1 真机复测：6 张/帧仍有 26~29ms，降到 4 张优先保证手势帧率。
constexpr size_t kInteractiveTextureUploadBytes = 1ull * 1024 * 1024; // @Jin 交互期约 4 张 256 瓦片，限制在约 1MiB RGBA 上传量。
constexpr int kIdleTextureUploadTiles = 4; // @Jin exp1b 真机复测：8 张/帧仍有 54.5ms 长帧，空闲期也压到 4 张。
constexpr size_t kIdleTextureUploadBytes = 1ull * 1024 * 1024; // @Jin 空闲期限制约 1MiB，优先保证加载期画面稳定。
// @Jin DEM 到达时仅允许少量瓦片当帧重建网格，其余保持上一版几何并在后续帧平滑升级。
constexpr int kInteractiveMeshRebuildsPerFrame = 2;
constexpr int kIdleMeshRebuildsPerFrame = 4; // @Jin DEM 首批到达时降低 24x24 地形网格重建峰值。
// @Jin 没有 DEM 覆盖时使用固定签名，避免空集合哈希受 unordered_map 重排影响。
constexpr uint64_t kEmptyTerrainSignature = 1469598103934665603ull;

void Renderer::setCameraFlightActive(bool active) {
    cameraFlightActive_ = active;
    if (active) lastCameraFlightTs_ = std::chrono::steady_clock::now();
}

bool Renderer::cameraFlightBudgetActive() const {
    if (cameraFlightActive_) return true;
    if (lastCameraFlightTs_.time_since_epoch().count() == 0) return false;
    constexpr auto kFlightTailBudget = std::chrono::milliseconds(300);
    return std::chrono::steady_clock::now() - lastCameraFlightTs_ < kFlightTailBudget;
}

namespace {

// @Jin: FNV-1a 视口签名：只把“最终需要的瓦片集合”编码给 TileLoader，集合不变时保留补帧队列，集合变化时立即废弃旧请求。
uint64_t mixTileViewValue(uint64_t hash, uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

uint64_t tileViewSignature2D(int level, int txMin, int txMax, int tyMin, int tyMax) {
    uint64_t hash = 14695981039346656037ULL;
    hash = mixTileViewValue(hash, 0x32442031ULL); // @Jin: 固定盐值，区分 2D 视口与 3D 瓦片集。
    hash = mixTileViewValue(hash, static_cast<uint64_t>(level));
    hash = mixTileViewValue(hash, static_cast<uint64_t>(txMin));
    hash = mixTileViewValue(hash, static_cast<uint64_t>(txMax));
    hash = mixTileViewValue(hash, static_cast<uint64_t>(tyMin));
    hash = mixTileViewValue(hash, static_cast<uint64_t>(tyMax));
    return hash;
}

uint64_t tileViewSignature3D(const std::vector<GlobeTile> &tiles) {
    uint64_t hash = 14695981039346656037ULL;
    hash = mixTileViewValue(hash, 0x33442031ULL); // @Jin: 固定盐值，区分 3D 视口与 2D 范围。
    hash = mixTileViewValue(hash, static_cast<uint64_t>(tiles.size()));
    for (const GlobeTile &t : tiles) {
        hash = mixTileViewValue(hash, static_cast<uint64_t>(t.z));
        hash = mixTileViewValue(hash, static_cast<uint64_t>(t.x));
        hash = mixTileViewValue(hash, static_cast<uint64_t>(t.y));
    }
    return hash;
}

// ── 占位（纯色）着色器：世界坐标顶点 + 颜色，正交投影输出 ──────────────────────
// uOffset 为 RTC 偏移：矢量顶点存为「相对图层原点」的 float32，着色器做 aPos+uOffset 还原到
// 相机相对坐标（等价瓦片的 world−camCenter）；瓦片占位与定位标记的顶点已烘焙相机相对坐标，故传 (0,0)。
const char *const kColorVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec4 aColor;
uniform mat4 uMvp;
uniform vec2 uOffset;
varying vec4 vColor;
void main() {
    gl_Position = uMvp * vec4(aPos + uOffset, 0.0, 1.0);
    vColor = aColor;
}
)GLSL";

const char *const kColorFragmentSrc = R"GLSL(
precision mediump float;
varying vec4 vColor;
void main() {
    gl_FragColor = vColor;
}
)GLSL";

// ── GPU 拾取着色器（2D 面填充）：aPos + aPickId → 颜色编码 (layerIndex, primIndex) ──
// 与 colorProgram_ 同口径（aPos + uOffset），但以 PickID 替代顶点色输出；片元直接输出 varying。
// 拾取 pass 渲到离屏 FBO，glReadPixels 读点击像素解码回 (layerIndex, primIndex) → pickPrims[primIndex].fid。
const char *const kPickColorVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec4 aPickId;
uniform mat4 uMvp;
uniform vec2 uOffset;
varying vec4 vPickId;
void main() {
    gl_Position = uMvp * vec4(aPos + uOffset, 0.0, 1.0);
    vPickId = aPickId;
}
)GLSL";

// ── GPU 拾取着色器（2D 线/描边）：aPos + aMiter + aPickId → 同上但加 miter 偏移 ──
// 与 lineProgram_ 同口径（aPos + uOffset + aMiter*uHalfWidthWorld），以 PickID 替代顶点色输出。
const char *const kPickLineVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec2 aMiter;
attribute vec4 aPickId;
uniform mat4 uMvp;
uniform vec2 uOffset;
uniform float uHalfWidthWorld;
varying vec4 vPickId;
void main() {
    vec2 p = aPos + uOffset + aMiter * uHalfWidthWorld;
    gl_Position = uMvp * vec4(p, 0.0, 1.0);
    vPickId = aPickId;
}
)GLSL";

const char *const kPickFragmentSrc = R"GLSL(
precision mediump float;
varying vec4 vPickId;
void main() {
    gl_FragColor = vPickId;
}
)GLSL";

// ── 线（屏幕空间等宽三角带）顶点着色器：中心线顶点 + miter 法向，按像素半宽（换算世界单位）偏移 ──
// aMiter 为加载期烘焙的接头/端点法向（含 miter 长度系数），uHalfWidthWorld = 像素半宽 × 世界单位/像素，
// 每帧按缩放传入 → 屏幕空间恒定像素线宽；片元复用 kColorFragmentSrc（同为 varying vec4 vColor）。
const char *const kLineVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec2 aMiter;
attribute vec4 aColor;
uniform mat4 uMvp;
uniform vec2 uOffset;
uniform float uHalfWidthWorld;
varying vec4 vColor;
void main() {
    vec2 p = aPos + uOffset + aMiter * uHalfWidthWorld;
    gl_Position = uMvp * vec4(p, 0.0, 1.0);
    vColor = aColor;
}
)GLSL";

// ── 纹理着色器：单位四边形 + uv，逐瓦片模型矩阵定位，采样瓦片纹理 ─────────────────
const char *const kTexVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec2 aTexCoord;
uniform mat4 uMvp;
uniform vec2 uUvOffset;
uniform vec2 uUvScale;
varying vec2 vTexCoord;
void main() {
    gl_Position = uMvp * vec4(aPos, 0.0, 1.0);
    // 自身纹理时 uUvOffset=(0,0)、uUvScale=(1,1) 为恒等；祖先兜底时映射到祖先纹理的 UV 子矩形
    vTexCoord = uUvOffset + aTexCoord * uUvScale;
}
)GLSL";

const char *const kTexFragmentSrc = R"GLSL(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)GLSL";

// ── 文本（矢量标注）着色器：屏幕空间 billboard 字形四边形 ──────────────────
// 顶点位置已为相机相对世界坐标（每帧烘焙），无需 uOffset；片元以图集覆盖率（alpha 通道）× 文字色 alpha 输出，
// rgb 取顶点颜色（图集 rgb 恒为白，仅用其 alpha 作覆盖率蒙版）。
const char *const kTextVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec2 aTexCoord;
attribute vec4 aColor;
uniform mat4 uMvp;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    gl_Position = uMvp * vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)GLSL";

const char *const kTextFragmentSrc = R"GLSL(
precision mediump float;
varying vec2 vTexCoord;
varying vec4 vColor;
uniform sampler2D uTexture;
void main() {
    float cov = texture2D(uTexture, vTexCoord).a;
    gl_FragColor = vec4(vColor.rgb, vColor.a * cov);
}
)GLSL";

// 图标顶点：矢量点要素图标 billboard 用——顶点存「中心坐标 + 角点属性 (mx,my)∈[-1,1]」，
// 着色器按 aPos + uOffset + aCorner*uHalf 展开四边形：uOffset 为图层 RTC 偏移、uHalf 为世界半尺寸
// （像素半尺寸 × 世界单位/像素，每帧传）→ 屏幕固定尺寸；顶点不随相机变化，可常驻静态 VBO，
// 免逐帧 CPU 重建 + glBufferData（原每帧逐层重建全部四边形顶点）。片元见 kIconFragmentSrc。
const char *const kIconVertexSrc = R"GLSL(
attribute vec2 aPos;
attribute vec2 aCorner;
attribute vec2 aTexCoord;
attribute vec4 aColor;
uniform mat4 uMvp;
uniform vec2 uOffset;
uniform vec2 uHalf;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    vec2 p = aPos + uOffset + aCorner * uHalf;
    gl_Position = uMvp * vec4(p, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)GLSL";

// 图标片元：矢量点要素图标 billboard 用——输出 图标纹理 RGBA × 顶点色（默认白，即纯图标）。
// 与文本片元（仅取图集 alpha 作覆盖率）不同，图标位图含完整彩色，须保留 rgb。顶点着色器见 kIconVertexSrc。
const char *const kIconFragmentSrc = R"GLSL(
precision mediump float;
varying vec2 vTexCoord;
varying vec4 vColor;
uniform sampler2D uTexture;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord) * vColor;
}
)GLSL";

// ── 3D 球面瓦片纹理着色器：pos3 + uv（交错 9 floats，见 kGlobeFloatsPerVertex），透视 MVP ──
// uMvp 传 viewProjRtc（顶点已在 CPU 端 double 减眼点，RTC 口径同 2D 相机相对渲染，否则 ECEF ~6.4e6 m
// 量级下 float32 仅 0.5~1 m 误差，近地视角顶点拖动/自阴影条纹）；uUvOffset/uUvScale 同 2D 做祖先子矩形。
// 加 uEye：顶点算球面法向（normalize(aPos+uEye)，aPos 为减眼后 RTC，加回 eye 得 ECEF 表面点方向即法向）
// 与视线方向 -aPos（眼点相对表面点的方向），传片元做 Lambert 漫反射 + 边缘 rim glow（大气感）。
const char *const kGlobeTexVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec2 aTexCoord;
uniform mat4 uMvp;
uniform vec2 uUvOffset;
uniform vec2 uUvScale;
uniform vec3 uRtcOffset;   // @Jin 瓦片中心 ECEF − eye：局部坐标还原为眼点相对坐标（唯一逐帧变化量）
uniform vec3 uTileCenter;  // @Jin 瓦片中心 ECEF：aPos 是相对瓦片中心的静态局部坐标
varying vec2 vTexCoord;
varying vec3 vNormal;
varying vec3 vViewDir;
void main() {
    vec3 rtc = aPos + uRtcOffset; // @Jin 眼点相对坐标；静态 VBO + uniform 平移，缓冲零重传
    gl_Position = uMvp * vec4(rtc, 1.0);
    vTexCoord = uUvOffset + aTexCoord * uUvScale;
    vNormal = normalize(aPos + uTileCenter); // 球面法向（表面 ECEF 方向），顶点阶段归一化防 mediump 丢精度
    vViewDir = normalize(-rtc);              // 表面→眼点的视线方向
}
)GLSL";

// 3D 瓦片纹理片元着色器：Lambert 漫反射（uSunDir·N）+ 边缘 rim glow（青蓝大气边缘光）。
// uAmbient 为环境光底（0.25 左右），diff 为日光贡献；rim 在视角切线方向增强模拟大气壳散射。
const char *const kGlobeTexFragmentSrc = R"GLSL(
precision mediump float;
varying vec2 vTexCoord;
varying vec3 vNormal;
varying vec3 vViewDir;
uniform sampler2D uTexture;
uniform vec3 uSunDir;
uniform float uAmbient;
void main() {
    vec4 tex = texture2D(uTexture, vTexCoord);
    vec3 n = normalize(vNormal);
    vec3 v = normalize(vViewDir);
    float diff = max(dot(n, uSunDir), 0.0);
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);
    vec3 color = tex.rgb * (uAmbient + (1.0 - uAmbient) * diff);
    color += vec3(0.3, 0.5, 0.7) * rim * 0.25;   // 青蓝大气边缘光
    gl_FragColor = vec4(color, tex.a);
}
)GLSL";

// 3D 占位着色器：同交错缓冲取 pos3 + color4，画无图无祖先瓦片的棋盘深色。同样加 uEye 与法向/视线
// varying，片元用 kGlobeColorFragmentSrc 做 Lambert + rim glow，使占位与纹理瓦片光照一致。
const char *const kGlobeColorVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec4 aColor;
uniform mat4 uMvp;
uniform vec3 uRtcOffset;  // @Jin 同纹理通路：瓦片中心 ECEF − eye
uniform vec3 uTileCenter; // @Jin 瓦片中心 ECEF（求法向）
varying vec4 vColor;
varying vec3 vNormal;
varying vec3 vViewDir;
void main() {
    vec3 rtc = aPos + uRtcOffset; // @Jin 静态局部坐标 → 眼点相对坐标
    gl_Position = uMvp * vec4(rtc, 1.0);
    vColor = aColor;
    vNormal = aPos + uTileCenter;
    vViewDir = -rtc;
}
)GLSL";

// 3D 占位瓦片片元着色器：与 kGlobeTexFragmentSrc 同口径（Lambert + rim glow），但 rgb 取顶点色（棋盘深色）。
const char *const kGlobeColorFragmentSrc = R"GLSL(
precision mediump float;
varying vec4 vColor;
varying vec3 vNormal;
varying vec3 vViewDir;
uniform vec3 uSunDir;
uniform float uAmbient;
void main() {
    vec3 n = normalize(vNormal);
    vec3 v = normalize(vViewDir);
    float diff = max(dot(n, uSunDir), 0.0);
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);
    vec3 color = vColor.rgb * (uAmbient + (1.0 - uAmbient) * diff);
    color += vec3(0.3, 0.5, 0.7) * rim * 0.25;
    gl_FragColor = vec4(color, vColor.a);
}
)GLSL";

// ── P2.a 3D 矢量面填充顶点着色器 ──────────────────────────────────────
// 顶点在 CPU 端 worldXYToCartesian(逐顶点高程) − 图层固定锚点 anchor（纯几何，不随相机/高度变）；
// uOffset=anchor−eye 补 eye 平移、uAnchor=anchor 供恢复 ecef 求法向。
// 高度：aMode=1（高程要素）→ 已 baked 在 aPos 的真实高度，不再叠加 uVecAlt；aMode=0（贴地）→ 沿用 uVecAlt
// 沿法向抬升（防与瓦片 z-fighting、随缩放零重建）。即 抬升 = uVecAlt*(1-aMode)。uMvp 传 viewProjRtc。
const char *const kGlobeFillVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec4 aColor;
attribute float aMode;
uniform mat4 uMvp;
uniform vec3 uOffset;
uniform vec3 uAnchor;
uniform float uVecAlt;
varying vec4 vColor;
void main() {
    vec3 nrm = normalize(aPos + uAnchor);   // 表面法向（球近似：ecef 方向 = aPos+anchor）
    gl_Position = uMvp * vec4(aPos + uOffset + nrm * (uVecAlt * (1.0 - aMode)), 1.0);
    vColor = aColor;
}
)GLSL";

// ── P2.a 3D 矢量描边/线顶点着色器（参考实现 3-顶点模板屏幕空间 miter 移植）─────────────
// 参考 参考实现Kotlin/render/program/TriangleShaderProgram.kt，与本项目差异：
//   1) corner 从独立 attribute aSide 传入（±±），不再利 参考实现 将 corner 编到 pointB.w（需 cornerX 回退）；
//   2) 去掉 bevel 回退（仅靠 uMiterLimit 钳制）；无 enableTexture/lighting/shadows 分支；
//   3) 顶点位置已 RTC 减眼，无 modelMatrix。
// 流程同 参考实现：三顶点过 MVP → NDC → 视口像素空差归一→ AB/BC→tangent→miter→miterLength（钳住）；
// 端点 prev==cur 或 next==cur 时 AB/BC 退化为 0，归一到同侧使 miter = 段法向；
// gl_Position.xy = pCur.w * (pCur.xy + side * miter * uHalfWidthPx * 2.0/uViewport) → 恒定像素半宽。
const char *const kGlobeStrokeVertexSrc = R"GLSL(
attribute vec3 aPrev;
attribute vec3 aCur;
attribute vec3 aNext;
attribute float aSide;
attribute vec4 aColor;
attribute float aMode;
uniform mat4 uMvp;
uniform vec3 uOffset;
uniform vec3 uAnchor;
uniform float uVecAlt;
uniform float uHalfWidthPx;
uniform vec2 uViewport;
uniform float uMiterLimit;
varying vec4 vColor;
void main() {
    float lift = uVecAlt * (1.0 - aMode);   // 贴地走 uVecAlt，高程要素（已 baked 高度）不叠加
    vec4 pPS = uMvp * vec4(aPrev + uOffset + normalize(aPrev + uAnchor) * lift, 1.0);
    vec4 pCS = uMvp * vec4(aCur  + uOffset + normalize(aCur  + uAnchor) * lift, 1.0);
    vec4 pNS = uMvp * vec4(aNext + uOffset + normalize(aNext + uAnchor) * lift, 1.0);
    float eps = 1e-4;
    // 当前顶点 cur 在相机背后/近平面上（w<=eps）时，屏幕空间 miter 无意义：若只把 w 强设 eps，
    // clip.xy 不变 → pC = clip.xy/eps 爆到天际 → 与相邻正常顶点构成的 strip 三角斜穿屏幕（重叠区/
    // 地平线附近撕裂的真因）。正确做法：cur 塌缩到任一在前的邻点使该段零宽退化（前后邻都在后则整顶点
    // 丢到裁剪外）。cur 塌缩后 w 变为邻点的有效 w，后续 pC 除法安全。
    if (pCS.w < eps) {
        if (pPS.w >= eps)      pCS = pPS;
        else if (pNS.w >= eps) pCS = pNS;
        else { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); vColor = aColor; return; }
    }
    // prev/next 在 cur 之后仍可能在背后：退化到 cur（已保证 w>=eps）→ AB/BC 零长，下方归一到同侧法向。
    if (pPS.w < eps) pPS = pCS;
    if (pNS.w < eps) pNS = pCS;
    vec2 pP = pPS.xy / pPS.w;
    vec2 pC = pCS.xy / pCS.w;
    vec2 pN = pNS.xy / pNS.w;
    // 屏幕像素空间的段方向（NDC 差 * viewport/2，非方形视口下 miter 方向才欧氏正确）
    vec2 ABraw = (pC - pP) * uViewport * 0.5;
    vec2 BCraw = (pN - pC) * uViewport * 0.5;
    // 端点/重合点退化：把零长向量归一到同侧（tangent=normalize(AB+BC) 仍为段方向，miter = 段法向）
    if (dot(ABraw, ABraw) < 1e-8) ABraw = BCraw;
    if (dot(BCraw, BCraw) < 1e-8) BCraw = ABraw;
    if (dot(ABraw, ABraw) < 1e-8) ABraw = vec2(1.0, 0.0); // 三点全重合兵至 anchor→ 退化方向
    vec2 AB = normalize(ABraw);
    vec2 BC = normalize(BCraw);
    vec2 tangent = normalize(AB + BC);
    vec2 miter = vec2(-tangent.y, tangent.x);
    vec2 normalA = vec2(-AB.y, AB.x);
    float d = dot(miter, normalA);
    float cutoff = 1.0 / max(uMiterLimit, 1.0);
    float miterLength = 1.0 / max(d, cutoff);
    // 偏移在像素空间 = side * miter * halfWidthPx * miterLength；回 NDC 需乘 2/viewport；再乘 w 回 clip
    vec2 offsetNdc = aSide * miter * uHalfWidthPx * miterLength * (2.0 / uViewport);
    gl_Position.xy = pCS.w * (pC + offsetNdc);
    gl_Position.zw = pCS.zw;
    vColor = aColor;
}
)GLSL";

// ── P2.b 3D 点要素 billboard 顶点着色器（屏幕固定圆，投影驱动单管线）───────────────
// 与 stroke 同源：同一 viewProjRtc 把 RTC 中心投影到 clip，再在屏幕像素空间按 corner 展开。
//   corner=0 的顶点落在圆心，corner=单位方向∈[-1,1] 的顶点沿该方向外扩 uHalfPx 像素 → 圆环。
// 偏移量口径：像素 → NDC 乘 (2.0/uViewport)，再乘 pCS.w 回 clip（同 stroke 恒定像素宽推导）。
// 深度 gl_Position.z 保留中心投影 pCS.z → 开 GL_DEPTH_TEST 后，背面半球的点被地球瓦片深度剔除。
// 中心点在相机背后（w<=eps）无意义：整顶点丢到裁剪外（点无相邻点可塌缩，直接裁掉）。
const char *const kGlobePointVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec2 aCorner;
attribute vec4 aColor;
attribute float aMode;
uniform mat4 uMvp;
uniform vec3 uOffset;
uniform vec3 uAnchor;
uniform float uVecAlt;
uniform vec2 uHalfPx;
uniform vec2 uViewport;
varying vec4 vColor;
void main() {
    vec3 nrm = normalize(aPos + uAnchor);
    vec4 pCS = uMvp * vec4(aPos + uOffset + nrm * (uVecAlt * (1.0 - aMode)), 1.0);
    float eps = 1e-4;
    if (pCS.w < eps) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        vColor = aColor;
        return;
    }
    vec2 pC = pCS.xy / pCS.w;
    vec2 offsetNdc = aCorner * uHalfPx * (2.0 / uViewport);
    gl_Position.xy = pCS.w * (pC + offsetNdc);
    gl_Position.zw = pCS.zw;
    vColor = aColor;
}
)GLSL";

// ── P2.c 3D 点要素图标 billboard 顶点着色器（屏幕固定尺寸纹理四边形，投影驱动单管线）───
// 与 globePoint 同源：同一 viewProjRtc 把 RTC 中心投影到 clip，再在屏幕像素空间按 corner 展开；
// 仅多一条 uv 采样图标纹理。uHalfPx 为图标像素半宽/半高（各向异性），每帧逐层传 → 屏幕恒定尺寸。
// 中心点在相机背后（w<=eps）无意义：整顶点丢到裁剪外。深度 zw 保留 → 开 GL_DEPTH_TEST 后背半球图标被地球剔除。
const char *const kGlobeIconVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec2 aCorner;
attribute vec2 aTexCoord;
attribute vec4 aColor;
attribute float aMode;
uniform mat4 uMvp;
uniform vec3 uOffset;
uniform vec3 uAnchor;
uniform float uVecAlt;
uniform vec2 uHalfPx;
uniform vec2 uViewport;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    vec3 nrm = normalize(aPos + uAnchor);
    vec4 pCS = uMvp * vec4(aPos + uOffset + nrm * (uVecAlt * (1.0 - aMode)), 1.0);
    float eps = 1e-4;
    if (pCS.w < eps) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        vTexCoord = aTexCoord;
        vColor = aColor;
        return;
    }
    vec2 pC = pCS.xy / pCS.w;
    vec2 offsetNdc = aCorner * uHalfPx * (2.0 / uViewport);
    gl_Position.xy = pCS.w * (pC + offsetNdc);
    gl_Position.zw = pCS.zw;
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)GLSL";

// ── 3D 贴地定位标识纹理四边形顶点着色器（透视、无屏幕展开，平行于地球表面）───
// 与 globeIcon（屏幕展开 billboard，恒正对屏幕→ tilt 时变「立面」）不同：本通路顶点已在 CPU 端
// 沿局部切平面基 east/north 铺成贴地矩形并 double 减眼点转 float，直接走 viewProjRtc 透视投影 →
// 图标平铺于地表、指真北（RELATIVE_TO_GLOBE），随相机俯仰自然透视压扁。uMvp 传 viewProjRtc。
const char *const kGlobeFlatIconVertexSrc = R"GLSL(
attribute vec3 aPos;
attribute vec2 aTexCoord;
attribute vec4 aColor;
uniform mat4 uMvp;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    gl_Position = uMvp * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)GLSL";

// 占位顶点：vec2 pos + vec4 color = 6 floats
constexpr int kColorFloatsPerVertex = 6;
// 线顶点：vec2 pos + vec2 miter + vec4 color = 8 floats
constexpr int kLineFloatsPerVertex = 8;
// 纹理单位四边形顶点：vec2 pos + vec2 uv = 4 floats
constexpr int kTexFloatsPerVertex = 4;
// 文本字形顶点：vec2 pos + vec2 uv + vec4 color = 8 floats
constexpr int kTextFloatsPerVertex = 8;
// 图标顶点：vec2 center + vec2 corner + vec2 uv + vec4 color = 10 floats
constexpr int kIconFloatsPerVertex = 10;
// 3D 球面瓦片顶点：vec3 pos + vec2 uv + vec4 color = 9 floats（纹理/占位两通路共用交错缓冲，
// 各自取自己的属性段，免为占位色另建一份网格）
constexpr int kGlobeFloatsPerVertex = 9;
// P2.a 3D 面填充顶点：vec3 pos + vec4 color + float mode = 8 floats
constexpr int kGlobeFillFloatsPerVertex = 8;
// P2.a 3D 描边/线顶点：vec3 prev + vec3 cur + vec3 next + float side + vec4 color + float mode = 15 floats
constexpr int kGlobeStrokeFloatsPerVertex = 15;
// P2.b 3D 点 billboard 顶点：vec3 pos + vec2 corner + vec4 color + float mode = 10 floats
constexpr int kGlobePointFloatsPerVertex = 10;
// P2.c 3D 图标 billboard 顶点：vec3 pos + vec2 corner + vec2 uv + vec4 color + float mode = 12 floats
constexpr int kGlobeIconFloatsPerVertex = 12;
// 3D 贴地定位标识图标顶点：vec3 pos + vec2 uv + vec4 color = 9 floats（pos 已 RTC 减眼点）
constexpr int kGlobeFlatIconFloatsPerVertex = 9;
// 3D 纯色顶点（贴地箭头/引线，复用 globeColorProgram_）：vec3 pos + vec4 color = 7 floats
constexpr int kGlobeColorFloatsPerVertex = 7;
// P2.a 3D 矢量贴球高度上夹（米）：face 与 line/outline 同值。**fill 开 glPolygonOffset(+2,+2) 把面深度 push 更远**，
// 保证同高度的 stroke 后画时能通过 GL_LEQUAL（否则 interior 描边会被自己那层 fill 挡回。—见 Pass 2 注释）；
// fill 与瓦片（alt=0）的深度差恒大于 2 units 偏移量级，不会反过来被瓦片吃掉。
// 抬升不可为 0：g2c 只变换顶点，线段/三角形在球上是平弦，中段陷入球面以下的「矢高」≈ L²/(8R)
//（L=顶点间大圆距）。动态高度按 camAlt²/2e7 取（≈ 视角内最长弦 L≈1.4·camAlt 的矢高需求），
// 钳 [2, 上夹]：低空（tilt/贴地视角的惯用工作高度）仅米级，视觉即贴地——旧线性 camAlt·0.1
// 在 camAlt≥5km 即触顶 500m，tilt 后视差使贴地层明显「悬浮」，故改回贴地口径；
// 深视角（camAlt≥100km）仍须百米级抬升容纳百公里跨径弦切。
// 边跨径远超视野的超大要素（如低空视角下的国界）弦中点矢高会超出动态高度而被深度剔除，
// 需配套 CPU 边细分（P2.e）；当前林业调查数据集无此类要素，真机 tilt 验证无缺块回归。
constexpr double kGlobeVecAltMeters = 500.0;
// #10 大气/光照：3D 球面瓦片环境光底（Lambert 漫反射之外的恒定亮度，保证夜侧不全黑、保留纹理可读）
constexpr float kGlobeAmbient = 0.25f;
// P2.a 3D 描边/线 miter 长度上限制（与 2D VectorBuilder 中 kMiterLimit 同口径，尖角钳制）
constexpr float kGlobeMiterLimit = 4.0f;
// 球面瓦片网格每边分段数（G×G 四边格 → 每瓦片 2·G·(G+1) 顶点）：G=8 时根瓦片（90°跨径）
// 棱弦矢高约 6km（球半径的 0.1%，球视轮廓不可见），近地视角瓦片跨径极小更无感；
// 逐帧 CPU 开销≈每瓦片 144 次三角函数，200 瓦片内毫秒级（后续可按 (z,x,y) 缓存网格优化）
constexpr int kGlobeGridDiv = 8;

// 单位四边形 [0,1]x[0,1]（pos 与 uv 相同：uv.v=0 对应世界北边=图像顶行），2 个三角形
const float kUnitQuad[] = {
    0.0f, 0.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 1.0f, 0.0f,
    1.0f, 1.0f, 1.0f, 1.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    1.0f, 1.0f, 1.0f, 1.0f,
    0.0f, 1.0f, 0.0f, 1.0f,
};

inline void pushColorVertex(std::vector<float> &v, float x, float y, float r, float g, float b) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(1.0f);
}

// 带 alpha 的占位顶点（矢量点要素屏幕固定圆用，颜色含透明度）
inline void pushColorVertexA(std::vector<float> &v, float x, float y, float r, float g, float b, float a) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(a);
}

// 文本字形顶点 [x,y,u,v,r,g,b,a]（pos 为相机相对世界坐标，uv 为图集子矩形）
inline void pushTextVert(std::vector<float> &v, float x, float y, float u, float t,
                         float r, float g, float b, float a) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(u);
    v.push_back(t);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(a);
}

// 共享落位：把已排版 lay 的字形四边形发射到 verts。坐标系为任一「y 向下」的 2D 空间（2D=世界单位、
// 3D=屏幕像素）：(ax,ay) 为锚点（含 RTC/轮廓偏移，已叠加），s 为该空间「单位/烘焙像素」比例。
// 水平居中于锚点，基线 = ay + vCenterPx*s 使视觉中心对齐锚点（y 向下→基线在锚点下方，同 2D 口径）。
inline void emitLabelQuads(const VectorLayer::LabelLayout &lay, float ax, float ay, float s,
                           float vCenterPx, float cr, float cg, float cb, float ca,
                           std::vector<float> &verts) {
    float penX = ax - lay.totalAdv * s * 0.5f;
    const float baseline = ay + vCenterPx * s;
    for (const auto &gb : lay.boxes) {
        if (gb.x1 > gb.x0) {
            const float x0 = penX + gb.x0 * s, x1 = penX + gb.x1 * s;
            const float y0 = baseline + gb.y0 * s, y1 = baseline + gb.y1 * s;
            pushTextVert(verts, x0, y0, gb.u0, gb.v0, cr, cg, cb, ca);
            pushTextVert(verts, x1, y0, gb.u1, gb.v0, cr, cg, cb, ca);
            pushTextVert(verts, x1, y1, gb.u1, gb.v1, cr, cg, cb, ca);
            pushTextVert(verts, x0, y0, gb.u0, gb.v0, cr, cg, cb, ca);
            pushTextVert(verts, x1, y1, gb.u1, gb.v1, cr, cg, cb, ca);
            pushTextVert(verts, x0, y1, gb.u0, gb.v1, cr, cg, cb, ca);
        }
        penX += gb.adv * s;
    }
}

// 图标顶点 [cx,cy,mx,my,u,v,r,g,b,a]：中心 + 角点属性∈[-1,1] + uv + 色（着色器按角点×uHalf 展开）
inline void pushIconVert(std::vector<float> &v, float cx, float cy, float mx, float my,
                         float u, float t, float r, float g, float b, float a) {
    v.push_back(cx);
    v.push_back(cy);
    v.push_back(mx);
    v.push_back(my);
    v.push_back(u);
    v.push_back(t);
    v.push_back(r);
    v.push_back(g);
    v.push_back(b);
    v.push_back(a);
}

// P2.a 3D 面填充顶点 [x,y,z,r,g,b,a,mode]（位置已在 CPU 端 worldXYToCartesian(逐顶点高程) 后减 anchor、
// double 差后转 float；mode 0/1 门控着色器 uVecAlt 抬升）
inline void pushGlobeFillVert(std::vector<float> &v, float x, float y, float z,
                              float r, float g, float b, float a, float mode) {
    v.push_back(x); v.push_back(y); v.push_back(z);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
    v.push_back(mode);
}

// P2.a 3D 描边/线顶点 [pPrev3, pCur3, pNext3, side, color4, mode]（同 参考实现 3-顶点模板， RTC 已减 eye）
inline void pushGlobeStrokeVert(std::vector<float> &v,
                                const Vec3 &prev, const Vec3 &cur, const Vec3 &next, float side,
                                float r, float g, float b, float a, float mode) {
    v.push_back(prev.x); v.push_back(prev.y); v.push_back(prev.z);
    v.push_back(cur.x);  v.push_back(cur.y);  v.push_back(cur.z);
    v.push_back(next.x); v.push_back(next.y); v.push_back(next.z);
    v.push_back(side);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
    v.push_back(mode);
}

// P2.b 3D 点 billboard 顶点 [pos3, corner2, color4, mode]（pos 已 RTC 减 eye；corner 为单位圆方向或 (0,0)）
inline void pushGlobePointVert(std::vector<float> &v, const Vec3 &pos, float cx, float cy,
                               float r, float g, float b, float a, float mode) {
    v.push_back(pos.x); v.push_back(pos.y); v.push_back(pos.z);
    v.push_back(cx); v.push_back(cy);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
    v.push_back(mode);
}

// P2.c 3D 图标 billboard 顶点 [pos3, corner2, uv2, color4, mode]（pos 已 RTC 减 eye；corner 为四边形角点∈[-1,1]）
inline void pushGlobeIconVert(std::vector<float> &v, const Vec3 &pos, float cx, float cy,
                              float u, float t, float r, float g, float b, float a, float mode) {
    v.push_back(pos.x); v.push_back(pos.y); v.push_back(pos.z);
    v.push_back(cx); v.push_back(cy);
    v.push_back(u); v.push_back(t);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
    v.push_back(mode);
}

// 3D 贴地定位标识图标顶点 [pos3, uv2, color4]（pos 传入已 RTC 减 eye 的坐标，内部转 float 上传）
inline void pushGlobeFlatIconVert(std::vector<float> &v, const Vec3 &pos, float u, float t,
                                  float r, float g, float b, float a) {
    v.push_back(static_cast<float>(pos.x)); v.push_back(static_cast<float>(pos.y)); v.push_back(static_cast<float>(pos.z));
    v.push_back(u); v.push_back(t);
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
}

// 3D 纯色顶点 [pos3, color4]（复用 globeColorProgram_；pos 传入已 RTC 减 eye 的坐标，贴地箭头/引线用）
inline void pushGlobeColorVert(std::vector<float> &v, const Vec3 &pos,
                               float r, float g, float b, float a) {
    v.push_back(static_cast<float>(pos.x)); v.push_back(static_cast<float>(pos.y)); v.push_back(static_cast<float>(pos.z));
    v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a);
}

// P2.a 归一化世界坐标 (wx, wy) → 地理 (lon°, lat°)。与 Tessellator::rowToLat 同公式的逆口径：
// lon = wx*360 − 180（wx=0 西边缘 −180°、wx=1 东边缘 +180°）；
// lat = atan(sinh(π(1−2·wy))) 度，wy=0 顶=北 +85.05、wy=1 底=南 −85.05。钳到 ±85.05°。
inline void worldXYToGeographic(double wx, double wy, double &outLonDeg, double &outLatDeg) {
    outLonDeg = wx * 360.0 - 180.0;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kRad2Deg = 180.0 / kPi;
    constexpr double kMaxLat = 85.0511287798066; // = atan(sinh(π)) · rad2deg，与 rowToLat 同口径
    double lat = std::atan(std::sinh(kPi * (1.0 - 2.0 * wy))) * kRad2Deg;
    if (lat > kMaxLat) lat = kMaxLat;
    if (lat < -kMaxLat) lat = -kMaxLat;
    outLatDeg = lat;
}

// P2.a 归一化世界坐标 → ECEF（米）：worldXYToGeographic 后交 Wgs84Globe。alt 传入面/线贴球高度。
inline Vec3 worldXYToCartesian(double wx, double wy, double altMeters) {
    double lon = 0.0, lat = 0.0;
    worldXYToGeographic(wx, wy, lon, lat);
    Vec3 out{0.0, 0.0, 0.0};
    Wgs84Globe::instance().geographicToCartesian(lon, lat, altMeters, out);
    return out;
}

// 定位标记尺寸：屏幕固定 dp（乘屏幕密度得 px），三角扇圆分段数
constexpr int kMarkerSegments = 32;
constexpr double kMarkerDotRadiusDp = 6.0;  // 蓝心半径（dp）
constexpr double kMarkerRingRadiusDp = 9.0; // 白边半径（dp）
// 罗盘图标显示缩放：原主界面 COMPASS_IMAGE_SCALE=0.5f × density，原图 150px；
// 此处按图标原始像素 × 该比例显示，效果对齐主界面（约 75px/density=1）
constexpr double kMarkerIconScale = 0.5;
// 3D 贴地标示的引线屏幕长度（dp，乘 density 得 px）：图标沿地表法向抬高该屏幕长度悬于地面点上方，
// 斜视时显出一条连回地面点的引线（俯视时沿视线坍缩为点、不可见）。对齐 参考实现 leader-line Placemark。
constexpr double kMarkerLeaderLenDp = 100.0;
// 移动方向箭头颜色（淡蓝，对齐原主界面 LocationModel.ARROW_COLOR = 0xFF57ACFD）
constexpr float kArrowR = 0x57 / 255.0f;
constexpr float kArrowG = 0xAC / 255.0f;
constexpr float kArrowB = 0xFD / 255.0f;
// 移动方向箭头几何（dp，乘 density 得 px）——直接移植 0.2.8 主界面 LocationModel.createHeadingArrowIcon：
// 128px 画布 + imageScale 0.5×density（位图坐标 ×0.5 即 dp 尺寸），Path 为凹口导航箭头：
// (center, center-0.16S) 箭尖 → (center±0.14S, center+0.16S) 底角 → (center, center+0.08S) 凹口。
// 箭头以定位点为中心固定屏幕尺寸绘制，不随罗盘图标放大（旧版按图标半宽 1.35/2.2 倍拉伸过大）。
constexpr double kArrowTipDp = 10.24;   // 箭尖到定位点距离（前）= 128×0.16×0.5
constexpr double kArrowBackDp = 10.24;  // 底角到定位点距离（后）
constexpr double kArrowHalfDp = 8.96;   // 底角横向半宽 = 128×0.14×0.5
constexpr double kArrowNotchDp = 5.12;  // 凹口到定位点距离（后）= 128×0.08×0.5

/**
 * 把多条 GL_TRIANGLE_STRIP 段（ranges 为 (first,count) 顶点区间）的 [fromRange,toRange) 切片合批为
 * 单一顶点序列：相邻两段之间插入「上段末顶点 + 本段首顶点」两个重复顶点构成 degenerate 桥，
 * 消除跨段产生的错误填充三角形（段首尾不相连时 GPU 会桥接出跨越三角形，插入退化顶点后
 * 该桥接三角形面积退化为 0 不可见）。未启用面剔除，各段绕序翻转无视觉影响，故无需处理顶点奇偶对齐。
 * 顶点布局为 kLineFloatsPerVertex floats/顶点（[x,y,mx,my,r,g,b,a]）。返回合并后顶点数组。
 * 切片间各自独立成带（首段不加桥、落在不同 chunk VBO），流式上传逐片合批时跨片无粘连。
 */
std::vector<float> buildStripBatch(const std::vector<float> &verts,
                                    const std::vector<std::pair<int, int>> &ranges,
                                    size_t fromRange, size_t toRange) {
    constexpr int fpv = kLineFloatsPerVertex;
    const int total = static_cast<int>(verts.size() / fpv);
    toRange = std::min(toRange, ranges.size());
    // 预留：切片顶点 + 每段接缝至多 2 个重复顶点（fpv floats/顶点）
    std::vector<float> out;
    size_t est = 0;
    for (size_t ri = fromRange; ri < toRange; ++ri)
        if (ranges[ri].second > 0) est += static_cast<size_t>(ranges[ri].second) + 2;
    out.reserve(est * fpv);
    bool first = true;
    for (size_t ri = fromRange; ri < toRange; ++ri) {
        const auto &rg = ranges[ri];
        const int f = rg.first;
        const int c = rg.second;
        if (c <= 0 || f < 0 || f + c > total) continue; // 越界保护
        if (!first) {
            // degenerate 桥：复制 out 中末顶点（上段末）+ 本段首顶点
            const int prevLast = static_cast<int>(out.size() / fpv - 1) * fpv;
            for (int k = 0; k < fpv; ++k) out.push_back(out[prevLast + k]);
            const int curFirst = f * fpv;
            for (int k = 0; k < fpv; ++k) out.push_back(verts[curFirst + k]);
        }
        for (int i = 0; i < c; ++i) {
            const int base = (f + i) * fpv;
            for (int k = 0; k < fpv; ++k) out.push_back(verts[base + k]);
        }
        first = false;
    }
    return out;
}

// 流式上传单组要素的总浮点数预算（三组顶点合计 ≈ 4MB）：按真机 glBufferData 吞吐（≈550MB/4.7s）
// 单组耗时≈十毫秒级，以「每组必成 + 超时即停」的节奏把巨层秒级单帧冻结摊薄到逐帧小卡顿，
// 画面按「整块图斑」一片一片长出（同组 fill/outline/line 同帧落块，无混合态）。
constexpr size_t kUploadChunkFloats = 1u << 20;
// 单层单帧流式上传时间预算：当前进行片必定完成（故每帧至少推进一片），完成后超预算则留到下一帧。
constexpr int kUploadFrameBudgetMs = 6;

/// 以 [data, data+floats) 新建单个 chunk VBO（GL_STATIC_DRAW）；fpv = 每顶点浮点数。仅 GL 线程调用。
VectorLayer::GlChunk uploadVectorChunk(const float *data, size_t floats, int fpv) {
    VectorLayer::GlChunk c;
    if (floats == 0) return c;
    glGenBuffers(1, &c.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(floats * sizeof(float)), data, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    c.vertexCount = static_cast<GLsizei>(floats / fpv);
    return c;
}

/// GPU 拾取 PickID 编码：layerIndex（0-255）占高 8 位，primIndex（0-16M）占低 24 位。
/// 拾取 pass 渲到 RGBA8 FBO，glReadPixels 读回后解码 → pickPrims[primIndex].fid。
inline uint32_t encodePickId(int layerIndex, size_t primIndex) {
    return (static_cast<uint32_t>(layerIndex & 0xFF) << 24) |
           (static_cast<uint32_t>(primIndex) & 0x00FFFFFF);
}
inline void packPickIdRgba(uint32_t id, uint8_t *out) {
    out[0] = static_cast<uint8_t>((id >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((id >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((id >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(id & 0xFF);
}
/// 从 glReadPixels 的 RGBA 解码回 (layerIndex, primIndex)。返回 false 表示背景（全 0）。
inline bool decodePickPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                            int &outLayer, size_t &outPrim) {
    const uint32_t id = (static_cast<uint32_t>(r) << 24) |
                        (static_cast<uint32_t>(g) << 16) |
                        (static_cast<uint32_t>(b) << 8) |
                        static_cast<uint32_t>(a);
    if (id == 0) return false; // 背景（无矢量命中）
    outLayer = static_cast<int>((id >> 24) & 0xFF);
    outPrim = static_cast<size_t>(id & 0x00FFFFFF);
    return true;
}

/// 同 uploadVectorChunk，但额外创建一张逐顶点 RGBA8 PickID VBO（pickIds 为 vertexCount*4 字节）。
VectorLayer::GlChunk uploadVectorChunkWithPickId(const float *data, size_t floats, int fpv,
                                                  const uint8_t *pickIds, size_t pickIdBytes) {
    VectorLayer::GlChunk c = uploadVectorChunk(data, floats, fpv);
    if (c.vertexCount > 0 && pickIds && pickIdBytes >= static_cast<size_t>(c.vertexCount) * 4) {
        glGenBuffers(1, &c.pickIdVbo);
        glBindBuffer(GL_ARRAY_BUFFER, c.pickIdVbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(c.vertexCount * 4),
                     pickIds, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    return c;
}

/// 构建面填充 chunk 的逐顶点 PickID 数组。fillVerts 在 [fStart, fEnd) 内按要素序连续，
/// 每要素的顶点区间由 featSpans[fi] 给出。返回 vertexCount*4 字节 RGBA8 数组。
std::vector<uint8_t> buildFillPickIds(const VectorGeometry &geom, size_t fStart, size_t fEnd,
                                      int layerIndex, size_t firstFeat, size_t nextFeat) {
    const size_t vertCount = (fEnd - fStart) / kColorFloatsPerVertex;
    std::vector<uint8_t> ids(vertCount * 4, 0);
    for (size_t fi = firstFeat; fi < nextFeat; ++fi) {
        const FeatureSpans &s = geom.featSpans[fi];
        const int vStart = (s.fillStart - static_cast<int>(fStart)) / kColorFloatsPerVertex;
        const int vCount = s.fillCount / kColorFloatsPerVertex;
        if (vStart < 0 || vCount <= 0) continue;
        const uint32_t id = encodePickId(layerIndex, fi);
        uint8_t rgba[4];
        packPickIdRgba(id, rgba);
        for (int v = 0; v < vCount; ++v) {
            size_t off = (static_cast<size_t>(vStart) + static_cast<size_t>(v)) * 4;
            if (off + 4 <= ids.size()) {
                ids[off] = rgba[0]; ids[off+1] = rgba[1]; ids[off+2] = rgba[2]; ids[off+3] = rgba[3];
            }
        }
    }
    return ids;
}

/// 构建线/描边 strip batch 的逐顶点 PickID 数组。复刻 buildStripBatch 的顶点布局逻辑
///（含 degenerate 桥复制顶点），每段 range 的顶点取该 range 所属要素的 pickId。
/// 桥复制顶点取背景色（0,0,0,0）——零面积三角形不影响拾取。
/// @param rangeStart  FeatureSpans 中对应 range 的起始字段偏移（&FeatureSpans::outRangeStart 或
///                    &FeatureSpans::lineRangeStart），用于建 range→要素 映射
/// @param rangeCount  对应 range 的计数字段偏移
std::vector<uint8_t> buildStripPickIds(const std::vector<std::pair<int, int>> &ranges,
                                       size_t fromRange, size_t toRange,
                                       const std::vector<FeatureSpans> &featSpans,
                                       size_t featFirst, size_t featNext,
                                       int layerIndex,
                                       int FeatureSpans::*rangeStartField,
                                       int FeatureSpans::*rangeCountField) {
    // 先算总顶点数（复刻 buildStripBatch 的预留逻辑）
    size_t estVerts = 0;
    for (size_t ri = fromRange; ri < toRange; ++ri)
        if (ranges[ri].second > 0) estVerts += static_cast<size_t>(ranges[ri].second) + 2;
    std::vector<uint8_t> ids;
    ids.reserve(estVerts * 4);

    // 预建 range→要素 映射（featSpans 按要素序连续覆盖 range 区间）
    // 用线性扫描：外层要素、内层 range 均按序递增，range 数有限
    size_t curFeat = featFirst;
    auto featForRange = [&](size_t ri) -> size_t {
        while (curFeat < featNext) {
            const FeatureSpans &s = featSpans[curFeat];
            const int start = s.*rangeStartField;
            const int count = s.*rangeCountField;
            if (ri >= static_cast<size_t>(start) &&
                ri < static_cast<size_t>(start) + static_cast<size_t>(count))
                return curFeat;
            // ri 在本要素 range 区间之后 → 推进到下一要素
            if (ri >= static_cast<size_t>(start) + static_cast<size_t>(count))
                ++curFeat;
            else
                break; // ri 在本要素区间之前（不应发生，但防死循环）
        }
        return curFeat < featNext ? curFeat : (featNext > 0 ? featNext - 1 : featFirst);
    };

    bool first = true;
    for (size_t ri = fromRange; ri < toRange; ++ri) {
        const auto &rg = ranges[ri];
        const int c = rg.second;
        if (c <= 0) continue;
        if (!first) {
            // degenerate 桥：2 顶点取背景色（零面积三角形）
            ids.insert(ids.end(), 8, 0);
        }
        const size_t fi = featForRange(ri);
        const uint32_t id = encodePickId(layerIndex, fi);
        uint8_t rgba[4];
        packPickIdRgba(id, rgba);
        for (int v = 0; v < c; ++v) {
            ids.push_back(rgba[0]); ids.push_back(rgba[1]);
            ids.push_back(rgba[2]); ids.push_back(rgba[3]);
        }
        first = false;
    }
    return ids;
}

// ── P2.e 3D 矢量分帧流式上传 + 视锥剔除参数 ──
// 3D 要素组预算按「源(2D RTC) floats」计：转换后 fill 7/6、stroke 14/8 倍膨胀再加桥顶点，
// 0.5M 源 floats ≈ ≤1M 转换 floats ≈ ≤4MB/chunk，单块 glBufferData 十毫秒级、不拆要素。
constexpr size_t kGlobeChunkSrcFloats = 1u << 19;
// 点圆/图标 chunk 每组点数：点圆每点 96 顶点×9f≈3.4KB → 1024 点 ≈3.5MB 上限；图标 6 顶点×11f 远小于此。
constexpr size_t kGlobeChunkPoints = 1024;
// 视锥剔除外扩余量（米）：顶点按 alt=0 烘焙、着色器沿法向抬 uVecAlt≤500m，AABB 不含抬升量，
// 余量覆盖抬升与误差；保守剔除（整盒出平面 ≥ 余量才剔）→ 最坏少剔不误剔。
constexpr double kGlobeCullMarginM = 1000.0;
// 每层每类 chunk 数硬上限（失控安全网：预算逻辑已随几何有界，此处仅防程序错误致 chunk 爆炸）。
constexpr size_t kGlobeMaxChunks = 8192;

/// 3D chunk 可见性：世界 ECEF AABB 对 Navigator 预构的 Gribb 6 面做保守盒测试（与瓦片视锥剔除
/// 同判据：取盒上使 a·x+b·y+c·z 最大的正顶点，其在某平面外侧 → 整盒在外 → 剔）。margin 见常量注释。
inline bool globeChunkVisible(const VectorLayer::GlobeChunk &ch,
                              const std::array<FrustumPlane, 6> &planes) {
    for (const auto &pl : planes) {
        const double px = (pl.a >= 0.0) ? ch.xmax : ch.xmin;
        const double py = (pl.b >= 0.0) ? ch.ymax : ch.ymin;
        const double pz = (pl.c >= 0.0) ? ch.zmax : ch.zmin;
        if (pl.distanceTo(px, py, pz) < -kGlobeCullMarginM) return false;
    }
    return true;
}

/// AABB（double[6]: xmin,xmax,ymin,ymax,zmin,zmax 顺序见 gGlobeAabb*）展开/更新用：并入一个 ECEF 点。
inline void globeAabbExtend(double *bb, const Vec3 &p) {
    if (p.x < bb[0]) bb[0] = p.x;
    if (p.x > bb[1]) bb[1] = p.x;
    if (p.y < bb[2]) bb[2] = p.y;
    if (p.y > bb[3]) bb[3] = p.y;
    if (p.z < bb[4]) bb[4] = p.z;
    if (p.z > bb[5]) bb[5] = p.z;
}

/// 以转换完成的顶点数组新建 3D chunk VBO 并携带其世界 ECEF AABB（bb 序：xmin,xmax,ymin,ymax,zmin,zmax）。
/// 仅 GL 线程调用。
VectorLayer::GlobeChunk uploadGlobeChunk(const std::vector<float> &verts, const double *bb, int fpv) {
    VectorLayer::GlobeChunk c;
    if (verts.empty()) return c;
    glGenBuffers(1, &c.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    c.vertexCount = static_cast<GLsizei>(verts.size() / fpv);
    c.xmin = bb[0]; c.xmax = bb[1]; c.ymin = bb[2]; c.ymax = bb[3]; c.zmin = bb[4]; c.zmax = bb[5];
    return c;
}

/// 新建 AABB 初值（无效盒，首次 Extend 即覆盖）。
inline void globeAabbInit(double *bb) {
    bb[0] = 1e300;  bb[1] = -1e300;
    bb[2] = 1e300;  bb[3] = -1e300;
    bb[4] = 1e300;  bb[5] = -1e300;
}
/// AABB 是否仍为无效盒（无顶点时跳过上传）。
inline bool globeAabbEmpty(const double *bb) { return bb[0] > bb[1]; }

/// 回收某矢量层的全部 chunk VBO 与图标静态 VBO、重置流式上传进度（回到首要素）。
/// [del]=false 仅放弃句柄不 glDelete（GL 上下文已重建、旧句柄失效场景）。仅 GL 线程调用。
void resetVectorUpload(VectorLayer &vl, bool del) {
    auto delAll = [del](std::vector<VectorLayer::GlChunk> &chunks) {
        for (auto &c : chunks) {
            if (del && c.vbo != 0) glDeleteBuffers(1, &c.vbo);
            if (del && c.pickIdVbo != 0) glDeleteBuffers(1, &c.pickIdVbo);
        }
        chunks.clear();
    };
    delAll(vl.fillChunks);
    delAll(vl.outlineChunks);
    delAll(vl.lineChunks);
    if (del && vl.iconVbo != 0) glDeleteBuffers(1, &vl.iconVbo);
    vl.iconVbo = 0;
    vl.iconVertexCount = 0;
    // P2.e 3D chunk 与 2D chunk 同生命周期：新几何接管 / 上下文重建 / 层销毁三条路径统一回收，
    // 游标与完成位重置，下帧据 geom 重新流式上传。del=false 仅放弃句柄（旧上下文失效时 glDelete
    // 无意义且可能误删同名对象）。
    auto delGlobe = [del](std::vector<VectorLayer::GlobeChunk> &chunks) {
        for (auto &c : chunks) if (del && c.vbo != 0) glDeleteBuffers(1, &c.vbo);
        chunks.clear();
    };
    delGlobe(vl.globeFillChunks);
    delGlobe(vl.globeOutlineChunks);
    delGlobe(vl.globeLineChunks);
    delGlobe(vl.globePointChunks);
    delGlobe(vl.globeIconChunks);
    delGlobe(vl.globeExtrudeChunks);
    vl.globeUploadFeat = 0;
    vl.globeUploadPoint = 0;
    vl.globeUploadExtrude = 0;
    vl.globeUploadDone = false;
    vl.uploadFeat = 0;
    vl.uploadDone = false;
    vl.glReady = false;
}

} // namespace

Renderer::Renderer(Navigator &navigator, const std::vector<std::unique_ptr<TileLayer>> &layers,
                   const std::vector<std::unique_ptr<VectorLayer>> &vectorLayers,
                   const std::vector<std::unique_ptr<RasterImageLayer>> &rasterImageLayers)
    : nav_(navigator), layers_(layers), vectorLayers_(vectorLayers), rasterImageLayers_(rasterImageLayers) {}

Renderer::~Renderer() {
    // GL 资源必须在 GL 线程删除，故此处不调用 release()（off-thread 调用 glDelete* 会出错）。
    // 正常路径由 GlobeEngine::releaseGl() 在 GL 线程触发；EGL 上下文销毁时亦会回收。
}

uint64_t Renderer::tileKey(int z, int x, int y) {
    return (static_cast<uint64_t>(z) << 40) |
           (static_cast<uint64_t>(x) << 20) |
           static_cast<uint64_t>(y);
}

void Renderer::handleVectorViewModeChanged() {
    const Navigator::ViewMode currentMode = nav_.viewMode();
    if (vectorViewModeInitialized_ && currentMode == vectorViewMode_) return;

    if (vectorViewModeInitialized_) {
        // @Jin 2D VBO 是 Mercator RTC [x,y,color]，3D VBO 是 ECEF RTC [x,y,z,...]；这里删除两种 chunk，
        // @Jin 后续帧只按当前模式重建，避免切模式后看到线状残片、错误填充或贴地状态跨模式复用。
        for (auto &vlp : vectorLayers_) {
            if (!vlp) continue;
            resetVectorUpload(*vlp, /*del=*/true);
            vlp->globeDrapedTerrainSignature = 0;
            vlp->globeDrapedPendingSignature = 0;
            vlp->globeDrapedSignaturePending = false;
            vlp->globeDrapeCameraMoveInitialized = false;
            vlp->globeDraped = false;
            vlp->globeRemoteLargeDrapeMode = false;
            vlp->globeAnchor = Vec3{0.0, 0.0, 0.0};
            vlp->labelLayouts.clear();
            if (!vlp->geom.empty() && vectorLayerShown(*vlp, nav_.displayLevel())) {
                vlp->uploadStartedAt = std::chrono::steady_clock::now();
                vlp->streamingUpload.store(true);
            } else {
                vlp->streamingUpload.store(false);
            }
        }
        LOGI("[GlobeVec] @Jin view mode changed to %d: vector VBOs reset for 2D/3D layout switch",
             static_cast<int>(currentMode));
    }
    vectorViewMode_ = currentMode;
    vectorViewModeInitialized_ = true;
}

void Renderer::onSurfaceCreated() {
    // EGL 上下文可能是「重建」的：本界面被不透明界面（如设置页）遮挡 → onStop → GL surface 销毁 →
    // EGL 上下文丢失；返回时 surface 重新创建，GLSurfaceView 会再次回调本方法（按 Home 键返回同理）。
    // 旧上下文里的 VBO 名与纹理名在新上下文中已全部失效，必须先丢弃再按「全新上下文」重建，否则崩溃：
    // glBindBuffer 绑到失效 VBO 名会使 ARRAY_BUFFER 绑定退化为 0，glVertexAttribPointer 随之转为读
    // 客户端内存地址 0，glDrawArrays 解引用空指针触发 SIGSEGV，拖垮进程（模拟器表现为整机崩溃需重启）。
    // 纹理缓存直接 clear 即可（TileTexEntry/Texture 析构为 default，不调 glDeleteTextures），后续帧会重新
    // 解码上传（瓦片磁盘缓存仍在，重载很快）；VBO 句柄清零后由下方守卫重新 glGenBuffers 并上传数据。
    // 此处绝不能对旧句柄调 glDelete*——旧上下文已销毁，删除失效名无意义，且可能误删新上下文中的同名对象。
    layerTextures_.clear();
    // @Jin exp1f 的交互 LOD 迟滞只在有效纹理上下文成立；surface 重建后必须重新学习稳定层级。
    lastIdleGlobeTileLevel_ = -1;
    lastIdleGlobeTileMaxLevel_ = -1;
    vboDynamic_ = 0;
    vboUnitQuad_ = 0;
    // 字形图集 GL 纹理属旧上下文：放弃旧句柄（不 glDelete），保留 CPU 图集与字形表；
    // 下一帧据 dirty/invalid 重新上传（与新上下文重建同一口径，避免绑失效纹理名）。
    atlasTex_.abandon();
    markerIconTex_.abandon(); // 罗盘图标纹理属旧上下文：放弃句柄，下一帧据 markerIconRgba_ 重传
    resetRasterImageGl();     // 全幅栅格纹理句柄失效：abandon 后置，下一帧据常驻 CPU rgba 懒重传
    vboText_ = 0;
    vboIcon_ = 0;
    vboGlobeMesh_ = 0; // 3D 网格动态 VBO 属旧上下文：放弃句柄，下帧逐瓦片重传
    // @Jin 瓦片静态 VBO 同样属旧上下文：只放弃 GL 句柄（不能 glDelete，旧上下文已销毁），
    // 清空缓存表，下帧随 acquireTileGlobeMesh 在新上下文重建。
    for (auto &cached : tileGlobeMesh_) cached.second.vbo = 0;
    tileGlobeMesh_.clear();
    // 矢量层 GL 资源同样属于旧上下文，已失效：清零 VBO 句柄 + glReady（保留 CPU 几何），
    // 下一帧据 geom 重新上传（与瓦片纹理/VBO 重建同一口径，避免绑失效 VBO 导致崩溃）。
    resetVectorGl();

    // #9 RHI 抽象层：创建后端实例（默认 GLES2，经编译守卫可选 Vulkan）并初始化。
    // 后端 initialize() 内部查询能力上限（如 GL_MAX_TEXTURE_SIZE），替代散落的直接 GL 查询。
    // 现有 ShaderProgram/Texture 薄封装保留不动，RHI 在更高层抽象状态管理（视口/深度/混合/清除）。
    rhi_ = createRHIBackend();
    rhi_->initialize();

    // 设备纹理边长上限：供 GlobeEngine::addRasterImageLayer 取全幅位图边长帽（超出则 GDAL 沿 overviews 降采样）。
    // 经 RHI 后端查询（GLES2Backend::initialize 内 glGetIntegerv），替代原直接 GL 调用。
    maxTextureSize_ = rhi_->maxTextureSize();
    if (maxTextureSize_ < 1024) maxTextureSize_ = 2048;
    LOGI("Renderer GL_MAX_TEXTURE_SIZE=%d (via RHI:%s)", maxTextureSize_, rhi_->name());

    if (!colorProgram_.init(kColorVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: color shader init failed");
    } else {
        cAPos_ = colorProgram_.attribLocation("aPos");
        cAColor_ = colorProgram_.attribLocation("aColor");
        cUMvp_ = colorProgram_.uniformLocation("uMvp");
        cUOffset_ = colorProgram_.uniformLocation("uOffset");
    }

    if (!lineProgram_.init(kLineVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: line shader init failed");
    } else {
        lAPos_ = lineProgram_.attribLocation("aPos");
        lAMiter_ = lineProgram_.attribLocation("aMiter");
        lAColor_ = lineProgram_.attribLocation("aColor");
        lUMvp_ = lineProgram_.uniformLocation("uMvp");
        lUOffset_ = lineProgram_.uniformLocation("uOffset");
        lUHalfWidthWorld_ = lineProgram_.uniformLocation("uHalfWidthWorld");
    }

    if (!texProgram_.init(kTexVertexSrc, kTexFragmentSrc)) {
        LOGE("Renderer: texture shader init failed");
    } else {
        tAPos_ = texProgram_.attribLocation("aPos");
        tATexCoord_ = texProgram_.attribLocation("aTexCoord");
        tUMvp_ = texProgram_.uniformLocation("uMvp");
        tUTexture_ = texProgram_.uniformLocation("uTexture");
        tUUvOffset_ = texProgram_.uniformLocation("uUvOffset");
        tUUvScale_ = texProgram_.uniformLocation("uUvScale");
    }

    if (!textProgram_.init(kTextVertexSrc, kTextFragmentSrc)) {
        LOGE("Renderer: text shader init failed");
    } else {
        txAPos_ = textProgram_.attribLocation("aPos");
        txATexCoord_ = textProgram_.attribLocation("aTexCoord");
        txAColor_ = textProgram_.attribLocation("aColor");
        txUMvp_ = textProgram_.uniformLocation("uMvp");
        txUTexture_ = textProgram_.uniformLocation("uTexture");
    }

    if (!iconProgram_.init(kIconVertexSrc, kIconFragmentSrc)) {
        LOGE("Renderer: icon shader init failed");
    } else {
        ixAPos_ = iconProgram_.attribLocation("aPos");
        iACorner_ = iconProgram_.attribLocation("aCorner");
        ixATexCoord_ = iconProgram_.attribLocation("aTexCoord");
        ixAColor_ = iconProgram_.attribLocation("aColor");
        ixUMvp_ = iconProgram_.uniformLocation("uMvp");
        ixUTexture_ = iconProgram_.uniformLocation("uTexture");
        ixUOffset_ = iconProgram_.uniformLocation("uOffset");
        ixUHalf_ = iconProgram_.uniformLocation("uHalf");
    }

    if (vboDynamic_ == 0) glGenBuffers(1, &vboDynamic_);

    // GPU 拾取着色器（2D 面填充 + 线/描边两路，片元直接输出 PickID 颜色）
    if (!pickColorProgram_.init(kPickColorVertexSrc, kPickFragmentSrc)) {
        LOGE("Renderer: pick color shader init failed");
    } else {
        pkAPos_ = pickColorProgram_.attribLocation("aPos");
        pkAPickId_ = pickColorProgram_.attribLocation("aPickId");
        pkUMvp_ = pickColorProgram_.uniformLocation("uMvp");
        pkUOffset_ = pickColorProgram_.uniformLocation("uOffset");
    }
    if (!pickLineProgram_.init(kPickLineVertexSrc, kPickFragmentSrc)) {
        LOGE("Renderer: pick line shader init failed");
    } else {
        pkLAPos_ = pickLineProgram_.attribLocation("aPos");
        pkLAMiter_ = pickLineProgram_.attribLocation("aMiter");
        pkLAPickId_ = pickLineProgram_.attribLocation("aPickId");
        pkLUMvp_ = pickLineProgram_.uniformLocation("uMvp");
        pkLUOffset_ = pickLineProgram_.uniformLocation("uOffset");
        pkLUHalfWidthWorld_ = pickLineProgram_.uniformLocation("uHalfWidthWorld");
    }
    // 拾取 FBO 属旧上下文：放弃句柄（与纹理/VBO 同口径，下帧懒重建）
    pickFbo_ = 0;
    pickTex_ = 0;
    pickW_ = 0;
    pickH_ = 0;

    // 3D 球体通路：透视瓦片纹理/占位两程序。片元用带 Lambert + rim glow 的 3D 专用着色器
    //（kGlobeTexFragmentSrc/kGlobeColorFragmentSrc），由 uEye/uSunDir/uAmbient 驱动方向光 + 大气边缘光。
    if (!globeTexProgram_.init(kGlobeTexVertexSrc, kGlobeTexFragmentSrc)) {
        LOGE("Renderer: globe texture shader init failed");
    } else {
        g3APos_ = globeTexProgram_.attribLocation("aPos");
        g3ATexCoord_ = globeTexProgram_.attribLocation("aTexCoord");
        g3UMvp_ = globeTexProgram_.uniformLocation("uMvp");
        g3UTexture_ = globeTexProgram_.uniformLocation("uTexture");
        g3UUvOffset_ = globeTexProgram_.uniformLocation("uUvOffset");
        g3UUvScale_ = globeTexProgram_.uniformLocation("uUvScale");
        g3URtcOffset_ = globeTexProgram_.uniformLocation("uRtcOffset");
    g3UTileCenter_ = globeTexProgram_.uniformLocation("uTileCenter");
        g3USunDir_ = globeTexProgram_.uniformLocation("uSunDir");
        g3UAmbient_ = globeTexProgram_.uniformLocation("uAmbient");
    }
    if (!globeColorProgram_.init(kGlobeColorVertexSrc, kGlobeColorFragmentSrc)) {
        LOGE("Renderer: globe color shader init failed");
    } else {
        gc3APos_ = globeColorProgram_.attribLocation("aPos");
        gc3AColor_ = globeColorProgram_.attribLocation("aColor");
        gc3UMvp_ = globeColorProgram_.uniformLocation("uMvp");
        gc3URtcOffset_ = globeColorProgram_.uniformLocation("uRtcOffset");
    gc3UTileCenter_ = globeColorProgram_.uniformLocation("uTileCenter");
        gc3USunDir_ = globeColorProgram_.uniformLocation("uSunDir");
        gc3UAmbient_ = globeColorProgram_.uniformLocation("uAmbient");
    }
    // P2.a 3D 矢量 fill/stroke 两程序（片元均复用 kColorFragmentSrc）
    if (!globeFillProgram_.init(kGlobeFillVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: globe fill shader init failed");
    } else {
        gfAPos_ = globeFillProgram_.attribLocation("aPos");
        gfAColor_ = globeFillProgram_.attribLocation("aColor");
        gfAMode_ = globeFillProgram_.attribLocation("aMode");
        gfUMvp_ = globeFillProgram_.uniformLocation("uMvp");
        gfUOffset_ = globeFillProgram_.uniformLocation("uOffset");
        gfUAnchor_ = globeFillProgram_.uniformLocation("uAnchor");
        gfUVecAlt_ = globeFillProgram_.uniformLocation("uVecAlt");
    }
    if (!globeStrokeProgram_.init(kGlobeStrokeVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: globe stroke shader init failed");
    } else {
        gsAPrev_ = globeStrokeProgram_.attribLocation("aPrev");
        gsACur_ = globeStrokeProgram_.attribLocation("aCur");
        gsANext_ = globeStrokeProgram_.attribLocation("aNext");
        gsASide_ = globeStrokeProgram_.attribLocation("aSide");
        gsAColor_ = globeStrokeProgram_.attribLocation("aColor");
        gsAMode_ = globeStrokeProgram_.attribLocation("aMode");
        gsUMvp_ = globeStrokeProgram_.uniformLocation("uMvp");
        gsUOffset_ = globeStrokeProgram_.uniformLocation("uOffset");
        gsUAnchor_ = globeStrokeProgram_.uniformLocation("uAnchor");
        gsUVecAlt_ = globeStrokeProgram_.uniformLocation("uVecAlt");
        gsUHalfWidthPx_ = globeStrokeProgram_.uniformLocation("uHalfWidthPx");
        gsUViewportPx_ = globeStrokeProgram_.uniformLocation("uViewport");
        gsUMiterLimit_ = globeStrokeProgram_.uniformLocation("uMiterLimit");
    }
    // P2.b 3D 点要素 billboard 程序（片元复用 kColorFragmentSrc）
    if (!globePointProgram_.init(kGlobePointVertexSrc, kColorFragmentSrc)) {
        LOGE("Renderer: globe point shader init failed");
    } else {
        gpAPos_ = globePointProgram_.attribLocation("aPos");
        gpACorner_ = globePointProgram_.attribLocation("aCorner");
        gpAColor_ = globePointProgram_.attribLocation("aColor");
        gpAMode_ = globePointProgram_.attribLocation("aMode");
        gpUMvp_ = globePointProgram_.uniformLocation("uMvp");
        gpUOffset_ = globePointProgram_.uniformLocation("uOffset");
        gpUAnchor_ = globePointProgram_.uniformLocation("uAnchor");
        gpUVecAlt_ = globePointProgram_.uniformLocation("uVecAlt");
        gpUHalfPx_ = globePointProgram_.uniformLocation("uHalfPx");
        gpUViewportPx_ = globePointProgram_.uniformLocation("uViewport");
    }
    // P2.c 3D 点要素图标 billboard 程序（片元复用 kIconFragmentSrc：纹理×顶点色，保留图标彩色）
    if (!globeIconProgram_.init(kGlobeIconVertexSrc, kIconFragmentSrc)) {
        LOGE("Renderer: globe icon shader init failed");
    } else {
        giaPos_ = globeIconProgram_.attribLocation("aPos");
        giACorner_ = globeIconProgram_.attribLocation("aCorner");
        giATexCoord_ = globeIconProgram_.attribLocation("aTexCoord");
        giAColor_ = globeIconProgram_.attribLocation("aColor");
        giaMode_ = globeIconProgram_.attribLocation("aMode");
        giUMvp_ = globeIconProgram_.uniformLocation("uMvp");
        giUOffset_ = globeIconProgram_.uniformLocation("uOffset");
        giUAnchor_ = globeIconProgram_.uniformLocation("uAnchor");
        giUVecAlt_ = globeIconProgram_.uniformLocation("uVecAlt");
        giUHalfPx_ = globeIconProgram_.uniformLocation("uHalfPx");
        giUViewportPx_ = globeIconProgram_.uniformLocation("uViewport");
        giUTexture_ = globeIconProgram_.uniformLocation("uTexture");
    }
    // 3D 贴地定位标识图标通路（片元复用 kIconFragmentSrc：纹理×顶点色，保留图标彩色；透视无屏幕展开）
    if (!globeFlatIconProgram_.init(kGlobeFlatIconVertexSrc, kIconFragmentSrc)) {
        LOGE("Renderer: globe flat icon shader init failed");
    } else {
        gfiPos_ = globeFlatIconProgram_.attribLocation("aPos");
        gfiTexCoord_ = globeFlatIconProgram_.attribLocation("aTexCoord");
        gfiColor_ = globeFlatIconProgram_.attribLocation("aColor");
        gfiMvp_ = globeFlatIconProgram_.uniformLocation("uMvp");
        gfiTexture_ = globeFlatIconProgram_.uniformLocation("uTexture");
    }
    if (vboGlobeMesh_ == 0) glGenBuffers(1, &vboGlobeMesh_);

    // 文本标注逐帧填充的动态 VBO
    if (vboText_ == 0) glGenBuffers(1, &vboText_);

    // 点要素图标逐帧填充的动态 VBO
    if (vboIcon_ == 0) glGenBuffers(1, &vboIcon_);

    // 静态单位四边形（纹理瓦片复用，逐瓦片仅换模型矩阵与纹理）
    if (vboUnitQuad_ == 0) {
        glGenBuffers(1, &vboUnitQuad_);
        glBindBuffer(GL_ARRAY_BUFFER, vboUnitQuad_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kUnitQuad), kUnitQuad, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glClearColor(0.05f, 0.08f, 0.12f, 1.0f);
    // 纹理上下文的 GL 状态在绘制时按需设置
    LOGI("Renderer::onSurfaceCreated ok");
    // 构建核对：打印手工维护的 GLOBECORE_BUILD_ID（定义见本文件顶部），用于确认装机的是否为本次重建的 HAR。
    LOGI("globecore build id = %s", GLOBECORE_BUILD_ID);
}

void Renderer::onSurfaceChanged(int width, int height) {
    viewportWidth_ = width;
    viewportHeight_ = height;
    // #9 RHI：经后端抽象设置视口（GLES2Backend 内 glViewport）
    if (rhi_) rhi_->setViewport(0, 0, width, height);
    LOGI("Renderer::onSurfaceChanged %d x %d", width, height);
}

Renderer::TileTexEntry &Renderer::getTexture(TexMap &texMap, TileLoader &loader, int z, int x, int y) {
    const uint64_t key = tileKey(z, x, y);
    auto it = texMap.find(key);
    if (it != texMap.end()) {
        it->second.lastUsed = frameTick_; // 命中即刷新 LRU
        // 占位条目（纹理未就绪）幂等重请求：其唯一一次 request 的交付若被 ready_ 高水位丢弃、或遇瞬时
        // 失败、或 EGL 上下文重建后 layerTextures_ 清空引发的一次性全量重请求洪峰中被丢，则该无效占位
        // 既不重请求、又因 lastUsed 每帧刷新而不被 LRU 驱逐 → 永久棋盘、缩放也不自愈。TileLoader::request
        // 以 pending_/failedUntil_ 去重：在途/冷却期为廉价 no-op，交付丢失时重新入队、读盘秒得 → 占位最终填满。
        if (!it->second.tex.isValid()) loader.request(z, x, y);
        return it->second;
    }
    // 缓存未命中：插入 id==0 占位条目并交后台加载器异步取（先读盘后联网）；GL 线程绝不阻塞于磁盘 I/O。
    // 就绪后经 renderLayer 的 drainReady 解码上传；未就绪期间该瓦片走祖先兜底或占位（对齐 参考实现 getTexture 返回 null）。
    // 占位条目一旦插入，后续帧命中即返回，不会对同一瓦片重复请求。
    loader.request(z, x, y);
    TileTexEntry entry;
    entry.lastUsed = frameTick_;
    return texMap.emplace(key, std::move(entry)).first->second;
}

Renderer::TileTexEntry *Renderer::findAncestorEntry(TexMap &texMap, int level, int tx, int ty, int &outAncestorLevel) {
    // 自 level-1 向上逐级查找最近的、纹理有效的祖先瓦片 (L, tx>>k, ty>>k)，k=level-L。
    // tx/ty 已由 visibleTileRange 裁剪到 [0,n-1] 非负，右移安全。
    for (int L = level - 1; L >= 0; --L) {
        const int k = level - L;
        auto found = texMap.find(tileKey(L, tx >> k, ty >> k));
        if (found != texMap.end() && found->second.tex.isValid()) {
            outAncestorLevel = L;
            return &found->second;
        }
    }
    outAncestorLevel = -1;
    return nullptr;
}

void Renderer::evictTileTextures(TexMap &texMap) {
    // 按字节预算淘汰（替代按张数）：累计有效纹理字节，超预算时先驱逐无纹理占位条目（保护已上传纹理），
    // 再按 lastUsed 升序淘汰最久未用的有效纹理，直到总字节降到预算以下。
    // 字节预算与按张数相比：512² 高分瓦片不再因张数少而漏网、256² 旧瓦不再因张数多被误留。
    // 占位优先避免 3D 大范围视图叶瓦片超容量时挤掉成功纹理→miss 重请求→服务限流（HTTP 429）
    // 请求风暴死循环（切 3D 只剩棋盘、切回 2D 也异常的真因）。
    size_t totalBytes = 0;
    for (const auto &kv : texMap) {
        if (kv.second.tex.isValid()) {
            totalBytes += size_t(kv.second.tex.width()) * size_t(kv.second.tex.height()) * 4;
        }
    }
    if (totalBytes <= kMaxTextureBytesPerLayer) return;
    std::vector<std::tuple<int, uint64_t, uint64_t>> order; // (有纹理?1:0, lastUsed, key)
    order.reserve(texMap.size());
    for (const auto &kv : texMap)
        order.emplace_back(kv.second.tex.isValid() ? 1 : 0, kv.second.lastUsed, kv.first);
    std::sort(order.begin(), order.end());
    for (size_t i = 0; i < order.size() && totalBytes > kMaxTextureBytesPerLayer; ++i) {
        auto it = texMap.find(std::get<2>(order[i]));
        if (it == texMap.end()) continue;
        if (it->second.tex.isValid()) {
            totalBytes -= size_t(it->second.tex.width()) * size_t(it->second.tex.height()) * 4;
        }
        it->second.tex.release();
        texMap.erase(it);
    }
}

void Renderer::clearTileTextures() {
    for (auto &texMap : layerTextures_) {
        for (auto &kv : texMap) kv.second.tex.release();
    }
    layerTextures_.clear();
    // @Jin 清空全部瓦片纹理后旧 LOD 不再有对应父瓦兜底，下帧从当前视口重新记录稳定层级。
    lastIdleGlobeTileLevel_ = -1;
    lastIdleGlobeTileMaxLevel_ = -1;
}

void Renderer::discardRemovedLayerTextures() {
    // 逐层消费「待回收」标志（一次性）：只释放被宿主移除那层的纹理，其余层缓存原封不动——
    // 删一个栅格层不再牵动底图（旧口径靠整界面 recreate，GL 上下文销毁 → 全部纹理重解码
    // → 底图先棋盘后瓦片的可见闪动，且 recreate 同时重建矢量层/重读文档，代价远超删除本身）。
    const size_t n = std::min(layers_.size(), layerTextures_.size());
    for (size_t li = 0; li < n; ++li) {
        TileLayer *layer = layers_[li].get(); // 元素可变（容器只 const 引用），exchange 需非 const 原子对象
        if (layer == nullptr || !layer->discardTextures.exchange(false)) continue;
        TexMap &texMap = layerTextures_[li];
        for (auto &kv : texMap) kv.second.tex.release();
        texMap.clear();
        // @Jin 图源切换会丢弃该层全部纹理，同步清空 exp1f LOD 记忆，避免新源沿用旧源手势层级。
        lastIdleGlobeTileLevel_ = -1;
        lastIdleGlobeTileMaxLevel_ = -1;
        LOGI("Renderer discardRemovedLayerTextures layer=%d", static_cast<int>(li));
    }
}

bool Renderer::onDrawFrame() {
    // @Jin 帧首重置共享上传与地形网格预算；手势活动窗口内压低预算，抬手后恢复较高补齐速度。
    frameStartedAt_ = std::chrono::steady_clock::now();
    frameTextureUploads_ = 0;
    frameTextureUploadBytes_ = 0;
    const bool interacting = kExperimentalAdaptiveUploadBudget &&
                          (nav_.isInteracting() || cameraFlightBudgetActive()); // @Jin flyTo 也按手势期限流，避免程序化飞行动画批量上传/重建。
    frameTextureUploadLimit_ = interacting ? kInteractiveTextureUploadTiles : kIdleTextureUploadTiles;
    frameTextureUploadByteLimit_ = interacting ? kInteractiveTextureUploadBytes : kIdleTextureUploadBytes;
    globeMeshRebuildBudget_ = interacting ? kInteractiveMeshRebuildsPerFrame : kIdleMeshRebuildsPerFrame;
    globeMeshRebuildDeferred_ = false; // @Jin 每帧重新累计待补网格，避免上一帧标记让设备持续空转。

    // @Jin 2D/3D 矢量 VBO 的顶点布局不同，模式切换必须先在 GL 线程清理，否则新旧 chunk 会残留到另一通路。
    handleVectorViewModeChanged();

    // 地形注入：先把非 GL 线程排入的 DEM 数据落到 terrainCache_（GL 线程独占写）
    // 置于最前，2D/3D 两条通路都能在同一帧内看到新注入的地形。
    const bool moreTerrain = drainTerrainPending();
    if (nav_.viewMode() == Navigator::ViewMode::MODE_3D) {
        updateNavigatorTerrainFrame();
    } else {
        // @Jin 2D 不消费地形高度；切回 2D 时清零，避免模式共享相机状态带入无效抬升。
        nav_.setTerrainSurfaceHeights(0.0, 0.0);
    }
    // 视图模式分发：3D 球体走独立管线（透视+深度+贴球网格，复用瓦片纹理缓存），2D 正交通路零回归
    // @Jin 还有待应用 DEM 或延后的地形网格时强制再请一帧，保证按需渲染下平滑补齐。
    if (nav_.viewMode() == Navigator::ViewMode::MODE_3D) {
        const bool needRedraw = onDrawFrame3D();
        logTilePipelineStats();
        return needRedraw || moreTerrain;
    }
    // #9 RHI：经后端抽象清除颜色缓冲（GLES2Backend 内 glClear(GL_COLOR_BUFFER_BIT)）
    if (rhi_) rhi_->clear(RHI_CLEAR_COLOR); else glClear(GL_COLOR_BUFFER_BIT);
    if (colorProgram_.program() == 0 || texProgram_.program() == 0) return false;

    ++frameTick_; // LRU 帧标记：本帧用到的纹理 lastUsed 置此值，淘汰时最久未用者先出

    if (layers_.empty()) return false;
    // 每层一份纹理缓存，索引与 layers_ 对齐；addTileLayer 在运行期追加图层时按需扩容
    if (layerTextures_.size() != layers_.size()) layerTextures_.resize(layers_.size());
    // 先回收被移除层的纹理（必须在扩容对齐之后，否则新层尚无 TexMap 可清），再做本帧绘制
    discardRemovedLayerTextures();
    // 帧内层数快照：取两容器较小者为本帧遍历边界（正常路径下两者已对齐；若宿主旁路
    // 绕过 GlobeEngine::drawFrame 的锁调用本函数，至少不会越界引用 layerTextures_ 脏元素）
    const size_t layerCount = std::min(layers_.size(), layerTextures_.size());

    // 相机 → 世界中心与可视半跨度（每帧算一次，各层共享；瓦片级别按各层 maxLevel 分别钳制）
    double cwx = 0.0, cwy = 0.0;
    nav_.worldCenter(cwx, cwy);
    const double hw = nav_.halfWorldWidth();
    const double hh = nav_.halfWorldHeight();

    // 正交投影（相机相对渲染 RTC，对齐 参考实现 globe.offset）：世界可视矩形 → NDC；top<bottom 翻转 y 轴。
    // 所有瓦片坐标先在 double 下减去相机中心 (cwx,cwy) 再转 float，投影对称于原点 [-hw,hw]x[-hh,hh]。
    // 否则高缩放级别下世界坐标（~0.12）与视口半跨度（~1e-5）量级悬殊，float32 有效位被大偏移占满，
    // 瓦片边缘定位误差被 ortho 大比例放大到亚像素级 → 相邻瓦片留缝露出深色清屏形成「黑线」（仅高级别可见）。
    const Matrix4 ortho = Matrix4::ortho(
        static_cast<float>(-hw), static_cast<float>(hw),
        static_cast<float>(hh), static_cast<float>(-hh));

    // 绘制顺序（对齐主界面「注记置顶于矢量之上」）：底图瓦片层（非 overlay）→ 矢量层 →
    // 注记 overlay 层 → 定位标记。任一层仍有资源在途则需再画一帧。
    bool needRedraw = false;
    for (size_t li = 0; li < layerCount; ++li) {
        // 注记 overlay 层留到矢量之后；栅格层虽 overlay=true（alpha 混合）但须与底图同趟画在矢量之前
        if (layers_[li]->overlay && !layers_[li]->raster) continue;
        if (renderLayer(li, cwx, cwy, hw, hh, ortho)) needRedraw = true;
    }
    // 全幅栅格贴图（tif 整幅直读）：与瓦片栅格同层级——底图瓦片趟之后、矢量层之前
    if (drawRasterImages2D(cwx, cwy, ortho)) needRedraw = true;
    // 矢量层：绘制在底图之上、注记之下
    if (drawVectorLayers(cwx, cwy, hw, ortho)) needRedraw = true;
    // 点要素图标：屏幕固定 billboard，绘制在矢量层之上、标注之下
    drawVectorIcons(cwx, cwy, hw, ortho);
    // 矢量要素标注：绘制在矢量层之上、注记 overlay 之下（不影响 needRedraw：字体加载/几何就绪由其它路径驱动重绘）
    drawVectorLabels(cwx, cwy, hw, ortho);
    for (size_t li = 0; li < layerCount; ++li) {
        if (!layers_[li]->overlay || layers_[li]->raster) continue; // 仅注记 overlay 层（栅格已在矢量前绘制）
        if (renderLayer(li, cwx, cwy, hw, hh, ortho)) needRedraw = true;
    }
    // 定位标记（蓝点）叠加在所有图层之上，以屏幕固定尺寸绘制（不影响 needRedraw：其位置更新由外部触发重绘）。
    drawLocationMarker(cwx, cwy, hw, ortho);
    // 快照本帧 2D 相机参数供 renderPickPass2D 复用（免重算）
    lastCwx_ = cwx; lastCwy_ = cwy; lastHw_ = hw; lastOrtho_ = ortho;
    logTilePipelineStats();
    return needRedraw;
}

int Renderer::drainLayerReady(TileLoader &loader, TexMap &texMap) {
    // @Jin 当前图层只能消费跨图层共享预算的剩余部分，避免底图与注记层同帧叠加超限。
    const int countBudget = frameTextureUploadLimit_ - frameTextureUploads_;
    if (countBudget <= 0) return 0;
    const size_t byteBudget = frameTextureUploadByteLimit_ > frameTextureUploadBytes_
                                  ? frameTextureUploadByteLimit_ - frameTextureUploadBytes_
                                  : 0;
    if (byteBudget == 0) return 0;

    std::vector<TileLoader::ReadyTile> ready;
    loader.drainReady(ready, countBudget, byteBudget);
    int uploaded = 0;
    for (auto &rt : ready) {
        if (rt.width <= 0 || rt.height <= 0 || rt.rgba.empty()) continue;
        Texture tex;
        if (!tex.uploadRGBA(rt.rgba.data(), rt.width, rt.height)) continue;
        const size_t bytes = static_cast<size_t>(rt.width) * static_cast<size_t>(rt.height) * 4ULL;
        ++frameTextureUploads_;
        frameTextureUploadBytes_ += bytes;
        ++uploaded;
        TileTexEntry &e = texMap[tileKey(rt.z, rt.x, rt.y)];
        e.tex.release(); // @Jin 先释放旧纹理再替换，避免同 key 更新时短暂占用两份显存。
        e.tex = std::move(tex);
        e.lastUsed = frameTick_;
    }
    return uploaded;
}

void Renderer::logTilePipelineStats() {
    // @Jin 每秒输出一次管线峰值；统计窗口内取最大上传帧而不是最后一帧，便于验证交互期预算是否真正生效。
    const auto now = std::chrono::steady_clock::now();
    const double frameMs = std::chrono::duration<double, std::milli>(now - frameStartedAt_).count();
    if (frameMs > maxFrameDrawMs_) maxFrameDrawMs_ = frameMs;
    if (frameTextureUploads_ > maxFrameTextureUploads_) maxFrameTextureUploads_ = frameTextureUploads_;
    if (frameTextureUploadBytes_ > maxFrameTextureUploadBytes_) {
        maxFrameTextureUploadBytes_ = frameTextureUploadBytes_;
    }
    if (!hasTileStatsLog_) {
        lastTileStatsLog_ = now;
        hasTileStatsLog_ = true;
        return;
    }
    if (now - lastTileStatsLog_ < std::chrono::seconds(1)) return;

    uint64_t processed = 0, cacheHits = 0, networkHits = 0, failures = 0;
    size_t pending = 0, inflight = 0, ready = 0;
    for (const auto &layer : layers_) {
        if (!layer || !layer->loader) continue;
        const TileLoader::StatsSnapshot st = layer->loader->stats();
        processed += st.processed;
        cacheHits += st.cacheHits;
        networkHits += st.networkHits;
        failures += st.failures;
        pending += st.pending;
        inflight += st.inflight;
        ready += st.ready;
    }
    const HttpClient::StatsSnapshot net = HttpClient::stats();
    const uint64_t dProcessed = processed - lastPipeProcessed_;
    const uint64_t dCache = cacheHits - lastPipeCacheHits_;
    const uint64_t dNetwork = networkHits - lastPipeNetworkHits_;
    const uint64_t dFailures = failures - lastPipeFailures_;
    const uint64_t dHttpReq = net.requests - lastHttpRequests_;
    const uint64_t dReused = net.reusedConnections - lastHttpReusedConnections_;
    const uint64_t dNewConn = net.newConnections - lastHttpNewConnections_;
    const uint64_t dHttp2 = net.http2Responses - lastHttp2Responses_;
    const uint64_t dTerrainApplied = terrainApplied_ - lastTerrainApplied_;
    const uint64_t dMeshRebuilds = meshRebuilds_ - lastMeshRebuilds_;
    const uint64_t dPlaceholder = placeholderDraws_ - lastPlaceholderDraws_;
    const uint64_t dAncestor = ancestorTextureDraws_ - lastAncestorTextureDraws_;
    const uint64_t dLeaf = leafTextureDraws_ - lastLeafTextureDraws_;
    const uint64_t dFallbackMesh = terrainFallbackMeshes_ - lastTerrainFallbackMeshes_;
    const bool active = maxFrameTextureUploads_ > 0 || dProcessed > 0 || dHttpReq > 0 ||
                        pending > 0 || inflight > 0 || ready > 0 || terrainDeferred_ > 0 ||
                        dTerrainApplied > 0 || dMeshRebuilds > 0 || dPlaceholder > 0 ||
                        dAncestor > 0 || dLeaf > 0 || dFallbackMesh > 0 || maxFrameDrawMs_ >= 24.0;
    if (active) {
        // @Jin 字段覆盖纹理上传、任务状态、网络复用、地形应用和网格重建，真机 hilog 可直接横向对比版本。
        LOGI("TILEPIPE interact=%d uploads=%d uploadBytes=%zu frameMaxMs=%.2f processed=%llu cache=%llu network=%llu fail=%llu pending=%zu inflight=%zu ready=%zu httpReq=%llu reused=%llu newConn=%llu http2=%llu terrainApplied=%llu terrainDeferred=%zu meshRebuilds=%llu placeholder=%llu ancestor=%llu leaf=%llu fallbackMesh=%llu",
             nav_.isInteracting() ? 1 : 0, maxFrameTextureUploads_, maxFrameTextureUploadBytes_, maxFrameDrawMs_,
             static_cast<unsigned long long>(dProcessed), static_cast<unsigned long long>(dCache),
             static_cast<unsigned long long>(dNetwork), static_cast<unsigned long long>(dFailures),
             pending, inflight, ready, static_cast<unsigned long long>(dHttpReq),
             static_cast<unsigned long long>(dReused), static_cast<unsigned long long>(dNewConn),
             static_cast<unsigned long long>(dHttp2), static_cast<unsigned long long>(dTerrainApplied),
             terrainDeferred_, static_cast<unsigned long long>(dMeshRebuilds),
             static_cast<unsigned long long>(dPlaceholder), static_cast<unsigned long long>(dAncestor),
             static_cast<unsigned long long>(dLeaf), static_cast<unsigned long long>(dFallbackMesh));
    }
    lastPipeProcessed_ = processed;
    lastPipeCacheHits_ = cacheHits;
    lastPipeNetworkHits_ = networkHits;
    lastPipeFailures_ = failures;
    lastHttpRequests_ = net.requests;
    lastHttpReusedConnections_ = net.reusedConnections;
    lastHttpNewConnections_ = net.newConnections;
    lastHttp2Responses_ = net.http2Responses;
    lastTerrainApplied_ = terrainApplied_;
    lastMeshRebuilds_ = meshRebuilds_;
    lastPlaceholderDraws_ = placeholderDraws_;
    lastAncestorTextureDraws_ = ancestorTextureDraws_;
    lastLeafTextureDraws_ = leafTextureDraws_;
    lastTerrainFallbackMeshes_ = terrainFallbackMeshes_;
    lastTileStatsLog_ = now;
    maxFrameDrawMs_ = 0.0;
    maxFrameTextureUploads_ = 0;
    maxFrameTextureUploadBytes_ = 0;
}

bool Renderer::renderLayer(size_t layerIndex, double cwx, double cwy, double hw, double hh, const Matrix4 &ortho) {
    // 双界防护：层数据与纹理缓存任一侧不足即跳过本层（免越界引用脏 TexMap → rehash 非法分配崩溃）
    if (layerIndex >= layers_.size() || layerIndex >= layerTextures_.size()) return false;
    TileLayer &layer = *layers_[layerIndex];
    // 隐藏层整层跳过（对齐 参考实现 Layer.isEnabled=false）：不绘制、不请求瓦片，也不需要因它再画一帧。
    if (!layer.visible) return false;
    TileLoader &loader = *layer.loader;
    TexMap &texMap = layerTextures_[layerIndex];

    // LOD 叶级别：按本层图源 maxLevel 逐层钳制（zoom 超过后恒为 maxLevel，由投影拉伸末级瓦片纹理）。
    const int level = nav_.tileLevel(layer.maxLevel);

    // 可见瓦片行列范围
    int txMin = 0, txMax = -1, tyMin = 0, tyMax = -1;
    TileMatrix::visibleTileRange(cwx, cwy, hw, hh, level, txMin, txMax, tyMin, tyMax);
    if (txMax < txMin || tyMax < tyMin) {
        evictTileTextures(texMap);
        return loader.hasPendingOrReady();
    }
    // @Jin: 先按本帧可见范围建立视口代际，再接收就绪瓦片，防止相机初始化过程中的旧视口结果抢占网络和 GL 上传预算。
    loader.beginView(tileViewSignature2D(level, txMin, txMax, tyMin, tyMax));

    const double n = static_cast<double>(TileMatrix::tilesAtLevel(level));
    const float tileWorld = static_cast<float>(1.0 / n);
    // 纹理瓦片外扩半纹素做重叠（overlap/bleed）：Pass B 用「平移+缩放」的模型矩阵定位，
    // 相邻瓦片的公共边在浮点上不严格相等（x0+tileWorld ≠ float((tx+1)/n)），会留亚像素缝隙
    // 露出深色清屏背景形成「黑边」。外扩后邻块重叠约 1px，彻底盖住缝隙；放大显示下拉伸 <0.5% 不可见。
    const float texelPad = tileWorld * (0.5f / 256.0f);

    // @Jin 将可见叶按屏幕中心距离排序；中心先出图，外围稍后补，主观上比逐块随机出现更接近“整屏快速成型”。
    std::vector<std::pair<int, int>> visibleTiles;
    visibleTiles.reserve(static_cast<size_t>((txMax - txMin + 1) * (tyMax - tyMin + 1)));
    for (int ty = tyMin; ty <= tyMax; ++ty) {
        for (int tx = txMin; tx <= txMax; ++tx) visibleTiles.emplace_back(tx, ty);
    }
    const double centerTx = cwx * n - 0.5;
    const double centerTy = cwy * n - 0.5;
    std::stable_sort(visibleTiles.begin(), visibleTiles.end(),
                     [centerTx, centerTy](const std::pair<int, int> &a, const std::pair<int, int> &b) {
                         // @Jin 使用曼哈顿距离而非平方距离，优先形成以视点为中心的连续矩形覆盖。
                         const double da = std::abs(a.first - centerTx) + std::abs(a.second - centerTy);
                         const double db = std::abs(b.first - centerTx) + std::abs(b.second - centerTy);
                         return da < db;
                     });

    // @Jin 中心优先投递可见叶及其近段祖先链纹理；同一祖先会被去重，队列顺序仍保持中心在前。
    for (int k = kAncestorChainDepth; k >= 1; --k) {
        const int ancestorLevel = level - k;
        if (ancestorLevel < 0) continue;
        for (const auto &tileXY : visibleTiles) {
            getTexture(texMap, loader, ancestorLevel, tileXY.first >> k, tileXY.second >> k);
        }
    }
    // @Jin 粗瓦全部入队后再按中心顺序投递叶子；粗瓦完成后队列自然切到叶子，视觉上由完整低清变高清。
    for (const auto &tileXY : visibleTiles) {
        getTexture(texMap, loader, level, tileXY.first, tileXY.second);
    }
    // @Jin 请求完成后再交付就绪纹理：当前代际优先，且预算由所有图层共享。
    drainLayerReady(loader, texMap);

    // Pass A：占位（无纹理）瓦片批量纯色绘制，保留网格可见性。
    // overlay（注记）层跳过：注记瓦片为带透明 PNG，缺失处应保持透明露出下方底图，不画深色占位块。
    if (!layer.overlay) {
        std::vector<float> verts;
        for (int ty = tyMin; ty <= tyMax; ++ty) {
            const float y0 = static_cast<float>(ty / n - cwy);
            const float y1 = static_cast<float>((ty + 1) / n - cwy);
            for (int tx = txMin; tx <= txMax; ++tx) {
                auto it = texMap.find(tileKey(level, tx, ty));
                if (it != texMap.end() && it->second.tex.isValid()) continue; // 自身有纹理 → Pass B
                int ancLevelA = -1;
                if (findAncestorEntry(texMap, level, tx, ty, ancLevelA) != nullptr) continue; // 有祖先兜底 → Pass B
                const float x0 = static_cast<float>(tx / n - cwx);
                const float x1 = static_cast<float>((tx + 1) / n - cwx);
                const bool even = (((tx + ty) & 1) == 0);
                // @Jin 降低占位棋盘对比度，加载中不再出现明显深浅绿块，减少“逐格蹦出”的主观卡顿感。
                const float r = even ? 0.08f : 0.10f;
                const float g = even ? 0.11f : 0.13f;
                const float b = even ? 0.14f : 0.16f;
                pushColorVertex(verts, x0, y0, r, g, b);
                pushColorVertex(verts, x1, y0, r, g, b);
                pushColorVertex(verts, x1, y1, r, g, b);
                pushColorVertex(verts, x0, y0, r, g, b);
                pushColorVertex(verts, x1, y1, r, g, b);
                pushColorVertex(verts, x0, y1, r, g, b);
            }
        }
        if (!verts.empty()) {
            colorProgram_.use();
            glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
            glUniform2f(cUOffset_, 0.0f, 0.0f); // 瓦片占位顶点已烘焙相机相对坐标，无需 RTC 偏移
            glBindBuffer(GL_ARRAY_BUFFER, vboDynamic_);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                         verts.data(), GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(cAPos_);
            glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                                  reinterpret_cast<const void *>(0));
            glEnableVertexAttribArray(cAColor_);
            glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                                  reinterpret_cast<const void *>(2 * sizeof(float)));
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / kColorFloatsPerVertex));
            glDisableVertexAttribArray(cAPos_);
            glDisableVertexAttribArray(cAColor_);
        }
    }

    // Pass B：纹理瓦片逐瓦片绘制（各自绑定纹理 + 模型矩阵，复用单位四边形）。
    // overlay（注记）层开 alpha 混合，使 PNG 透明区露出下方底图、非透明注记叠加其上（对齐主界面注记置顶）。
    if (layer.overlay) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    texProgram_.use();
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(tUTexture_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vboUnitQuad_);
    glEnableVertexAttribArray(tAPos_);
    glVertexAttribPointer(tAPos_, 2, GL_FLOAT, GL_FALSE, kTexFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(0));
    glEnableVertexAttribArray(tATexCoord_);
    glVertexAttribPointer(tATexCoord_, 2, GL_FLOAT, GL_FALSE, kTexFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(2 * sizeof(float)));

    for (int ty = tyMin; ty <= tyMax; ++ty) {
        const float y0 = static_cast<float>(ty / n - cwy);
        for (int tx = txMin; tx <= txMax; ++tx) {
            const float x0 = static_cast<float>(tx / n - cwx);
            const Matrix4 model = Matrix4::model2D(x0 - texelPad, y0 - texelPad,
                                                   tileWorld + 2.0f * texelPad,
                                                   tileWorld + 2.0f * texelPad);
            const Matrix4 mvp = Matrix4::multiply(ortho, model);
            glUniformMatrix4fv(tUMvp_, 1, GL_FALSE, mvp.data());

            auto it = texMap.find(tileKey(level, tx, ty));
            if (it != texMap.end() && it->second.tex.isValid()) {
                // 自身纹理：UV 恒等映射
                glUniform2f(tUUvOffset_, 0.0f, 0.0f);
                glUniform2f(tUUvScale_, 1.0f, 1.0f);
                it->second.tex.bind();
                glDrawArrays(GL_TRIANGLES, 0, 6);
                continue;
            }
            // 祖先兜底（对齐 参考实现 useAncestorTileTexture）：用最近有效祖先纹理的 UV 子矩形拉伸铺满本子瓦片，
            // 覆盖跨级/加载途中未就绪的瓦片，消除黑缝。子瓦片在祖先内的行列 = (tx,ty) 的低 k 位。
            int ancLevel = -1;
            TileTexEntry *anc = findAncestorEntry(texMap, level, tx, ty, ancLevel);
            if (anc == nullptr) continue; // 无自身也无祖先 → overlay 保持透明 / 底图已在 Pass A 占位
            anc->lastUsed = frameTick_;   // 触碰祖先，避免被 LRU 淘汰
            const int subInt = 1 << (level - ancLevel);
            const float sub = static_cast<float>(subInt);
            const float u0 = static_cast<float>(tx & (subInt - 1)) / sub;
            const float v0 = static_cast<float>(ty & (subInt - 1)) / sub;
            glUniform2f(tUUvOffset_, u0, v0);
            glUniform2f(tUUvScale_, 1.0f / sub, 1.0f / sub);
            anc->tex.bind();
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }

    glDisableVertexAttribArray(tAPos_);
    glDisableVertexAttribArray(tATexCoord_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (layer.overlay) glDisable(GL_BLEND);

    // LRU 淘汰：限制本层持久纹理缓存上限，防止跨级别累积无界增长
    evictTileTextures(texMap);

    // 返回本层是否需再画一帧：后台仍有瓦片排队/下载/就绪待上传时需继续刷新增量补齐。
    return loader.hasPendingOrReady();
}

// ==================== 3D 球体通路（P1） ====================

bool Renderer::onDrawFrame3D() {
    // 清颜色+深度（深度缓冲由 GLSurfaceView 默认 EGLConfig 提供 16bit）；背景仍为深色=太空
    // #9 RHI：经后端抽象清除颜色+深度缓冲
    if (rhi_) rhi_->clear(RHI_CLEAR_COLOR | RHI_CLEAR_DEPTH);
    else      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (globeTexProgram_.program() == 0 || globeColorProgram_.program() == 0) return false;

    ++frameTick_; // 与 2D 共用 LRU 帧标记：切换模式后缓存继续按帧号淘汰

    const Navigator::Camera3D cam = nav_.camera3D();
    if (!cam.valid) return false;

    // 日光跟随相机（默认）：把日下点置于视野中心偏向屏幕上方 30°——e 为相机天底方向（从地心
    // 指向相机），t 为屏幕上方向在地表切平面的投影。视野中心入射角仅 30°（Lambert 亮度
    // cos30≈0.87，始终明亮），斜射又让地形起伏保留明暗层次；背对相机的夜半球自然变暗，
    // 转到哪里哪里是白天。heading 旋转时光在屏幕空间固定（总从屏幕上方斜照），不随方位跳变。
    // 外部显式 setSunDirection 后 sunFollowCamera_=false，改用指定的固定太阳方向。
    if (sunFollowCamera_) {
        const Vec3 e = cam.eye.normalized();
        const Vec3 tRaw = cam.up - e * cam.up.dot(e);
        const double tLen = tRaw.length();
        if (tLen > 1e-9) {
            const double kSunTilt = 0.5235987756; // 30°：日下点相对视野中心的偏角
            sunDir_ = (e * std::cos(kSunTilt) + (tRaw / tLen) * std::sin(kSunTilt)).normalized();
        } else {
            sunDir_ = e; // up 恰与视线方向平行（正视天顶）的退化情形：阳光直射视野中心
        }
    }

    // 绝对 viewProj 仅供 Tessellator 提视锥平面（包围球心为绝对 ECEF）；绘制用 RTC 版：
    // 顶点已在 CPU 端 double 减眼点，若再套含 -s·eye 平移的绝对 view 会二次错位。
    const Matrix4 viewProjAbs = Matrix4::multiply(cam.proj, cam.view);
    const Matrix4 viewRtc = Matrix4::lookAt(Vec3{}, cam.center - cam.eye, cam.up);
    const Matrix4 viewProjRtc = Matrix4::multiply(cam.proj, viewRtc);

    bool needRedraw = false;
    // 帧内层数快照（同 2D 口径）：在扩容对齐后取两容器较小者，Pass A/B2 共用
    size_t layerCount = 0;

    // Pass A：瓦片球面（开深度）——只画底图与栅格 overlay（栅格须垫矢量之下，2D 同口径）；
    // 注记 overlay（非栅格）留到矢量之后画，对齐 2D「注记置顶于矢量之上」（onDrawFrame 分发序）。
    // #9 RHI：经后端抽象开深度测试 + 设比较函数（GLES2Backend 内 glEnable/glDepthFunc）
    if (rhi_) { rhi_->setDepthTest(true); rhi_->setDepthFunc(RHIDepthFunc::LessEqual); }
    else      { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); } // 贴球网格间允许共面覆盖（外扩重叠区后画者胜，免大面片 early-z 拒绘）

    if (!layers_.empty()) {
        if (layerTextures_.size() != layers_.size()) layerTextures_.resize(layers_.size());
        discardRemovedLayerTextures(); // 同 2D：回收被移除层纹理（3D 与 2D 共用同一层纹理缓存）
        layerCount = std::min(layers_.size(), layerTextures_.size());
        for (size_t li = 0; li < layerCount; ++li) {
            if (layers_[li]->overlay && !layers_[li]->raster) continue; // 注记 overlay 留到矢量后
            if (renderGlobeLayer3D(li, cam, viewProjAbs, viewProjRtc)) needRedraw = true;
        }
    }

    // Pass A2：全幅栅格贴图沿经纬网格贴球面（瓦片栅格之后、矢量之前，层级感与 2D 一致）。深度仍开。
    if (drawRasterImages3D(cam, viewProjRtc)) needRedraw = true;

    // Pass B：3D 矢量层（面/描边/线/点/图标），仍开深度、启 polygon offset（实现内局部开关）。
    // drainReady 接管几何，与 2D 共用 loader/glReady 口径；相机未变则绑旧 VBO 直接画。
    if (globeFillProgram_.program() != 0 && globeStrokeProgram_.program() != 0) {
        if (renderGlobeVectors3D(cam, viewProjRtc)) needRedraw = true;
    }

    // Pass B2：注记 overlay 贴球面画在矢量之上（矢量不写深度 + 抬 vecAlt，若先画会被注记的深度测试挡掉）。
    // 本 Pass 内 fill 的 polygonOffset(+2) 已失效——注记与矢量填充共面斜视时的轻度 z-fight 留待后续优化。
    for (size_t li = 0; li < layerCount; ++li) {
        if (!layers_[li]->overlay || layers_[li]->raster) continue; // 仅注记 overlay（栅格已在矢量前绘制）
        if (renderGlobeLayer3D(li, cam, viewProjAbs, viewProjRtc)) needRedraw = true;
    }

    // Pass C：3D 矢量标注（P2.d）——逐锚点投影到屏幕像素后作为纯屏幕叠加（内部关/复开深度），
    // 画在矢量层之上、marker 之下（marker 始终置顶）。
    drawVectorLabels3D(cam, viewProjRtc);

    // Pass D：3D 定位 marker（罗盘图标/蓝点 + 方向箭头）——投影到屏幕像素后作为纯屏幕叠加画在最上层
    // （内部自关/复开深度测试）。
    drawLocationMarker3D(cam, viewProjRtc);

    // #9 RHI：经后端抽象关深度测试（GLES2Backend 内 glDisable(GL_DEPTH_TEST)）
    if (rhi_) rhi_->setDepthTest(false); else glDisable(GL_DEPTH_TEST);
    // @Jin 有网格因单帧预算被延后时必须继续请求下一帧，否则 WHEN_DIRTY 会停在旧地形几何上。
    return needRedraw || globeMeshRebuildDeferred_;
}

bool Renderer::renderGlobeLayer3D(size_t layerIndex, const Navigator::Camera3D &cam,
                                  const Matrix4 &viewProjAbs, const Matrix4 &viewProjRtc) {
    // 双界防护（同 renderLayer）：越界层直接计为无需再画一帧
    if (layerIndex >= layers_.size() || layerIndex >= layerTextures_.size()) return false;
    TileLayer &layer = *layers_[layerIndex];
    if (!layer.visible) return false; // 隐藏层整层跳过（同 2D）
    TileLoader &loader = *layer.loader;
    TexMap &texMap = layerTextures_[layerIndex];

    // 可见叶瓦片：目标级别与 2D LOD 同源（tileLevel(maxLevel) 保切换时地物粗细连续），
    // 更远处（地平线/背面）由 Tessellator 按屏幕空间误差自动取粗级；墨卡托 (z,x,y) 键与 2D 互通。
    const int targetLevel = nav_.tileLevel(layer.maxLevel);
    const bool interacting = kExperimentalAdaptiveUploadBudget &&
                          (nav_.isInteracting() || cameraFlightBudgetActive()); // @Jin flyTo 也按手势期限流，避免程序化飞行动画批量上传/重建。
    // @Jin exp1f：交互期锁定在抬手前的稳定 LOD，不随手指追新高清叶；缩小时允许降到更粗父级，抬手后再细化。
    int quadtreeLevel = targetLevel;
    if (!interacting) {
        lastIdleGlobeTileLevel_ = targetLevel;
        lastIdleGlobeTileMaxLevel_ = layer.maxLevel;
    } else if (lastIdleGlobeTileLevel_ >= 0 && lastIdleGlobeTileMaxLevel_ == layer.maxLevel) {
        quadtreeLevel = std::min(targetLevel, lastIdleGlobeTileLevel_);
    }
    const double tanHalf = std::tan(nav_.fieldOfViewDeg() * 0.5 * 3.14159265358979323846 / 180.0);
    TessCamera tc;
    tc.eye = cam.eye;
    // @Jin 帧间缓存必须同时看到相机朝向：仅比较眼点会让 tilt/heading 后继续复用旧视锥叶集，造成周边黑区。
    tc.forward = cam.forward;
    tc.up = cam.up;
    tc.viewProj = viewProjAbs;
    tc.frustumPlanes = cam.frustumPlanes; // 直接采用 Navigator 预提的 double 平面
    // #11 用带帧间一致性的地理四叉树替代 Tessellator 全量重建：
    // 相机微动时复用上帧叶集（零递归），极冠外推覆盖 85°~90° 空洞。
    // @Jin exp1h：四叉树原先只按椭球面做地平线/视锥剔除，太白山近景大倾角时山脊会被整块误删。
    const double terrainCullMargin = terrainCullMarginM();
    geoQuadtree_.buildVisibleTiles(tc, quadtreeLevel, layer.maxLevel, tanHalf, viewportHeight_,
                                   nav_.lodDetailFactor(), Wgs84Globe::instance(), globeTilesScratch_,
                                   terrainCullMargin);

    // @Jin 3D 可见叶可远超纹理容量；交互期只请求 96 块、空闲期 192 块，避免远景请求风暴触发限流。
    const size_t kGlobeLeafRequestBudget = interacting ? 96U : 192U;
    double focusLon = 0.0, focusLat = 0.0, focusAlt = 0.0;
    Wgs84Globe::instance().cartesianToGeographic(cam.center, focusLon, focusLat, focusAlt);
    // @Jin exp1g：大倾角近景必须按相机三维距离排队；旧口径先按 z、再按屏幕中心经纬距，
    // @Jin 会让屏幕下方离相机最近但略偏中心的叶瓦片排在 192 个请求预算之外并长期画黑。
    globeTilePriorityDistance2_.assign(globeTilesScratch_.size(), 0.0);
    globeTilePriorityOrder_.resize(globeTilesScratch_.size());
    for (size_t i = 0; i < globeTilesScratch_.size(); ++i) {
        const GlobeTile &t = globeTilesScratch_[i];
        const double tLon = (t.lonMin + t.lonMax) * 0.5;
        const double tLat = (t.latMin + t.latMax) * 0.5;
        Vec3 tileCenter{};
        Wgs84Globe::instance().geographicToCartesian(tLon, tLat, sampleTerrainHeight(t, tLon, tLat), tileCenter);
        const Vec3 d = tileCenter - cam.eye;
        globeTilePriorityDistance2_[i] = d.dot(d);
        globeTilePriorityOrder_[i] = i;
    }
    std::stable_sort(globeTilePriorityOrder_.begin(), globeTilePriorityOrder_.end(),
                     [&](size_t ia, size_t ib) {
                         const double da2 = globeTilePriorityDistance2_[ia];
                         const double db2 = globeTilePriorityDistance2_[ib];
                         if (std::fabs(da2 - db2) > 1.0) return da2 < db2;
                         const GlobeTile &a = globeTilesScratch_[ia];
                         const GlobeTile &b = globeTilesScratch_[ib];
                         // @Jin 距离几乎相同（高空正视）时保留高级别、中心优先，兼顾常规全球视角的清晰度。
                         if (a.z != b.z) return a.z > b.z;
                         const double aLon = (a.lonMin + a.lonMax) * 0.5;
                         const double aLat = (a.latMin + a.latMax) * 0.5;
                         const double bLon = (b.lonMin + b.lonMax) * 0.5;
                         const double bLat = (b.latMin + b.latMax) * 0.5;
                         const double aLonDiff = std::min(std::abs(aLon - focusLon), 360.0 - std::abs(aLon - focusLon));
                         const double bLonDiff = std::min(std::abs(bLon - focusLon), 360.0 - std::abs(bLon - focusLon));
                         return (aLonDiff + std::abs(aLat - focusLat)) <
                                (bLonDiff + std::abs(bLat - focusLat));
                     });
    globeTilesReorderedScratch_.clear();
    globeTilesReorderedScratch_.reserve(globeTilesScratch_.size());
    for (size_t index : globeTilePriorityOrder_) globeTilesReorderedScratch_.push_back(globeTilesScratch_[index]);
    globeTilesScratch_.swap(globeTilesReorderedScratch_);
    const size_t leafBudget = std::min(globeTilesScratch_.size(), kGlobeLeafRequestBudget);

    // @Jin 3D 可见叶集稳定排序后再生成签名；同一组瓦片的补帧不清队列，相机移动导致集合变化时推进代际。
    loader.beginView(tileViewSignature3D(globeTilesScratch_));

    // @Jin 3D 请求策略只覆盖预算内叶：先铺祖先粗瓦，再请求自身纹理，预算外远景仅用已有祖先兜底。
    // @Jin 在途去重、ready 高水位和跨图层上传预算共同限制并发、内存与 GL 峰值。
    // @Jin exp1g：高清叶只给前 192 个近景瓦片，但底图最粗的 z-2 祖先要覆盖全部可见叶。
    // @Jin 否则大倾角下预算外近景瓦片既没有自身纹理、也没有父纹理，会静止停留在 placeholder 黑面。
    const size_t coverageAncestorBudget = (!layer.overlay && kAncestorChainDepth >= 2)
                                              ? globeTilesScratch_.size()
                                              : leafBudget;
    for (int k = kAncestorChainDepth; k >= 1; --k) {
        // @Jin 只对最粗祖先做全可见覆盖；z-1 仍受近景预算约束，避免每帧产生大量高清请求。
        const size_t ancestorBudget = (k == kAncestorChainDepth) ? coverageAncestorBudget : leafBudget;
        for (size_t i = 0; i < ancestorBudget; ++i) {
            const GlobeTile &t = globeTilesScratch_[i];
            const int ancestorLevel = t.z - k;
            if (ancestorLevel >= 0) {
                getTexture(texMap, loader, ancestorLevel, t.x >> k, t.y >> k);
            }
        }
    }
    // @Jin 祖先粗瓦入队后再按中心优先顺序请求预算内叶，远景不再逐帧制造无效高清请求。
    for (size_t i = 0; i < leafBudget; ++i) {
        const GlobeTile &t = globeTilesScratch_[i];
        getTexture(texMap, loader, t.z, t.x, t.y);
    }
    // @Jin 3D 与 2D 共用同一帧上传预算；请求完成后再取当前代际结果，避免旧视口结果抢占预算。
    drainLayerReady(loader, texMap);

    // Pass A 占位：无图且无祖先的瓦片画棋盘深色贴球面（首帧/断网时呈现球体轮廓）；
    // overlay（注记）层跳过保持透明（同 2D）。批量累积一次上传一次绘制。
    // @Jin 两个 globe 通路共用：按瓦片静态网格中心 ECEF 设置 RTC 平移（center−eye）与法向锚点 uniform。
    // @Jin 这是静态 VBO 方案下逐瓦片唯一逐帧变化的量，替代原先每顶点 double 减眼点 + 缓冲重传。
    auto setGlobeRtc = [&](GLint locOffset, GLint locCenter, const Renderer::TileGlobeGpuMesh &gm) {
        glUniform3f(locOffset, static_cast<GLfloat>(gm.centerEcef.x - cam.eye.x),
                    static_cast<GLfloat>(gm.centerEcef.y - cam.eye.y),
                    static_cast<GLfloat>(gm.centerEcef.z - cam.eye.z));
        glUniform3f(locCenter, static_cast<GLfloat>(gm.centerEcef.x),
                    static_cast<GLfloat>(gm.centerEcef.y), static_cast<GLfloat>(gm.centerEcef.z));
    };
    if (!layer.overlay) {
        // @Jin 占位瓦片同样走静态 VBO（棋盘色在构建时按 (x+y) 奇偶烘焙）：bind VBO → RTC uniform → draw，
        // 不再每帧拼大缓冲、不再 glBufferData。
        globeColorProgram_.use();
        glUniformMatrix4fv(gc3UMvp_, 1, GL_FALSE, viewProjRtc.data());
        // 光照 uniform（占位与纹理瓦片同口径：uSunDir/uAmbient；法向锚点改逐瓦片 uTileCenter）
        glUniform3f(gc3USunDir_, static_cast<GLfloat>(sunDir_.x), static_cast<GLfloat>(sunDir_.y),
                    static_cast<GLfloat>(sunDir_.z));
        glUniform1f(gc3UAmbient_, kGlobeAmbient);
        const GLsizei strideC = kGlobeFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gc3APos_);
        glEnableVertexAttribArray(gc3AColor_);
        for (const GlobeTile &t : globeTilesScratch_) {
            auto it = texMap.find(tileKey(t.z, t.x, t.y));
            if (it != texMap.end() && it->second.tex.isValid()) continue;
            int ancLevelA = -1;
            if (findAncestorEntry(texMap, t.z, t.x, t.y, ancLevelA) != nullptr) continue;
            ++placeholderDraws_; // @Jin 统计没有自身纹理、也没有祖先纹理的深色占位，用于确认黑块是否仍存在。
            const TileGlobeGpuMesh &gm = acquireTileGlobeMesh(t);
            glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
            glVertexAttribPointer(gc3APos_, 3, GL_FLOAT, GL_FALSE, strideC,
                                  reinterpret_cast<const void *>(0));
            glVertexAttribPointer(gc3AColor_, 4, GL_FLOAT, GL_FALSE, strideC,
                                  reinterpret_cast<const void *>(5 * sizeof(float)));
            setGlobeRtc(gc3URtcOffset_, gc3UTileCenter_, gm);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, gm.vertexCount);
        }
        glDisableVertexAttribArray(gc3APos_);
        glDisableVertexAttribArray(gc3AColor_);
    }

    // Pass B 纹理：逐瓦片生成网格上传绘制（纹理绑定/UV uniform 本就逐瓦片，共享单一动态 VBO）。
    // 自身纹理 UV 恒等；未就绪用最近有效祖先子矩形拉伸兜底（同 2D useAncestorTileTexture）。
    if (layer.overlay) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    globeTexProgram_.use();
    glUniformMatrix4fv(g3UMvp_, 1, GL_FALSE, viewProjRtc.data());
    // 光照 uniform（方向光 + 大气边缘光）：uEye 供顶点求球面法向、uSunDir 为日光方向、uAmbient 为环境光底
    // @Jin 原 uEye 逐帧 uniform 取消：法向锚点/RTC 平移改为逐瓦片 uTileCenter/uRtcOffset。
    glUniform3f(g3USunDir_, static_cast<GLfloat>(sunDir_.x), static_cast<GLfloat>(sunDir_.y),
                static_cast<GLfloat>(sunDir_.z));
    glUniform1f(g3UAmbient_, kGlobeAmbient);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(g3UTexture_, 0);
    // @Jin 不再绑定共享动态 VBO：每瓦片用各自的静态 VBO，attrib 指针在瓦片循环内随 bind 设置。
    const GLsizei stride = kGlobeFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(g3APos_);
    glEnableVertexAttribArray(g3ATexCoord_);
    for (const GlobeTile &t : globeTilesScratch_) {
        TileTexEntry *use = nullptr;
        float u0 = 0.0f, v0 = 0.0f, sc = 1.0f;
        auto it = texMap.find(tileKey(t.z, t.x, t.y));
        if (it != texMap.end() && it->second.tex.isValid()) {
            use = &it->second;
            ++leafTextureDraws_; // @Jin 统计叶子高清纹理绘制数，和祖先/占位计数一起判断 refinement 效果。
        } else {
            int ancLevel = -1;
            TileTexEntry *anc = findAncestorEntry(texMap, t.z, t.x, t.y, ancLevel);
            if (anc == nullptr) continue; // 无图无祖先 → Pass A 已占位 / overlay 保持透明
            ++ancestorTextureDraws_; // @Jin 统计父级粗图拉伸覆盖数；手势中应主要增长该项而不是 placeholder。
            anc->lastUsed = frameTick_;
            const int subInt = 1 << (t.z - ancLevel);
            const float sub = static_cast<float>(subInt);
            u0 = static_cast<float>(t.x & (subInt - 1)) / sub;
            v0 = static_cast<float>(t.y & (subInt - 1)) / sub;
            sc = 1.0f / sub;
            use = anc;
        }
        // @Jin 静态 VBO 命中时零重建、零重传；只有 center−eye 的 3 个 float uniform 随帧变化。
        const TileGlobeGpuMesh &gm = acquireTileGlobeMesh(t);
        glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
        glVertexAttribPointer(g3APos_, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(0));
        glVertexAttribPointer(g3ATexCoord_, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void *>(3 * sizeof(float)));
        setGlobeRtc(g3URtcOffset_, g3UTileCenter_, gm);
        glUniform2f(g3UUvOffset_, u0, v0);
        glUniform2f(g3UUvScale_, sc, sc);
        use->tex.bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, gm.vertexCount);
    }
    glDisableVertexAttribArray(g3APos_);
    glDisableVertexAttribArray(g3ATexCoord_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (layer.overlay) glDisable(GL_BLEND);

    evictTileTextures(texMap);
    return loader.hasPendingOrReady();
}

// ==================== 地形 DEM 通路（quantized-mesh 高度缓存 + 球面网格抬升） ====================

static uint64_t serviceTerrainKeyByBounds(double lonMin, double latMin, double lonMax, double latMax) {
    // @Jin: quantized-mesh 使用 EPSG:4326 Cesium geographic grid：cols=2^(z+1), rows=2^z。
    // @Jin: 注入参数只带 bbox，不带服务 (sx,sy)，这里由瓦片跨度稳定反推服务瓦片号作为全局去重键。
    const double lonSpan = lonMax - lonMin;
    const double latSpan = latMax - latMin;
    if (!(lonSpan > 0.0) || !(latSpan > 0.0)) return 0;
    const int sz = std::max(0, static_cast<int>(std::lround(std::log2(360.0 / lonSpan) - 1.0)));
    const int sx = static_cast<int>(std::lround((lonMin + 180.0) / lonSpan));
    const int sy = static_cast<int>(std::lround((latMin + 90.0) / latSpan));
    return (static_cast<uint64_t>(sz) << 40) |
           (static_cast<uint64_t>(sx) << 20) |
           static_cast<uint64_t>(sy);
}

void Renderer::setTerrainTile(int z, int x, int y, const std::vector<uint8_t> &bytes,
                              double lonMin, double latMin, double lonMax, double latMax) {
    // @Jin 空 bytes 表示单块删除，但当前多级渲染可能共享同一服务 DEM，安全回收由 clearTerrain 统一处理。
    if (bytes.empty()) return;

    const uint64_t key = serviceTerrainKeyByBounds(lonMin, latMin, lonMax, latMax);
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lk(terrainMtx_);
        generation = terrainGeneration_;
        // @Jin 同一服务 DEM 常被多个引擎瓦片引用；解码完成前只登记一次，避免多个 Provider worker 重复栅格化。
        if (key != 0) {
            const auto pendingIt = terrainPendingKeys_.find(key);
            if (pendingIt != terrainPendingKeys_.end()) return;
            terrainPendingKeys_[key] = generation;
        }
    }

    // @Jin quantized-mesh 解码与 buildHeightGrid 均移出 GL 线程；这是本轮实验削掉地形到达长帧的核心。
    QuantizedMeshData decoded = decodeQuantizedMesh(bytes, lonMin, latMin, lonMax, latMax);
    if (!decoded.valid) {
        {
            std::lock_guard<std::mutex> lk(terrainMtx_);
            const auto pendingIt = terrainPendingKeys_.find(key);
            if (pendingIt != terrainPendingKeys_.end() && pendingIt->second == generation) {
                terrainPendingKeys_.erase(pendingIt);
            }
        }
        // @Jin 解码失败只记录告警，不向 GL 线程投递无效对象。
        LOGW("setTerrainTile decode failed z=%d x=%d y=%d bbox=[%.4f,%.4f,%.4f,%.4f]",
             z, x, y, lonMin, latMin, lonMax, latMax);
        return;
    }

    PendingTerrain pt;
    pt.z = z; pt.x = x; pt.y = y;
    pt.key = key;
    pt.generation = generation;
    pt.decoded = std::move(decoded);
    pt.lonMin = lonMin; pt.latMin = latMin; pt.lonMax = lonMax; pt.latMax = latMax;
    {
        std::lock_guard<std::mutex> lk(terrainMtx_);
        // @Jin clearTerrain 后旧 worker 才完成的解码不得回灌，防止关闭地形后又被后台结果点亮。
        if (generation != terrainGeneration_) {
            const auto pendingIt = terrainPendingKeys_.find(key);
            if (pendingIt != terrainPendingKeys_.end() && pendingIt->second == generation) {
                terrainPendingKeys_.erase(pendingIt);
            }
            return;
        }
        terrainPending_.push_back(std::move(pt));
    }
}

bool Renderer::drainTerrainPending() {
    std::vector<PendingTerrain> batch;
    {
        std::lock_guard<std::mutex> lk(terrainMtx_);
        if (terrainPending_.empty()) {
            terrainDeferred_ = 0; // @Jin 队列空时清空延后计数，允许 WHEN_DIRTY 正常停帧。
            return false;
        }
        batch.swap(terrainPending_);
    }

    // @Jin 解码已在线程外完成；GL 线程每帧只移动少量 DEM 到缓存，交互期 2 块、空闲期 4 块，避免移动与 LRU 抖动集中。
    const bool terrainBudgetInteractive = nav_.isInteracting() || cameraFlightBudgetActive(); // @Jin flyTo 与手指操作同口径限制 DEM 注入，避免动画中批量触发地形 VBO 重建。
    int applyBudget = terrainBudgetInteractive ? 2 : 4;
    bool terrainCacheChanged = false; // @Jin 本帧是否新增、清空或淘汰 DEM；只递增一次修订号。
    size_t idx = 0;
    for (; idx < batch.size(); ++idx) {
        PendingTerrain &pt = batch[idx];
        {
            // @Jin terrainGeneration_ 受 terrainMtx_ 保护；锁内同时判断有效性并清理对应去重项。
            std::lock_guard<std::mutex> lk(terrainMtx_);
            if (pt.generation != terrainGeneration_) {
                const auto pendingIt = terrainPendingKeys_.find(pt.key);
                if (pendingIt != terrainPendingKeys_.end() && pendingIt->second == pt.generation) {
                    terrainPendingKeys_.erase(pendingIt);
                }
                continue;
            }
        }
        if (pt.clearAll) {
            terrainCache_.clear();
            terrainLru_.clear();
            terrainCacheChanged = true;
            continue;
        }
        if (applyBudget <= 0) break;

        {
            std::lock_guard<std::mutex> lk(terrainMtx_);
            const auto pendingIt = terrainPendingKeys_.find(pt.key);
            if (pendingIt != terrainPendingKeys_.end() && pendingIt->second == pt.generation) {
                terrainPendingKeys_.erase(pendingIt);
            }
        }
        --applyBudget;
        const auto cached = terrainCache_.find(pt.key);
        if (cached != terrainCache_.end()) {
            // @Jin 同 key 已入库时只刷新 LRU，不替换稳定地址，避免正在重建的网格读取到半更新数据。
            for (auto it = terrainLru_.begin(); it != terrainLru_.end(); ++it) {
                if (*it == pt.key) { terrainLru_.erase(it); break; }
            }
            terrainLru_.push_back(pt.key);
            continue;
        }

        terrainCache_[pt.key] = std::move(pt.decoded);
        terrainLru_.push_back(pt.key);
        ++terrainApplied_;
        terrainCacheChanged = true;
        while (terrainLru_.size() > kMaxTerrainTiles) {
            const uint64_t oldest = terrainLru_.front();
            terrainLru_.pop_front();
            terrainCache_.erase(oldest);
            terrainCacheChanged = true;
        }
    }

    // @Jin 预算用尽后把剩余项按原顺序放回队首，下一帧继续应用；新到任务排在它们之后。
    if (idx < batch.size()) {
        std::vector<PendingTerrain> remain(
            batch.begin() + static_cast<ptrdiff_t>(idx), batch.end());
        std::lock_guard<std::mutex> lk(terrainMtx_);
        remain.insert(remain.end(), terrainPending_.begin(), terrainPending_.end());
        terrainPending_ = std::move(remain);
    }
    if (terrainCacheChanged) ++terrainCacheRevision_;
    {
        std::lock_guard<std::mutex> lk(terrainMtx_);
        terrainDeferred_ = terrainPending_.size();
    }
    return terrainDeferred_ > 0;
}

void Renderer::clearTerrain() {
    // @Jin 清空必须同时废弃队列和 in-flight 解码代际，否则关闭地形后旧 worker 仍可能把 DEM 重新注入。
    PendingTerrain pt;
    {
        std::lock_guard<std::mutex> lk(terrainMtx_);
        ++terrainGeneration_;
        pt.generation = terrainGeneration_;
        pt.clearAll = true;
        terrainPending_.clear();
        terrainPendingKeys_.clear();
        terrainPending_.push_back(std::move(pt));
    }
}

void Renderer::setSunDirection(const Vec3 &dir) {
    // 自动归一化（长度过小时保留默认方向，避免除零产生 NaN 污染所有瓦片亮度）
    const double len = dir.length();
    sunDir_ = len > 1e-9 ? dir / len : Vec3{1.0, 0.0, 0.3};
    // 外部显式指定太阳方向 → 退出「跟随相机」模式，之后 3D 每帧不再用相机位置覆盖 sunDir_。
    sunFollowCamera_ = false;
}

double Renderer::terrainCullMarginM() const {
    float maxHeight = 0.0f;
    for (const auto &entry : terrainCache_) {
        if (entry.second.aboveMax > maxHeight) maxHeight = entry.second.aboveMax;
    }
    if (!(maxHeight > 0.0f)) return 0.0;
    // @Jin 300m 余量覆盖栅格化误差、邻接瓦片和山脊边界；9000m 是实验阶段的全球安全帽。
    return std::min(static_cast<double>(maxHeight) + 300.0, 9000.0);
}

void Renderer::updateNavigatorTerrainFrame() {
    const Camera camera = nav_.camera();
    double targetHeight = 0.0;
    double safetyHeight = 0.0;
    for (const auto &entry : terrainCache_) {
        const QuantizedMeshData &dem = entry.second;
        safetyHeight = std::max(safetyHeight, static_cast<double>(dem.aboveMax) + 300.0);
        if (camera.longitude >= dem.lonMin && camera.longitude <= dem.lonMax &&
            camera.latitude >= dem.latMin && camera.latitude <= dem.latMax) {
            // @Jin 目标点高度必须取真实插值，安全高才使用 DEM 头部的区域最大值。
            targetHeight = std::max(targetHeight, dem.sampleHeight(camera.longitude, camera.latitude));
        }
    }
    safetyHeight = std::min(safetyHeight, 9000.0);
    nav_.setTerrainSurfaceHeights(targetHeight, std::max(targetHeight, safetyHeight));
}

double Renderer::sampleTerrainHeight(const GlobeTile &tile, double lon, double lat) const {
    (void)tile;
    // @Jin: 3D 叶瓦片会随屏幕误差在 z9-z12 等多级 LOD 间切换，而 ArkTS 当前只注入固定引擎级的一批 DEM；
    // @Jin: 因此这里不能再按渲染瓦片自身的 (z,x,y) 精确取缓存，需按经纬度从覆盖该点的 DEM bbox 中采样。
    return sampleTerrainHeightLonLat(lon, lat);
}

double Renderer::sampleTerrainHeightLonLat(double lon, double lat) const {
    for (const auto &entry : terrainCache_) {
        const QuantizedMeshData &dem = entry.second;
        if (lon >= dem.lonMin && lon <= dem.lonMax && lat >= dem.latMin && lat <= dem.latMax) {
            return dem.sampleHeight(lon, lat);
        }
    }
    // @Jin: 没有任何 DEM 精确覆盖该点时返回 0；不能拿任意缓存块钳制外推，否则会把邻区山体贴到错误位置。
    return 0.0;
}

uint64_t Renderer::terrainSignatureForTile(const GlobeTile &tile) const {
    // @Jin 只哈希与该渲染瓦片外扩范围相交的服务 DEM key；无关区域到达新地形时不会触发全场景网格重建。
    const double padLon = (tile.lonMax - tile.lonMin) * 0.0075;
    const double padLat = (tile.latMax - tile.latMin) * 0.0075;
    const double lon0 = tile.lonMin - padLon;
    const double lon1 = tile.lonMax + padLon;
    const double latS = tile.latMin - padLat;
    const double latN = tile.latMax + padLat;
    std::vector<uint64_t> keys;
    keys.reserve(terrainCache_.size());
    for (const auto &entry : terrainCache_) {
        const QuantizedMeshData &dem = entry.second;
        if (!(lon1 < dem.lonMin || lon0 > dem.lonMax || latS > dem.latMax || latN < dem.latMin)) {
            keys.push_back(entry.first);
        }
    }
    if (keys.empty()) return kEmptyTerrainSignature;
    std::sort(keys.begin(), keys.end());
    uint64_t hash = 1469598103934665603ull;
    for (const uint64_t key : keys) {
        hash ^= key;
        hash *= 1099511628211ull;
    }
    return hash;
}

void Renderer::buildGlobeTileMeshLocal(const GlobeTile &tile, std::vector<float> &verts,
                                        Vec3 &centerEcef, bool sampleTerrain,
                                        bool fullResolutionTerrain) const {
    // 包围盒外约 0.75% 外扩：盖住不同级别邻瓦片的 T 形缝隙（深度测试裁决前后，轻微 z-fighting
    // 可接受，同 2D Pass B texelPad 思路）；极地附近经度收敛，外扩仅按跨径比例不改变形状
    const double padLon = (tile.lonMax - tile.lonMin) * 0.0075;
    const double padLat = (tile.latMax - tile.latMin) * 0.0075;
    const double lon0 = tile.lonMin - padLon, lon1 = tile.lonMax + padLon;
    const double latN = tile.latMax + padLat, latS = tile.latMin - padLat;
    const Wgs84Globe &globe = Wgs84Globe::instance();
    // @Jin 占位棋盘色只取决于瓦片 (x+y) 奇偶：烘焙进静态 VBO，Pass A 直接用、Pass B 忽略。
    const bool even = (((tile.x + tile.y) & 1) == 0);
    // @Jin 3D 占位同样改低对比蓝灰，加载中保留球体轮廓但不再出现明显深浅棋盘块。
    const float r = even ? 0.08f : 0.10f;
    const float g = even ? 0.11f : 0.13f;
    const float b = even ? 0.14f : 0.16f;
    // @Jin RTC 锚点取未外扩瓦片中心（alt=0）：VBO 顶点存 ecefVertex-center 的局部坐标，
    // 量级仅几~几十 km，float 精度充足；centerEcef 随 VBO 缓存，绘制时由 uRtcOffset=center-eye 还原。
    globe.geographicToCartesian((tile.lonMin + tile.lonMax) * 0.5,
                                (tile.latMin + tile.latMax) * 0.5, 0.0, centerEcef);
    // @Jin 采样地形时收集相交 DEM；预算不足也允许走 8x8 低保真网格，保证新瓦片首帧可显示且不平贴。
    std::vector<const QuantizedMeshData *> terrainDems;
    if (sampleTerrain) {
        // @Jin 渲染叶可能是 z10-z12，而 DEM 注入层较粗；按地理包围盒相交收集，不要求 (z,x,y) 完全相同。
        for (const auto &entry : terrainCache_) {
            const QuantizedMeshData &dem = entry.second;
            const bool overlap = !(lon1 < dem.lonMin || lon0 > dem.lonMax ||
                                   latS > dem.latMax || latN < dem.latMin);
            if (overlap) terrainDems.push_back(&dem);
        }
    }
    // @Jin 无地形时用 8x8；预算受限时也采样 DEM 但只建 8x8 低分辨率地形，避免零高程后备网格与邻接山体开裂。
    constexpr int kTerrainGridDiv = 24;
    const int G = (sampleTerrain && !terrainDems.empty())
                      ? (fullResolutionTerrain ? kTerrainGridDiv : kGlobeGridDiv)
                      : kGlobeGridDiv;
    // @Jin 几何链路已由真机日志和画面确认生效，先恢复 1.0 真实高程；后续再产品化为 setTerrainExaggeration()。
    constexpr double kTerrainExaggeration = 1.0;
    // @Jin 诊断插桩只统计真实采样网格，后备网格不参与 DEM 命中率口径。
    static std::atomic<uint64_t> s_diagMeshSeq{0};
    const bool diagThis = sampleTerrain && (s_diagMeshSeq.fetch_add(1) % 256 == 0);
    uint64_t dbgHitCount = 0;
    uint64_t dbgSampleCount = 0;
    // @Jin: 上一顶点命中的 DEM 块（仅利用顶点扫描的空间连贯性，循环外声明）。
    const QuantizedMeshData *lastVertexDem = nullptr;
    double dbgAltMin = 1e300, dbgAltMax = -1e300;
    for (int row = 0; row < G; ++row) {
        for (int c = 0; c <= G; ++c) {
            const double u = static_cast<double>(c) / G; // 0=西（lonMin）
            const double lon = lon0 + (lon1 - lon0) * u;
            for (int e = 0; e < 2; ++e) {
                // 逐行 (上,下) 顶点对→ GL_TRIANGLE_STRIP；跨带缠绕方向交替但未开背面剪裁，无影响
                const double v = static_cast<double>(row + e) / G; // 0=北边=图像顶行（同 2D 单位四边形口径）
                const double lat = latN + (latS - latN) * v;
                // @Jin 只在完整地形网格中采样；后备网格直接保持海平面，确保低预算路径足够便宜。
                const QuantizedMeshData *vertexDem = nullptr;
                if (sampleTerrain) {
                    // @Jin 只从精确包含当前顶点的 DEM 采样，并复用上一顶点命中块，把逐顶点选块通常降为 O(1)。
                    if (lastVertexDem != nullptr &&
                        lon >= lastVertexDem->lonMin && lon <= lastVertexDem->lonMax &&
                        lat >= lastVertexDem->latMin && lat <= lastVertexDem->latMax) {
                        vertexDem = lastVertexDem;
                    } else {
                        for (const QuantizedMeshData *candidate : terrainDems) {
                            if (lon >= candidate->lonMin && lon <= candidate->lonMax &&
                                lat >= candidate->latMin && lat <= candidate->latMax) {
                                vertexDem = candidate;
                                break;
                            }
                        }
                        lastVertexDem = vertexDem;
                    }
                    ++dbgSampleCount;
                    if (vertexDem != nullptr) ++dbgHitCount;
                }
                // @Jin 只有地理覆盖命中的 DEM 才能参与采样；缝隙外扩点宁可回 0，也不用相邻块边界值拉伸。
                const double rawAlt = (sampleTerrain && vertexDem) ? vertexDem->sampleHeight(lon, lat) : 0.0;
                if (diagThis && vertexDem) {
                    if (rawAlt < dbgAltMin) dbgAltMin = rawAlt;
                    if (rawAlt > dbgAltMax) dbgAltMax = rawAlt;
                }
                // @Jin: 仅放大几何高度，不改变经纬度和瓦片范围，便于隔离排查“看不到起伏”的视觉原因。
                const double alt = rawAlt * kTerrainExaggeration;
                Vec3 p;
                globe.geographicToCartesian(lon, lat, alt, p);
                // @Jin 存瓦片中心局部坐标（静态、与眼点无关）；double 相减后再转 float 保精度，
                // 眼点平移移到着色器（uRtcOffset），几何因此可跨帧固化在 VBO 中。
                p.x -= centerEcef.x;
                p.y -= centerEcef.y;
                p.z -= centerEcef.z;
                verts.push_back(static_cast<float>(p.x));
                verts.push_back(static_cast<float>(p.y));
                verts.push_back(static_cast<float>(p.z));
                verts.push_back(static_cast<float>(u));
                verts.push_back(static_cast<float>(v));
                verts.push_back(r);
                verts.push_back(g);
                verts.push_back(b);
                verts.push_back(1.0f);
            }
        }
    }
    // @Jin: 每 256 块瓦片输出一条：dem=1 且 alt 跨度>1m 才算“顶点真的被高度置换”。
    if (diagThis) {
        LOGI("TERDIAG mesh z=%d x=%d y=%d demCandidates=%zu hit=%llu/%llu alt=[%.1f,%.1f] tileBox=[%.4f,%.4f,%.4f,%.4f] cacheTiles=%zu",
             tile.z, tile.x, tile.y, terrainDems.size(),
             static_cast<unsigned long long>(dbgHitCount), static_cast<unsigned long long>(dbgSampleCount),
             dbgHitCount > 0U ? dbgAltMin : 0.0, dbgHitCount > 0U ? dbgAltMax : 0.0,
             tile.lonMin, tile.latMin, tile.lonMax, tile.latMax, terrainCache_.size());
    }
}

Renderer::TileGlobeGpuMesh &Renderer::acquireTileGlobeMesh(const GlobeTile &tile) {
    // @Jin 瓦片静态 VBO 缓存只比较当前瓦片覆盖的 DEM 签名；其它区域地形变化不会造成全场景重建尖峰。
    const uint64_t key = tileKey(tile.z, tile.x, tile.y);
    auto it = tileGlobeMesh_.find(key);
    if (it != tileGlobeMesh_.end() && it->second.vbo != 0 &&
        it->second.terrainSignatureRevision == terrainCacheRevision_ && it->second.terrainCurrent) {
        it->second.lastUsed = frameTick_; // @Jin 地形缓存未变时直接命中，不扫描 DEM、不重算顶点、不重传 VBO。
        return it->second;
    }
    // @Jin DEM 缓存修订号变化后才按 bbox 计算局部签名；无关瓦片签名相同，只更新修订号、不重建 VBO。
    const uint64_t currentSignature = (it != tileGlobeMesh_.end() &&
                                       it->second.terrainSignatureRevision == terrainCacheRevision_)
                                          ? it->second.terrainSignature
                                          : terrainSignatureForTile(tile);
    if (it != tileGlobeMesh_.end() && it->second.vbo != 0 && it->second.terrainCurrent &&
        it->second.terrainSignature == currentSignature) {
        it->second.terrainSignatureRevision = terrainCacheRevision_;
        it->second.lastUsed = frameTick_;
        return it->second;
    }

    const bool canBuildTerrainMesh = globeMeshRebuildBudget_ > 0;
    if (it != tileGlobeMesh_.end() && it->second.vbo != 0 && !canBuildTerrainMesh) {
        // @Jin 预算耗尽时保留上一版几何继续绘制，比同帧批量重建 VBO 更能稳定手势帧率。
        it->second.lastUsed = frameTick_;
        globeMeshRebuildDeferred_ = true;
        return it->second;
    }

    TileGlobeGpuMesh &m = tileGlobeMesh_[key];
    std::vector<float> localVerts;
    Vec3 center{0.0, 0.0, 0.0};
    // @Jin 没有预算的新瓦片也先采样已有 DEM，只降低到 8x8；后续帧再按 terrainCurrent=false 升级 24x24。
    const bool buildFullTerrainMesh = canBuildTerrainMesh;
    buildGlobeTileMeshLocal(tile, localVerts, center, true, buildFullTerrainMesh);
    if (localVerts.empty() || localVerts.size() % kGlobeFloatsPerVertex != 0) {
        m.terrainCurrent = false;
        m.lastUsed = frameTick_;
        if (currentSignature != kEmptyTerrainSignature) globeMeshRebuildDeferred_ = true;
        return m;
    }
    if (m.vbo == 0) glGenBuffers(1, &m.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    // @Jin 几何只随覆盖该瓦片的 DEM 集合变化，使用 STATIC_DRAW 让驱动常驻显存。
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(localVerts.size() * sizeof(float)),
                 localVerts.data(), GL_STATIC_DRAW);
    m.vertexCount = static_cast<int>(localVerts.size() / kGlobeFloatsPerVertex);
    m.centerEcef = center;
    m.terrainSignature = currentSignature;
    m.terrainSignatureRevision = terrainCacheRevision_;
    m.terrainCurrent = buildFullTerrainMesh || currentSignature == kEmptyTerrainSignature;
    m.lastUsed = frameTick_;
    ++meshRebuilds_;
    if (!buildFullTerrainMesh && currentSignature != kEmptyTerrainSignature) ++terrainFallbackMeshes_;
    if (buildFullTerrainMesh) --globeMeshRebuildBudget_;
    if (!m.terrainCurrent) globeMeshRebuildDeferred_ = true;

    if (tileGlobeMesh_.size() > kMaxTileGlobeMesh) {
        // @Jin 仅在超过显存上限时做一次 O(N) LRU 扫描，正常浏览不触发。
        auto victim = tileGlobeMesh_.end();
        for (auto jt = tileGlobeMesh_.begin(); jt != tileGlobeMesh_.end(); ++jt) {
            if (jt->first == key) continue;
            if (victim == tileGlobeMesh_.end() || jt->second.lastUsed < victim->second.lastUsed) victim = jt;
        }
        if (victim != tileGlobeMesh_.end()) {
            if (victim->second.vbo != 0) glDeleteBuffers(1, &victim->second.vbo);
            tileGlobeMesh_.erase(victim);
        }
    }
    return tileGlobeMesh_.find(key)->second;
}

void Renderer::setLocationMarker(double lonDeg, double latDeg, bool visible, double headingDeg) {
    std::lock_guard<std::mutex> lock(markerMtx_);
    markerLon_ = lonDeg;
    markerLat_ = latDeg;
    markerVisible_ = visible;
    markerHeading_ = headingDeg;
}

// ==================== 全幅栅格贴图通路（tif 整幅直读为一张纹理，非分瓦） ====================

bool Renderer::prepareRasterImageTex(RasterImageLayer &rl) {
    if (!rl.geomReady) {
        if (!rl.loader) return false;
        RasterImageLoader::Data data;
        if (!rl.loader->drainReady(data)) return false;
        rl.uploading.store(true);
        rl.rgba = std::move(data.rgba);
        rl.w = data.w;
        rl.h = data.h;
        rl.minLon = data.minLon;
        rl.minLat = data.minLat;
        rl.maxLon = data.maxLon;
        rl.maxLat = data.maxLat;
        rl.geomReady = true;
        LOGI("Renderer RasterImage takeover w=%d h=%d extent=[%.6f,%.6f,%.6f,%.6f] path=%s",
             rl.w, rl.h, rl.minLon, rl.minLat, rl.maxLon, rl.maxLat, rl.sourcePath.c_str());
    }
    if (rl.tex.isValid()) {
        rl.uploading.store(false);
        return true;
    }
    if (rl.rgba.empty() || rl.w <= 0 || rl.h <= 0) {
        rl.uploading.store(false);
        return false;
    }
    // 接管即上传（每层一次性、数毫秒级尖峰，同字形图集重传口径）；失败（纹理超限/OOM）保留 rgba，下帧重试
    Texture tex;
    if (!tex.uploadRGBA(rl.rgba.data(), rl.w, rl.h)) {
        rl.uploading.store(false);
        LOGW("Renderer RasterImage upload failed w=%d h=%d maxTex=%d path=%s",
             rl.w, rl.h, maxTextureSize_, rl.sourcePath.c_str());
        return false;
    }
    rl.tex = std::move(tex);
    rl.uploading.store(false);
    return true;
}

bool Renderer::drawRasterImages2D(double cwx, double cwy, const Matrix4 &ortho) {
    bool needRedraw = false;
    std::vector<RasterImageLayer *> drawables;
    for (const auto &ptr : rasterImageLayers_) {
        RasterImageLayer &rl = *ptr;
        if (!rl.visible) continue; // 隐藏层不绘制/不接管/不计入加载态（同瓦片/矢量口径）
        if (prepareRasterImageTex(rl)) {
            drawables.push_back(&rl);
        } else if (rl.loader && rl.loader->hasPendingOrReady()) {
            needRedraw = true; // 后台整幅读取中：续帧直至接管上传
        }
    }
    if (drawables.empty()) return needRedraw;
    if (texProgram_.program() == 0 || vboUnitQuad_ == 0) return needRedraw;

    // alpha 混合恒开：全幅位图四至之外像素 alpha=0（透明），范围内半透明影像亦正确压底图
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    texProgram_.use();
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(tUTexture_, 0);
    glUniform2f(tUUvOffset_, 0.0f, 0.0f); // 整幅即全纹理：UV 恒等
    glUniform2f(tUUvScale_, 1.0f, 1.0f);
    glBindBuffer(GL_ARRAY_BUFFER, vboUnitQuad_);
    glEnableVertexAttribArray(tAPos_);
    glVertexAttribPointer(tAPos_, 2, GL_FLOAT, GL_FALSE, kTexFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(0));
    glEnableVertexAttribArray(tATexCoord_);
    glVertexAttribPointer(tATexCoord_, 2, GL_FLOAT, GL_FALSE, kTexFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(2 * sizeof(float)));
    for (RasterImageLayer *p : drawables) {
        RasterImageLayer &rl = *p;
        double wx0 = 0.0, wy0 = 0.0, wx1 = 0.0, wy1 = 0.0;
        MercatorProjection::lonLatToWorld(rl.minLon, MercatorProjection::clampLatitude(rl.maxLat), wx0, wy0);
        MercatorProjection::lonLatToWorld(rl.maxLon, MercatorProjection::clampLatitude(rl.minLat), wx1, wy1);
        // RTC：double 减相机中心后转 float（同瓦片口径，防高级别下 float 有效位被大偏移占满而错位）
        const Matrix4 model = Matrix4::model2D(
            static_cast<float>(wx0 - cwx), static_cast<float>(wy0 - cwy),
            static_cast<float>(wx1 - wx0), static_cast<float>(wy1 - wy0));
        const Matrix4 mvp = Matrix4::multiply(ortho, model);
        glUniformMatrix4fv(tUMvp_, 1, GL_FALSE, mvp.data());
        rl.tex.bind();
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glDisableVertexAttribArray(tAPos_);
    glDisableVertexAttribArray(tATexCoord_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    return needRedraw;
}

bool Renderer::drawRasterImages3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc) {
    bool needRedraw = false;
    bool setup = false;
    for (const auto &ptr : rasterImageLayers_) {
        RasterImageLayer &rl = *ptr;
        if (!rl.visible) continue;
        if (!prepareRasterImageTex(rl)) {
            if (rl.loader && rl.loader->hasPendingOrReady()) needRedraw = true;
            continue;
        }
        if (!setup) {
            setup = true;
            if (globeTexProgram_.program() == 0 || vboGlobeMesh_ == 0) break; // 着色器/网格 VBO 不可用：跳过绘制
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            globeTexProgram_.use();
            glUniformMatrix4fv(g3UMvp_, 1, GL_FALSE, viewProjRtc.data());
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(g3UTexture_, 0);
            glUniform2f(g3UUvOffset_, 0.0f, 0.0f);
            glUniform2f(g3UUvScale_, 1.0f, 1.0f);
            glBindBuffer(GL_ARRAY_BUFFER, vboGlobeMesh_);
            const GLsizei stride = kGlobeFloatsPerVertex * sizeof(float);
            glEnableVertexAttribArray(g3APos_);
            glVertexAttribPointer(g3APos_, 3, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<const void *>(0));
            glEnableVertexAttribArray(g3ATexCoord_);
            glVertexAttribPointer(g3ATexCoord_, 2, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<const void *>(3 * sizeof(float)));
        }
        globeMeshScratch_.clear();
        buildRasterImageGlobeMesh(rl, cam.eye, globeMeshScratch_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(globeMeshScratch_.size() * sizeof(float)),
                     globeMeshScratch_.data(), GL_DYNAMIC_DRAW);
        rl.tex.bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0,
                     static_cast<GLsizei>(globeMeshScratch_.size() / kGlobeFloatsPerVertex));
    }
    if (setup) {
        glDisableVertexAttribArray(g3APos_);
        glDisableVertexAttribArray(g3ATexCoord_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_BLEND);
    }
    return needRedraw;
}

void Renderer::buildRasterImageGlobeMesh(const RasterImageLayer &rl, const Vec3 &eye,
                                         std::vector<float> &verts) const {
    const double latN = MercatorProjection::clampLatitude(rl.maxLat);
    const double latS = MercatorProjection::clampLatitude(rl.minLat);
    const Wgs84Globe &globe = Wgs84Globe::instance();
    // 贴球网格 + 抬 30m：网格顶点在球面、三角形内部沿弦垂入球体（与瓦片球面网格同量级、数米），
    // 全幅图若也贴 alt=0，其片元深度会大于瓦片 → GL_LEQUAL 下被剔掉整层不可见。抬到两侧弦垂之上
    // 稳定盖住瓦片；地图尺度下 30m 不可辨（低空俯仰视的轻微视差为已知取舍，后续可随高度自适应）。
    constexpr double kAltMeters = 30.0;
    constexpr int G = 16;
    for (int row = 0; row < G; ++row) {
        for (int c = 0; c <= G; ++c) {
            const double u = static_cast<double>(c) / G; // 0=西（minLon）
            const double lon = rl.minLon + (rl.maxLon - rl.minLon) * u;
            for (int e = 0; e < 2; ++e) {
                // 逐行 (上,下) 顶点对 → GL_TRIANGLE_STRIP（同 buildGlobeTileMesh；未开背面剪裁，缠绕无影响）
                const double v = static_cast<double>(row + e) / G; // 0=北边=图像顶行
                const double lat = latN + (latS - latN) * v;
                Vec3 p;
                globe.geographicToCartesian(lon, lat, kAltMeters, p);
                p -= eye; // RTC：double 差后转 float
                verts.push_back(static_cast<float>(p.x));
                verts.push_back(static_cast<float>(p.y));
                verts.push_back(static_cast<float>(p.z));
                verts.push_back(static_cast<float>(u));
                verts.push_back(static_cast<float>(v));
                verts.push_back(1.0f);
                verts.push_back(1.0f);
                verts.push_back(1.0f);
                verts.push_back(1.0f);
            }
        }
    }
}

void Renderer::resetRasterImageGl() {
    // 上下文重建：旧纹理句柄已随上下文失效，abandon（不 glDelete）保留 CPU rgba，下一帧懒重传
    for (const auto &ptr : rasterImageLayers_) ptr->tex.abandon();
}

void Renderer::releaseRasterImageGl() {
    for (const auto &ptr : rasterImageLayers_) ptr->tex.release();
}

void Renderer::setLocationMarkerIcon(std::vector<uint8_t> rgba, int w, int h) {
    // UI 线程写（构造期只写），GL 线程只读（drawLocationMarker），无竞争
    markerIconRgba_ = std::move(rgba);
    markerIconW_ = w;
    markerIconH_ = h;
    markerIconTex_.release(); // 旧纹理失效，下一帧据新 markerIconRgba_ 重传
}

void Renderer::drawLocationMarker(double cwx, double cwy, double hw, const Matrix4 &ortho) {
    bool visible;
    double lon, lat, heading;
    {
        std::lock_guard<std::mutex> lock(markerMtx_);
        visible = markerVisible_;
        lon = markerLon_;
        lat = markerLat_;
        heading = markerHeading_;
    }
    if (!visible || viewportWidth_ <= 0) return;

    // 地理坐标 → 归一化世界坐标，再减去相机中心（与瓦片同一 RTC 口径）
    double mwx = 0.0, mwy = 0.0;
    MercatorProjection::lonLatToWorld(lon, lat, mwx, mwy);
    const float cx = static_cast<float>(mwx - cwx);
    const float cy = static_cast<float>(mwy - cwy);

    // 屏幕固定尺寸：世界单位/像素 = 2*hw / viewportWidth（hw/hh 与视口同比，x/y 一致）；半径按屏幕密度 dp→px
    const double worldPerPx = (2.0 * hw) / viewportWidth_;
    const double density = nav_.displayDensity();
    const float ringR = static_cast<float>(worldPerPx * kMarkerRingRadiusDp * density);
    const float dotR = static_cast<float>(worldPerPx * kMarkerDotRadiusDp * density);
    if (ringR <= 0.0f) return;

    colorProgram_.use();
    glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
    glUniform2f(cUOffset_, 0.0f, 0.0f); // 定位标记顶点已烘焙相机相对坐标，无需 RTC 偏移
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, vboDynamic_);
    glEnableVertexAttribArray(cAPos_);
    glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(0));
    glEnableVertexAttribArray(cAColor_);
    glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                          reinterpret_cast<const void *>(2 * sizeof(float)));

    // 先画罗盘图标（或蓝点白边圆），再画移动方向箭头（箭头在最上层，不被图标遮盖）。
    // 方位角顺时针自北；世界坐标中北=-y、东=+x（wy 向南增长），故方向向量 dir=(sinθ, -cosθ)。
    float iconHalfW = 0.0f, iconHalfH = 0.0f;

    // 有罗盘图标时画纹理四边形替代蓝点（对齐原主界面 LocationModel ic_compass 方式）；
    // 无图标时回退经典蓝点白边圆
    if (!markerIconRgba_.empty() && markerIconW_ > 0 && markerIconH_ > 0) {
        // 懒上传/上下文重建后重传罗盘图标纹理（GL 线程；CPU 像素构造后只读，无竞争）
        if (!markerIconTex_.isValid() &&
            !markerIconTex_.uploadRGBA(markerIconRgba_.data(), markerIconW_, markerIconH_)) {
            // 上传失败回退蓝点
            goto drawBlueDot;
        }
        // 图标屏幕固定尺寸：原始像素 × kMarkerIconScale × density → 世界半宽/半高
        iconHalfW = static_cast<float>(markerIconW_ * kMarkerIconScale * density * worldPerPx * 0.5);
        iconHalfH = static_cast<float>(markerIconH_ * kMarkerIconScale * density * worldPerPx * 0.5);
        if (iconHalfW <= 0.0f || iconHalfH <= 0.0f) goto drawBlueDot;
        // 中心锚点四边形（与 drawVectorIcons 同口径：顶点存中心 + 角点属性，uHalf 每帧传尺寸；
        // 顶点色白=纯图标，v=0 图像顶行 → 正立）。定位标记仅单四边形，仍走动态 vboIcon_。
        std::vector<float> quad;
        quad.reserve(6 * kIconFloatsPerVertex);
        pushIconVert(quad, cx, cy, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        pushIconVert(quad, cx, cy, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        // 切换到 icon 程序绘制罗盘图标（纹理 × 白色 = 纯图标）
        glDisableVertexAttribArray(cAPos_);
        glDisableVertexAttribArray(cAColor_);
        iconProgram_.use();
        glUniformMatrix4fv(ixUMvp_, 1, GL_FALSE, ortho.data());
        glUniform2f(ixUOffset_, 0.0f, 0.0f); // 定位标记中心已烘焙相机相对坐标，无需 RTC 偏移
        glUniform2f(ixUHalf_, iconHalfW, iconHalfH);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(ixUTexture_, 0);
        markerIconTex_.bind();
        glBindBuffer(GL_ARRAY_BUFFER, vboIcon_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(quad.size() * sizeof(float)),
                     quad.data(), GL_DYNAMIC_DRAW);
        const GLsizei istride = kIconFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(ixAPos_);
        glVertexAttribPointer(ixAPos_, 2, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(iACorner_);
        glVertexAttribPointer(iACorner_, 2, GL_FLOAT, GL_FALSE, istride,
                              reinterpret_cast<const void *>(2 * sizeof(float)));
        glEnableVertexAttribArray(ixATexCoord_);
        glVertexAttribPointer(ixATexCoord_, 2, GL_FLOAT, GL_FALSE, istride,
                              reinterpret_cast<const void *>(4 * sizeof(float)));
        glEnableVertexAttribArray(ixAColor_);
        glVertexAttribPointer(ixAColor_, 4, GL_FLOAT, GL_FALSE, istride,
                              reinterpret_cast<const void *>(6 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(ixAPos_);
        glDisableVertexAttribArray(iACorner_);
        glDisableVertexAttribArray(ixATexCoord_);
        glDisableVertexAttribArray(ixAColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
drawBlueDot:
        // 无罗盘图标：回退经典蓝点白边圆（color 程序已绑定、顶点属性已启用）
        auto drawFan = [&](float radius, float r, float g, float b) {
            std::vector<float> verts;
            verts.reserve(static_cast<size_t>(kMarkerSegments + 2) * kColorFloatsPerVertex);
            pushColorVertex(verts, cx, cy, r, g, b); // 扇心
            for (int i = 0; i <= kMarkerSegments; ++i) {
                const float a = static_cast<float>(2.0 * 3.14159265358979 * i / kMarkerSegments);
                pushColorVertex(verts, cx + radius * std::cos(a), cy + radius * std::sin(a), r, g, b);
            }
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                         verts.data(), GL_DYNAMIC_DRAW);
            glDrawArrays(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(verts.size() / kColorFloatsPerVertex));
        };
        drawFan(ringR, 1.0f, 1.0f, 1.0f);     // 白边
        drawFan(dotR, 0.20f, 0.47f, 0.96f);   // 蓝心（≈ #3378F5）
        glDisableVertexAttribArray(cAPos_);
        glDisableVertexAttribArray(cAColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // 移动方向箭头（画在图标/蓝点之上，若 heading>=0）：0.2.8 主界面 createHeadingArrowIcon 同款
    // 凹口箭头（箭尖/两底角/凹口四顶点，两三角形填充），以定位点为中心固定 dp 尺寸，
    // 不随罗盘图标放大（旧版按图标尺寸拉伸导致三角过大）。
    if (heading >= 0.0) {
        const float th = static_cast<float>(heading * 3.14159265358979 / 180.0);
        const float dirX = std::sin(th);
        const float dirY = -std::cos(th);
        const float perpX = std::cos(th);  // 与 dir 垂直（dir·perp = sinθcosθ - cosθsinθ = 0）
        const float perpY = std::sin(th);
        const double dpPx = density * worldPerPx; // dp→px→世界单位
        const float fwd = static_cast<float>(kArrowTipDp * dpPx);   // 箭尖（前）
        const float back = static_cast<float>(kArrowBackDp * dpPx); // 底角（后）
        const float half = static_cast<float>(kArrowHalfDp * dpPx); // 底角横向半宽
        const float notch = static_cast<float>(kArrowNotchDp * dpPx); // 凹口（后）
        const float tipX = cx + dirX * fwd, tipY = cy + dirY * fwd;   // 箭尖
        const float brX = cx - dirX * back + perpX * half,
                    brY = cy - dirY * back + perpY * half;            // 右底角
        const float blX = cx - dirX * back - perpX * half,
                    blY = cy - dirY * back - perpY * half;            // 左底角
        const float nkX = cx - dirX * notch, nkY = cy - dirY * notch; // 凹口
        std::vector<float> arrow;
        arrow.reserve(6 * kColorFloatsPerVertex);
        pushColorVertex(arrow, tipX, tipY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, brX, brY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, nkX, nkY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, tipX, tipY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, nkX, nkY, kArrowR, kArrowG, kArrowB);
        pushColorVertex(arrow, blX, blY, kArrowR, kArrowG, kArrowB);
        // 切回 color 程序（icon 程序或蓝点绘制后可能已切换）
        colorProgram_.use();
        glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
        glUniform2f(cUOffset_, 0.0f, 0.0f);
        glEnableVertexAttribArray(cAPos_);
        glEnableVertexAttribArray(cAColor_);
        glBindBuffer(GL_ARRAY_BUFFER, vboDynamic_);
        glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                              reinterpret_cast<const void *>(0));
        glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, kColorFloatsPerVertex * sizeof(float),
                              reinterpret_cast<const void *>(2 * sizeof(float)));
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(arrow.size() * sizeof(float)),
                     arrow.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(cAPos_);
        glDisableVertexAttribArray(cAColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    glDisable(GL_BLEND);
}

void Renderer::uploadVectorChunks(VectorLayer &vl, int layerIndex) {
    // 要素组分片流式上传：时间预算内循环推进（每组必成 + 超时即停）。按 geom.featSpans 以要素序
    // 累计 fill+描边+线三组总 floats ≈ 预算为一组，整组一次上传（空段跳过）：同一图斑的填充与
    // 描边/线同帧落块 → 上传途中图斑整块整块长出，不再出现「前半要素带白描边、后半只剩淡填充」的混合态。
    // 三组顶点均按要素序连续追加 → 组内区间 = 首要素起点→尾要素终点（中间零长要素自动跨越），
    // 描边/线沿用 buildStripBatch(fromRange,toRange) 切片合批（CPU 合批与上传同组，免整层临时大带）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUploadFrameBudgetMs);
    const VectorGeometry &geom = vl.geom;
    const size_t nSpans = geom.featSpans.size();
    for (;;) {
        if (vl.uploadFeat >= nSpans) {
            vl.uploadDone = true;
            break;
        }
        // 组界 [uploadFeat, next)：总 floats ≈ 预算；至少含一个要素（巨型单要素超预算也自成一组，
        // 单帧多超有限、保证每帧有进展），越预算即停不拆要素。
        const size_t first = vl.uploadFeat;
        size_t next = first;
        size_t floats = 0;
        do {
            const FeatureSpans &s = geom.featSpans[next];
            size_t add = static_cast<size_t>(s.fillCount);
            for (int i = 0; i < s.outRangeCount; ++i)
                add += static_cast<size_t>(geom.outlineRanges[s.outRangeStart + i].second) * kLineFloatsPerVertex;
            for (int i = 0; i < s.lineRangeCount; ++i)
                add += static_cast<size_t>(geom.lineRanges[s.lineRangeStart + i].second) * kLineFloatsPerVertex;
            if (next > first && floats + add > kUploadChunkFloats) break;
            floats += add;
            ++next;
        } while (next < nSpans && floats < kUploadChunkFloats);
        // 三组顶点按要素序连续追加 → 组切片直接取首尾要素区间（零长要素无数据，跨越无害）
        const FeatureSpans &sF = geom.featSpans[first];
        const FeatureSpans &sL = geom.featSpans[next - 1];
        const size_t fStart = static_cast<size_t>(sF.fillStart);
        const size_t fEnd = static_cast<size_t>(sL.fillStart) + static_cast<size_t>(sL.fillCount);
        if (fEnd > fStart) {
            auto pickIds = buildFillPickIds(geom, fStart, fEnd, layerIndex, first, next);
            vl.fillChunks.push_back(
                uploadVectorChunkWithPickId(geom.fillVerts.data() + fStart, fEnd - fStart,
                                            kColorFloatsPerVertex, pickIds.data(), pickIds.size()));
        }
        const size_t oStart = static_cast<size_t>(sF.outRangeStart);
        const size_t oEnd = static_cast<size_t>(sL.outRangeStart) + static_cast<size_t>(sL.outRangeCount);
        if (oEnd > oStart) {
            // @Jin 在线 GeoJSON 的外环/洞环在 2D 也逐 range 独立上传，避免线宽 shader 展开退化桥后露出岛洞连接线。
            const bool separateOutlineRanges = vl.remoteVectorSource && (oEnd - oStart) <= 64;
            if (separateOutlineRanges) {
                for (size_t ri = oStart; ri < oEnd; ++ri) {
                    const std::vector<float> batch = buildStripBatch(geom.outlineVerts, geom.outlineRanges, ri, ri + 1);
                    if (batch.empty()) continue;
                    auto pickIds = buildStripPickIds(geom.outlineRanges, ri, ri + 1,
                                                     geom.featSpans, first, next, layerIndex,
                                                     &FeatureSpans::outRangeStart,
                                                     &FeatureSpans::outRangeCount);
                    vl.outlineChunks.push_back(uploadVectorChunkWithPickId(
                        batch.data(), batch.size(), kLineFloatsPerVertex, pickIds.data(), pickIds.size()));
                }
            } else {
                const std::vector<float> batch = buildStripBatch(geom.outlineVerts, geom.outlineRanges, oStart, oEnd);
                auto pickIds = buildStripPickIds(geom.outlineRanges, oStart, oEnd,
                                                 geom.featSpans, first, next, layerIndex,
                                                 &FeatureSpans::outRangeStart,
                                                 &FeatureSpans::outRangeCount);
                vl.outlineChunks.push_back(uploadVectorChunkWithPickId(
                    batch.data(), batch.size(), kLineFloatsPerVertex, pickIds.data(), pickIds.size()));
            }
        }
        const size_t lStart = static_cast<size_t>(sF.lineRangeStart);
        const size_t lEnd = static_cast<size_t>(sL.lineRangeStart) + static_cast<size_t>(sL.lineRangeCount);
        if (lEnd > lStart) {
            // @Jin 在线 GeoJSON 的多段线同样逐 range 成 chunk；段数很少时 draw call 增量可忽略，但能杜绝跨段斜接线。
            const bool separateLineRanges = vl.remoteVectorSource && (lEnd - lStart) <= 64;
            if (separateLineRanges) {
                for (size_t ri = lStart; ri < lEnd; ++ri) {
                    const std::vector<float> batch = buildStripBatch(geom.lineVerts, geom.lineRanges, ri, ri + 1);
                    if (batch.empty()) continue;
                    auto pickIds = buildStripPickIds(geom.lineRanges, ri, ri + 1,
                                                     geom.featSpans, first, next, layerIndex,
                                                     &FeatureSpans::lineRangeStart,
                                                     &FeatureSpans::lineRangeCount);
                    vl.lineChunks.push_back(uploadVectorChunkWithPickId(
                        batch.data(), batch.size(), kLineFloatsPerVertex, pickIds.data(), pickIds.size()));
                }
            } else {
                const std::vector<float> batch = buildStripBatch(geom.lineVerts, geom.lineRanges, lStart, lEnd);
                auto pickIds = buildStripPickIds(geom.lineRanges, lStart, lEnd,
                                                 geom.featSpans, first, next, layerIndex,
                                                 &FeatureSpans::lineRangeStart,
                                                 &FeatureSpans::lineRangeCount);
                vl.lineChunks.push_back(uploadVectorChunkWithPickId(
                    batch.data(), batch.size(), kLineFloatsPerVertex, pickIds.data(), pickIds.size()));
            }
        }
        vl.uploadFeat = next;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    if (vl.uploadDone) {
        vl.glReady = true;
        vl.streamingUpload.store(false);
        // [诊断] 流式上传收尾：chunk 数与总耗时（跨帧累计）；巨层耗时主要在 glBufferData 吞吐。
        LOGI("[VecReload] 流式上传完成: feats=%zu fillChunks=%zu outlineChunks=%zu lineChunks=%zu pts=%zu cost=%lldms",
             nSpans, vl.fillChunks.size(), vl.outlineChunks.size(), vl.lineChunks.size(),
             geom.pointCoords.size() / 2,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - vl.uploadStartedAt).count());
    }
}

void Renderer::resetVectorGl() {
    const int camLevel = nav_.displayLevel();
    for (auto &vlp : vectorLayers_) {
        // 旧上下文的 VBO 名已失效：只放弃句柄不 glDelete，进度重置回首要素，
        // 下一帧据保留的 CPU 几何重新流式上传（与瓦片纹理/VBO 重建同一口径，避免绑失效 VBO 导致崩溃）。
        resetVectorUpload(*vlp, /*del=*/false);
        // 图标纹理属旧上下文：放弃句柄（不 glDelete），保留 CPU 像素，下一帧据 iconRgba 重传
        vlp->iconTex.abandon();
        // 重传即重新接管：有几何待传且「有效可见」（文档可见 + 级别达标）的层保持提示标志
        // （与新几何接管口径一致；级别不足隐藏的层不计入，达标续传后再计入）
        if (!vlp->geom.empty() && vectorLayerShown(*vlp, camLevel)) vlp->streamingUpload.store(true);
    }
}

void Renderer::releaseVectorGl() {
    for (auto &vlp : vectorLayers_) {
        resetVectorUpload(*vlp, /*del=*/true);
        vlp->iconTex.release();
        vlp->streamingUpload.store(false);
    }
}

// ==================== GPU 拾取通路实现（2D PickID color-attachment） ====================

bool Renderer::ensurePickFbo() {
    if (viewportWidth_ <= 0 || viewportHeight_ <= 0) return false;
    if (pickFbo_ != 0 && pickW_ == viewportWidth_ && pickH_ == viewportHeight_) return true;
    // 尺寸变了或首次：删除旧的，建新的（懒创建口径同纹理/VBO）
    if (pickFbo_ != 0) { glDeleteFramebuffers(1, &pickFbo_); pickFbo_ = 0; }
    if (pickTex_ != 0) { glDeleteTextures(1, &pickTex_); pickTex_ = 0; }
    glGenFramebuffers(1, &pickFbo_);
    glGenTextures(1, &pickTex_);
    glBindTexture(GL_TEXTURE_2D, pickTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, viewportWidth_, viewportHeight_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, pickFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pickTex_, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("Renderer: pick FBO incomplete (0x%04x)", static_cast<unsigned>(status));
        glDeleteFramebuffers(1, &pickFbo_); pickFbo_ = 0;
        glDeleteTextures(1, &pickTex_); pickTex_ = 0;
        return false;
    }
    pickW_ = viewportWidth_;
    pickH_ = viewportHeight_;
    return true;
}

std::pair<int, long long> Renderer::renderPickPass2D(double sxPx, double syPx) {
    // 前置条件：拾取着色器已编译、视口有效、有矢量层
    if (pickColorProgram_.program() == 0 || pickLineProgram_.program() == 0) return {-1, -1};
    if (viewportWidth_ <= 0 || viewportHeight_ <= 0) return {-1, -1};
    if (vectorLayers_.empty()) return {-1, -1};
    if (!ensurePickFbo()) return {-1, -1};
    // 像素越界守卫
    const int px = static_cast<int>(sxPx);
    const int py = static_cast<int>(syPx);
    if (px < 0 || px >= pickW_ || py < 0 || py >= pickH_) return {-1, -1};

    const int camLevel = nav_.displayLevel();
    const GLsizei stride = kColorFloatsPerVertex * sizeof(float);
    const GLsizei lineStride = kLineFloatsPerVertex * sizeof(float);
    const float worldPerPx = viewportWidth_ > 0
        ? static_cast<float>((2.0 * lastHw_) / viewportWidth_) : 0.0f;

    // 1) 绑定离屏 FBO，清背景为 (0,0,0,0) = 无命中
    GLint savedViewport[4];
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    glBindFramebuffer(GL_FRAMEBUFFER, pickFbo_);
    glViewport(0, 0, pickW_, pickH_);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND); // 拾取 pass 不需混合：后绘要素直接覆盖前绘

    // 2) 逐可见矢量层以 PickID 着色器绘制（同 drawVectorLayers 可见/上传判据 + 绘制顺序）
    for (size_t li = 0; li < vectorLayers_.size(); ++li) {
        const VectorLayer &vl = *vectorLayers_[li];
        if (vl.dead || vl.noPick) continue;
        if (!vectorLayerShown(vl, camLevel)) continue;
        if (!vl.glReady || vl.geom.empty()) continue;
        const float offX = static_cast<float>(vl.geom.originWx - lastCwx_);
        const float offY = static_cast<float>(vl.geom.originWy - lastCwy_);

        // 面填充（pickColorProgram_：aPos stride=6f offset=0 + aPickId stride=4b）
        if (!vl.fillChunks.empty()) {
            pickColorProgram_.use();
            glUniformMatrix4fv(pkUMvp_, 1, GL_FALSE, lastOrtho_.data());
            glUniform2f(pkUOffset_, offX, offY);
            glEnableVertexAttribArray(pkAPos_);
            glEnableVertexAttribArray(pkAPickId_);
            for (const auto &c : vl.fillChunks) {
                if (c.pickIdVbo == 0) continue; // 无 PickID VBO（旧 chunk / 建失败）跳过
                glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
                glVertexAttribPointer(pkAPos_, 2, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(0));
                glBindBuffer(GL_ARRAY_BUFFER, c.pickIdVbo);
                glVertexAttribPointer(pkAPickId_, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4,
                                      reinterpret_cast<const void *>(0));
                glDrawArrays(GL_TRIANGLES, 0, c.vertexCount);
            }
            glDisableVertexAttribArray(pkAPos_);
            glDisableVertexAttribArray(pkAPickId_);
        }

        // 面描边 + 线要素（pickLineProgram_：aPos + aMiter stride=8f + aPickId stride=4b）
        const auto drawPickStrips = [&](const std::vector<VectorLayer::GlChunk> &chunks, float halfPx) {
            if (chunks.empty()) return;
            pickLineProgram_.use();
            glUniformMatrix4fv(pkLUMvp_, 1, GL_FALSE, lastOrtho_.data());
            glUniform2f(pkLUOffset_, offX, offY);
            glUniform1f(pkLUHalfWidthWorld_, halfPx * worldPerPx);
            glEnableVertexAttribArray(pkLAPos_);
            glEnableVertexAttribArray(pkLAMiter_);
            glEnableVertexAttribArray(pkLAPickId_);
            for (const auto &c : chunks) {
                if (c.pickIdVbo == 0) continue;
                glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
                glVertexAttribPointer(pkLAPos_, 2, GL_FLOAT, GL_FALSE, lineStride,
                                      reinterpret_cast<const void *>(0));
                glVertexAttribPointer(pkLAMiter_, 2, GL_FLOAT, GL_FALSE, lineStride,
                                      reinterpret_cast<const void *>(2 * sizeof(float)));
                glBindBuffer(GL_ARRAY_BUFFER, c.pickIdVbo);
                glVertexAttribPointer(pkLAPickId_, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4,
                                      reinterpret_cast<const void *>(0));
                glDrawArrays(GL_TRIANGLE_STRIP, 0, c.vertexCount);
            }
            glDisableVertexAttribArray(pkLAPos_);
            glDisableVertexAttribArray(pkLAMiter_);
            glDisableVertexAttribArray(pkLAPickId_);
        };
        drawPickStrips(vl.outlineChunks, vl.style.outlineWidth * 0.5f);
        drawPickStrips(vl.lineChunks, vl.style.lineWidth * 0.5f);
        // 点要素不在 GPU pass 中绘制：数量少、CPU 距离检测快；GPU miss 后 CPU 回退覆盖
    }

    // 3) 读回点击位置像素并解码（GLES 2.0 无 PBO，直接同步 glReadPixels）
    uint8_t pixel[4] = {0, 0, 0, 0};
    glReadPixels(px, pickH_ - 1 - py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    // 恢复默认帧缓冲 + 视口
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);

    int layer = -1;
    size_t prim = 0;
    if (!decodePickPixel(pixel[0], pixel[1], pixel[2], pixel[3], layer, prim))
        return {-1, -1};
    // 越界保护：layer 索引和 prim 索引有效性
    if (layer < 0 || static_cast<size_t>(layer) >= vectorLayers_.size()) return {-1, -1};
    const VectorLayer &vl = *vectorLayers_[static_cast<size_t>(layer)];
    if (prim >= vl.geom.pickPrims.size()) return {-1, -1};
    return {layer, vl.geom.pickPrims[prim].fid};
}

bool Renderer::drawVectorLayers(double cwx, double cwy, double hw, const Matrix4 &ortho) {
    if (vectorLayers_.empty()) return false;
    bool needRedraw = false;
    // 相机整数级别（级别可见性门控共用一帧一值）：低于 minDisplayLevel 的层整层隐藏，
    // 上传/绘制/提示接管均不生效（与文档 visible 同口径，级别恢复后自然续传）。
    const int camLevel = nav_.displayLevel();
    const GLsizei stride = kColorFloatsPerVertex * sizeof(float);    // 面填充/点：6 floats
    const GLsizei lineStride = kLineFloatsPerVertex * sizeof(float); // 线/描边：8 floats

    for (size_t li = 0; li < vectorLayers_.size(); ++li) {
        VectorLayer &vl = *vectorLayers_[li];
        // 墓碑叠加层：在 GL 线程回收其 VBO/图标纹理/CPU 几何（JNI 线程不可安全释放 GL 资源），随后跳过。
        // 与 releaseVectorGl 同款释放口径，幂等：句柄已清零/纹理已失效时为空操作，可反复进入不重复删除。
        if (vl.dead) {
            // 与 releaseVectorGl 同款释放口径（chunk/图标 VBO + 纹理 + 进度重置），
            // 幂等：句柄已清零/纹理已失效时为空操作，可反复进入不重复删除。
            resetVectorUpload(vl, /*del=*/true);
            vl.iconTex.release();
            vl.streamingUpload.store(false);
            vl.geom = VectorGeometry{};
            continue;
        }
        // 取走后台就绪几何（一次性），并据加载状态判断是否需再画一帧
        if (vl.loader) {
            VectorGeometry g;
            if (vl.loader->drainReady(g)) {
                // 新几何接管（首次 / 静止重载换入）：删旧 chunk VBO 并重置进度（图标静态 VBO 随几何
                // 作废懒重建）；loader 已转 Consumed，由 streamingUpload 接续向 UI 报「仍在工作」
                // 直到全部 chunk 上传完成（提示胶囊覆盖整个流式上传过程）。
                resetVectorUpload(vl, /*del=*/true);
                vl.geom = std::move(g);
                // 仅「有效可见」时立即接管上传并报 working；级别不足/隐藏的层照旧 drain（免 loader 积压），
                // 但不置 streamingUpload（胶囊不误计入），进门后由上传调度自然续跑并接续提示。
                if (!vl.geom.empty() && vectorLayerShown(vl, camLevel)) {
                    vl.uploadStartedAt = std::chrono::steady_clock::now();
                    vl.streamingUpload.store(true);
                }
            }
            if (vl.loader->hasPendingOrReady()) needRedraw = true;
        }
        if (!vectorLayerShown(vl, camLevel)) continue;
        // 流式上传（据保留的 CPU 几何）：每帧时间预算内按要素组推进上传，未传部分按已上传 chunk
        // 绘制 → 画面「整块图斑一片一片」长出；未完成期间持续请求重绘（WHEN_DIRTY 下自触发）。
        // 隐藏层上传挂起，转可见后再续跑（streamingUpload 判定含 visible，提示不误留）。
        if (!vl.uploadDone && !vl.geom.empty()) {
            uploadVectorChunks(vl, static_cast<int>(li));
            needRedraw = true;
        }
        if (vl.geom.empty()) continue;

        // RTC 偏移：图层原点 − 相机中心（double 算后转 float），与瓦片 world−camCenter 口径一致
        const float offX = static_cast<float>(vl.geom.originWx - cwx);
        const float offY = static_cast<float>(vl.geom.originWy - cwy);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // 世界单位/像素（与点半径同口径）：把像素半宽换算为世界偏移 → 屏幕空间恒定像素线宽
        const float worldPerPx = viewportWidth_ > 0 ? static_cast<float>((2.0 * hw) / viewportWidth_) : 0.0f;

        // 1) 面填充（color 程序，GL_TRIANGLES）：逐已上传 chunk 循环画（重绑 buffer 后须重设顶点属性指针）
        if (!vl.fillChunks.empty()) {
            colorProgram_.use();
            glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
            glUniform2f(cUOffset_, offX, offY);
            glEnableVertexAttribArray(cAPos_);
            glEnableVertexAttribArray(cAColor_);
            for (const auto &c : vl.fillChunks) {
                glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
                glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(0));
                glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(2 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, c.vertexCount);
            }
            glDisableVertexAttribArray(cAPos_);
            glDisableVertexAttribArray(cAColor_);
        }

        // 2)+3) 面描边 + 线要素（line 程序，GL_TRIANGLE_STRIP，屏幕空间等宽 + miter 接头）：
        // 每 chunk 已在上传时 degenerate 合批为单条带，此处逐 chunk 画（程序/uniform 每层仅设一次）
        if (!vl.outlineChunks.empty() || !vl.lineChunks.empty()) {
            lineProgram_.use();
            glUniformMatrix4fv(lUMvp_, 1, GL_FALSE, ortho.data());
            glUniform2f(lUOffset_, offX, offY);
            glEnableVertexAttribArray(lAPos_);
            glEnableVertexAttribArray(lAMiter_);
            glEnableVertexAttribArray(lAColor_);
            const auto drawStrips = [&](const std::vector<VectorLayer::GlChunk> &chunks, float halfPx) {
                glUniform1f(lUHalfWidthWorld_, halfPx * worldPerPx);
                for (const auto &c : chunks) {
                    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
                    glVertexAttribPointer(lAPos_, 2, GL_FLOAT, GL_FALSE, lineStride, reinterpret_cast<const void *>(0));
                    glVertexAttribPointer(lAMiter_, 2, GL_FLOAT, GL_FALSE, lineStride,
                                          reinterpret_cast<const void *>(2 * sizeof(float)));
                    glVertexAttribPointer(lAColor_, 4, GL_FLOAT, GL_FALSE, lineStride,
                                          reinterpret_cast<const void *>(4 * sizeof(float)));
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, c.vertexCount);
                }
            };
            drawStrips(vl.outlineChunks, vl.style.outlineWidth * 0.5f);
            drawStrips(vl.lineChunks, vl.style.lineWidth * 0.5f);
            glDisableVertexAttribArray(lAPos_);
            glDisableVertexAttribArray(lAMiter_);
            glDisableVertexAttribArray(lAColor_);
        }

        // 4) 点要素：无图标层画屏幕固定圆（color 程序，复用 vboDynamic_，相对坐标 + uOffset，与定位标记同口径）；
        // 有图标层由 drawVectorIcons 以 billboard 渲染，此处跳过避免圆与图标重叠。
        // 批量口径：全部点展开为 GL_TRIANGLES 单缓冲，每层仅 1 次 glBufferData + 1 次 draw
        //（原逐点 fan.clear+上传+draw，2000 点≈2000 次上传/draw，是点层卡顿主因）。
        // 点数据量小、无独立 VBO（逐帧动态展开），保持「全部上传完成后才显」口径（原 glReady 门控等价）。
        if (vl.glReady && vl.iconRgba.empty() && !vl.geom.pointCoords.empty() && viewportWidth_ > 0) {
            const float radius = static_cast<float>(worldPerPx * vl.style.pointRadiusDp * nav_.displayDensity());
            if (radius > 0.0f) {
                // 单位圆顶点表（一次构建跨帧复用）：免每点每帧 cos/sin
                static const std::vector<std::pair<float, float>> unitCircle = [] {
                    std::vector<std::pair<float, float>> pts;
                    pts.reserve(kMarkerSegments + 1);
                    for (int s = 0; s <= kMarkerSegments; ++s) {
                        const float a = static_cast<float>(2.0 * 3.14159265358979 * s / kMarkerSegments);
                        pts.emplace_back(std::cos(a), std::sin(a));
                    }
                    return pts;
                }();
                colorProgram_.use();
                glUniformMatrix4fv(cUMvp_, 1, GL_FALSE, ortho.data());
                glUniform2f(cUOffset_, offX, offY);
                glEnableVertexAttribArray(cAPos_);
                glEnableVertexAttribArray(cAColor_);
                glBindBuffer(GL_ARRAY_BUFFER, vboDynamic_);
                glVertexAttribPointer(cAPos_, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(0));
                glVertexAttribPointer(cAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(2 * sizeof(float)));
                std::vector<float> tris;
                tris.reserve(vl.geom.pointCoords.size() / 2 * kMarkerSegments * 3 * kColorFloatsPerVertex);
                for (size_t i = 0; i + 1 < vl.geom.pointCoords.size(); i += 2) {
                    const float px = vl.geom.pointCoords[i];
                    const float py = vl.geom.pointCoords[i + 1];
                    for (int s = 0; s < kMarkerSegments; ++s) {
                        pushColorVertexA(tris, px, py,
                                         vl.style.pointR, vl.style.pointG, vl.style.pointB, vl.style.pointA);
                        pushColorVertexA(tris, px + radius * unitCircle[s].first, py + radius * unitCircle[s].second,
                                         vl.style.pointR, vl.style.pointG, vl.style.pointB, vl.style.pointA);
                        pushColorVertexA(tris, px + radius * unitCircle[s + 1].first,
                                         py + radius * unitCircle[s + 1].second,
                                         vl.style.pointR, vl.style.pointG, vl.style.pointB, vl.style.pointA);
                    }
                }
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tris.size() * sizeof(float)),
                             tris.data(), GL_DYNAMIC_DRAW);
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tris.size() / kColorFloatsPerVertex));
                glDisableVertexAttribArray(cAPos_);
                glDisableVertexAttribArray(cAColor_);
            }
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_BLEND);
    }
    return needRedraw;
}

void Renderer::setFontPath(const std::string &path) {
    // FontAtlas::load 幂等、内部持锁、纯 CPU（含一次文件 IO），可在任意线程调用（早于 GL 上下文亦可）。
    // 加载后图集为 dirty（尚无字形），首帧排版按需烘焙并上传纹理。
    fontAtlas_.load(path);
}

const VectorLayer::LabelLayout *Renderer::ensureLabelLayout(VectorLayer &vl, size_t idx) {
    const LabelItem &li = vl.geom.labels[idx];
    if (vl.labelLayouts.size() != vl.geom.labels.size())
        vl.labelLayouts.resize(vl.geom.labels.size()); // 标注数变化（图层重载）：扩容/缩容后逐条按 text 判失效
    VectorLayer::LabelLayout &lay = vl.labelLayouts[idx];
    if (lay.text != li.text) {
        // 重建排版（烘焙像素单位）：UTF-8 解码 + 字形查表（按需烘焙，可能置 atlas dirty）。
        // 有码点未取到字形（如字体未加载/图集满）时不标记完成，下帧重试。
        std::vector<uint32_t> cps;
        FontAtlas::decodeUtf8(li.text, cps);
        lay.totalAdv = 0.0f;
        lay.boxes.clear();
        lay.boxes.reserve(cps.size());
        bool allResolved = true;
        for (uint32_t cp : cps) {
            const FontAtlas::Glyph *g = fontAtlas_.glyph(cp);
            VectorLayer::GlyphBox gb;
            if (g != nullptr) {
                gb.adv = g->advanceX;
                if (!g->blank) {
                    gb.x0 = g->x0; gb.y0 = g->y0; gb.x1 = g->x1; gb.y1 = g->y1;
                    gb.u0 = g->u0; gb.v0 = g->v0; gb.u1 = g->u1; gb.v1 = g->v1;
                }
            } else {
                allResolved = false;
            }
            lay.totalAdv += gb.adv;
            lay.boxes.push_back(gb);
        }
        lay.text = (allResolved && !cps.empty()) ? li.text : ""; // 未完成→留空串下帧重试
        if (cps.empty()) lay.text = li.text; // 空文本为确定结果，不反复重建
    }
    if (lay.totalAdv <= 0.0f) return nullptr;
    return &lay;
}

void Renderer::drawVectorLabels(double cwx, double cwy, double hw, const Matrix4 &ortho) {
    if (vectorLayers_.empty() || viewportWidth_ <= 0) return;
    if (!fontAtlas_.isLoaded() || textProgram_.program() == 0 || vboText_ == 0) return;

    // 世界单位/像素（与点/线同口径）：把像素尺寸换算为世界偏移 → 屏幕恒定字号
    const float worldPerPx = static_cast<float>((2.0 * hw) / viewportWidth_);
    if (worldPerPx <= 0.0f) return;
    const float density = static_cast<float>(nav_.displayDensity());
    const float bakePx = static_cast<float>(fontAtlas_.bakePx());
    const float vCenterPx = (fontAtlas_.ascentPx() + fontAtlas_.descentPx()) * 0.5f; // 视觉中心相对基线（上正）

    // 把一个矢量层的全部标注字形四边形追加到 verts：颜色 (cr,cg,cb,ca)、整体屏幕偏移 (dx,dy) 世界单位。
    // 排版结果按层缓存在 [VectorLayer::labelLayouts]（烘焙像素单位，与缩放/相机无关）：
    // 免每帧全量 UTF-8 解码 + 字形查表（轮廓层每帧重复 9 趟，是标注层卡顿主因）；
    // 仅余每帧必要的坐标变换（乘 ws + 平移）。锚点水平居中、垂直居中于锚点。
    // 把一个矢量层的全部标注字形四边形追加到 verts：逐条按 LabelItem 自带样式（色/字号/轮廓）绘制，
    // [outline]=true 画轮廓（仅该条启用轮廓才画、用其轮廓色），否则画文字主体（用其文字色）。
    // 排版结果按层缓存在 [VectorLayer::labelLayouts]（烘焙像素单位，与缩放/相机无关）：
    // 免每帧全量 UTF-8 解码 + 字形查表；仅余每帧必要的坐标变换（乘 ws + 平移）。锚点水平/垂直居中。
    auto appendLayer = [&](std::vector<float> &verts, VectorLayer &vl,
                           float offX, float offY, float dx, float dy, bool outline) {
        for (size_t idx = 0; idx < vl.geom.labels.size(); ++idx) {
            const LabelItem &li = vl.geom.labels[idx];
            if (outline && !li.outline) continue; // 仅该条启用轮廓时画轮廓
            // 期望文字像素高（屏幕恒定）：16dp × density × 逐条字号；每烘焙像素 → 世界单位
            const float ws = (16.0f * density * li.size) / bakePx * worldPerPx;
            if (ws <= 0.0f) continue;
            const VectorLayer::LabelLayout *lay = ensureLabelLayout(vl, idx);
            if (lay == nullptr) continue;
            const float *col = outline ? li.outlineColor : li.color;
            // 锚点（相机相对世界坐标，含 RTC 偏移与轮廓偏移）+ 世界单位/烘焙像素比例 ws
            emitLabelQuads(*lay, li.x + offX + dx, li.y + offY + dy, ws, vCenterPx,
                           col[0], col[1], col[2], col[3], verts);
        }
    };

    // 相机整数级别：级别不足隐藏的层其标注一并隐藏（与填充/描边同进同退，免出现无体只剩字的幽灵标注）。
    const int camLevel = nav_.displayLevel();

    // Pass 1：轮廓（poor-man's：8 方向偏移描边色），画在文字之下。逐条判轮廓开关，层内无任一启用轮廓则跳过 8 趟。
    std::vector<float> outlineVerts;
    const float o = 1.0f * density * worldPerPx; // 轮廓偏移 ≈ 1dp（世界单位）
    static const float kDir[8][2] = {
        {1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f},
        {0.7071f, 0.7071f}, {-0.7071f, 0.7071f}, {0.7071f, -0.7071f}, {-0.7071f, -0.7071f},
    };
    for (const auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (!vectorLayerShown(vl, camLevel) || vl.geom.labels.empty()) continue;
        bool anyOutline = false;
        for (const auto &li : vl.geom.labels) if (li.outline) { anyOutline = true; break; }
        if (!anyOutline) continue;
        const float offX = static_cast<float>(vl.geom.originWx - cwx);
        const float offY = static_cast<float>(vl.geom.originWy - cwy);
        for (const auto &d : kDir) {
            appendLayer(outlineVerts, vl, offX, offY, d[0] * o, d[1] * o, true);
        }
    }

    // Pass 2：文字主体（全部可见且有标注的层）
    std::vector<float> fillVerts;
    for (const auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (!vectorLayerShown(vl, camLevel) || vl.geom.labels.empty()) continue;
        const float offX = static_cast<float>(vl.geom.originWx - cwx);
        const float offY = static_cast<float>(vl.geom.originWy - cwy);
        appendLayer(fillVerts, vl, offX, offY, 0.0f, 0.0f, false);
    }

    if (outlineVerts.empty() && fillVerts.empty()) return;

    // 上传/重传图集：排版期间可能烘焙了新字形（dirty），或上下文重建后 atlasTex_ 已 abandon（invalid）
    if (!atlasTex_.isValid() || fontAtlas_.dirty()) {
        atlasTex_.uploadRGBA(fontAtlas_.pixels(), fontAtlas_.size(), fontAtlas_.size());
        fontAtlas_.clearDirty();
    }
    if (!atlasTex_.isValid()) return;

    textProgram_.use();
    glUniformMatrix4fv(txUMvp_, 1, GL_FALSE, ortho.data());
    // 文字为纯屏幕叠加：显式关深度测试（对齐 drawVectorLabels3D 已有做法）。原 2D 注释虽声明「关深度」
    // 却漏了调用；2D 主循环不清深度缓冲，本 pass 沿用进入时的深度测试残留态 → 与共面填充面同 z(0)
    // 的标注被深度剔除（面积标注时有时无即此因；距离/点无共面大面故不受影响）。收尾按进入时状态精确复位。
    GLboolean depthBefore = GL_FALSE;
    glGetBooleanv(GL_DEPTH_TEST, &depthBefore);
    glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(txUTexture_, 0);
    atlasTex_.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, vboText_);
    const GLsizei tstride = kTextFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(txAPos_);
    glEnableVertexAttribArray(txATexCoord_);
    glEnableVertexAttribArray(txAColor_);
    glVertexAttribPointer(txAPos_, 2, GL_FLOAT, GL_FALSE, tstride, reinterpret_cast<const void *>(0));
    glVertexAttribPointer(txATexCoord_, 2, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<const void *>(2 * sizeof(float)));
    glVertexAttribPointer(txAColor_, 4, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<const void *>(4 * sizeof(float)));

    auto drawBuf = [&](const std::vector<float> &buf) {
        if (buf.empty()) return;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(buf.size() * sizeof(float)),
                     buf.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(buf.size() / kTextFloatsPerVertex));
    };
    drawBuf(outlineVerts); // 轮廓在下
    drawBuf(fillVerts);    // 文字在上

    glDisableVertexAttribArray(txAPos_);
    glDisableVertexAttribArray(txATexCoord_);
    glDisableVertexAttribArray(txAColor_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    if (depthBefore) glEnable(GL_DEPTH_TEST); // 复位进入本 pass 前的深度态，不外泄
}

void Renderer::drawVectorIcons(double cwx, double cwy, double hw, const Matrix4 &ortho) {
    if (vectorLayers_.empty() || viewportWidth_ <= 0) return;
    if (iconProgram_.program() == 0) return;
    // 世界单位/像素（与点/线/标注同口径）：把图标的像素尺寸换算为世界偏移 → 屏幕恒定像素尺寸
    const float worldPerPx = static_cast<float>((2.0 * hw) / viewportWidth_);
    if (worldPerPx <= 0.0f) return;

    iconProgram_.use();
    glUniformMatrix4fv(ixUMvp_, 1, GL_FALSE, ortho.data());
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const GLsizei stride = kIconFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(ixAPos_);
    glEnableVertexAttribArray(iACorner_);
    glEnableVertexAttribArray(ixATexCoord_);
    glEnableVertexAttribArray(ixAColor_);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(ixUTexture_, 0);

    // 相机整数级别：级别不足隐藏的层其点要素图标一并隐藏（与其余绘制通路同口径）。
    const int camLevel = nav_.displayLevel();

    for (const auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (!vectorLayerShown(vl, camLevel) || vl.glReady == false ||
            vl.geom.pointCoords.empty() || vl.iconRgba.empty()) continue;
        // 懒建静态图标 VBO（首帧 / 几何重载 / 上下文重建后）：顶点存「中心相对坐标 + 角点属性」，
        // 不随相机变化，建一次长期复用（原路径逐帧重建全部四边形 + glBufferData，是图标层卡顿主因）
        if (vl.iconVbo == 0) {
            std::vector<float> verts;
            verts.reserve(vl.geom.pointCoords.size() / 2 * 6 * kIconFloatsPerVertex);
            for (size_t i = 0; i + 1 < vl.geom.pointCoords.size(); i += 2) {
                // 中心锚点（对齐 参考实现 Placemark imageOffset 默认居中）；v=0 对应图像顶行（世界 y 较小侧），
                // 与文本/瓦片同口径 → 图标正立。顶点色白（不染色，片元 texture × color = 纯图标）。
                const float px = vl.geom.pointCoords[i];
                const float py = vl.geom.pointCoords[i + 1];
                pushIconVert(verts, px, py, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                pushIconVert(verts, px, py, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            }
            if (verts.empty()) continue;
            glGenBuffers(1, &vl.iconVbo);
            glBindBuffer(GL_ARRAY_BUFFER, vl.iconVbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                         verts.data(), GL_STATIC_DRAW);
            vl.iconVertexCount = static_cast<GLsizei>(verts.size() / kIconFloatsPerVertex);
        }
        // 懒上传/上下文重建后重传图标纹理（GL 线程；iconRgba 构造后只读，无竞争）
        if (!vl.iconTex.isValid() &&
            !vl.iconTex.uploadRGBA(vl.iconRgba.data(), vl.iconW, vl.iconH)) {
            continue;
        }
        // 图标屏幕固定尺寸：位图像素（已含密度，由宿主解码矢量 drawable 得到）→ 世界半宽/半高
        const float halfW = vl.iconW * 0.5f * worldPerPx;
        const float halfH = vl.iconH * 0.5f * worldPerPx;
        if (halfW <= 0.0f || halfH <= 0.0f) continue;
        // RTC 偏移在着色器内施加（顶点存相对坐标，免逐帧烘焙）
        glUniform2f(ixUOffset_, static_cast<float>(vl.geom.originWx - cwx),
                    static_cast<float>(vl.geom.originWy - cwy));
        glUniform2f(ixUHalf_, halfW, halfH);
        vl.iconTex.bind();
        // 顶点属性指针记录「设置时绑定的 buffer」，逐层切 VBO 须在绑定后重设
        glBindBuffer(GL_ARRAY_BUFFER, vl.iconVbo);
        glVertexAttribPointer(ixAPos_, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(0));
        glVertexAttribPointer(iACorner_, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void *>(2 * sizeof(float)));
        glVertexAttribPointer(ixATexCoord_, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void *>(4 * sizeof(float)));
        glVertexAttribPointer(ixAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void *>(6 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, vl.iconVertexCount);
    }

    glDisableVertexAttribArray(ixAPos_);
    glDisableVertexAttribArray(iACorner_);
    glDisableVertexAttribArray(ixATexCoord_);
    glDisableVertexAttribArray(ixAColor_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}

// ==================== P2.e 3D 矢量层：分帧流式上传（chunk + 世界 ECEF AABB，绘制侧视锥剔除，口径对齐 2D uploadVectorChunks） ====================


bool Renderer::isExperimentalLocalDrapedLineLayer(const VectorLayer &vl) const {
    constexpr bool kExperimentalLocalVectorLineTerrainDrape = true; // @Jin drape36 实验开关，改 false 即可回退普通本地椭球面线。
    const VectorGeometry &geom = vl.geom;
    // @Jin drape36 首轮只接管“本地文件纯线层”：排除 overlay、在线源、面、点和图标，避免影响已验证的面/点贴地路径。
    return kExperimentalLocalVectorLineTerrainDrape &&
           !vl.isOverlay &&
           !vl.remoteVectorSource &&
           geom.fillVerts.empty() &&
           geom.outlineVerts.empty() &&
           !geom.lineVerts.empty() &&
           !geom.lineRanges.empty() &&
           geom.pointCoords.empty();
}

uint64_t Renderer::overlayDrapedTerrainSignature(const VectorLayer &vl) const {
    const VectorGeometry &geom = vl.geom;
    if (!geom.hasGeoBounds) return 0; // @Jin 旧几何/测试数据没有经纬度 bbox 时不做贴地自动重建。
    double lonMin = geom.geoLonMin;
    double latMin = geom.geoLatMin;
    double lonMax = geom.geoLonMax;
    double latMax = geom.geoLatMax;
    constexpr double kPadDeg = 0.002; // @Jin 约 200m 外扩，覆盖边缘插值和 DEM 瓦片边界。
    lonMin -= kPadDeg; lonMax += kPadDeg; latMin -= kPadDeg; latMax += kPadDeg;

    // @Jin 先收集命中 DEM 瓦片的稳定 word 再排序哈希：terrainCache_ 是 unordered_map，
    // @Jin 瓦片到达/淘汰后桶遍历顺序会变化，直接边遍历边 FNV 会让同一批 DEM 算出不同签名，
    // @Jin 导致叠加层每帧误判“地形变了”而反复重建 VBO（真机日志可见 sig 每帧都变、疯狂重建）。
    std::vector<uint64_t> words;
    words.reserve(16);
    for (const auto &entry : terrainCache_) {
        const QuantizedMeshData &dem = entry.second;
        if (lonMax < dem.lonMin || lonMin > dem.lonMax || latMax < dem.latMin || latMin > dem.latMax) continue;
        // @Jin key 与区域最大高都参与签名：同一范围内新增、删除或内容替换才让 overlay 重建。
        const uint64_t word = entry.first ^ (static_cast<uint64_t>(static_cast<int64_t>(dem.aboveMax)) * 1099511628211ULL);
        words.push_back(word);
    }
    std::sort(words.begin(), words.end()); // @Jin 顺序无关，保证签名对同一组 DEM 稳定。
    uint64_t sig = 1469598103934665603ULL;
    for (uint64_t word : words) {
        sig ^= word;
        sig *= 1099511628211ULL;
    }
    sig ^= static_cast<uint64_t>(words.size()) * 1469598103ULL;
    return sig;
}

bool Renderer::overlayHasTerrainCoverage(const VectorLayer &vl) const {
    const VectorGeometry &geom = vl.geom;
    if (!geom.hasGeoBounds) return false;
    const bool localDrapedLine = isExperimentalLocalDrapedLineLayer(vl); // @Jin drape36：本地省级纯线也等待精细 DEM，避免全球粗瓦片触发烘焙。
    const bool largeDrapedFile = vl.remoteVectorSource || localDrapedLine; // @Jin drape36：大跨度文件线与在线大面共用精细覆盖门限。
    constexpr double kPadDeg = 0.002; // @Jin 与签名外扩保持一致，DEM 瓦片边缘也能触发贴地路径。
    const double lonMin = geom.geoLonMin - kPadDeg;
    const double lonMax = geom.geoLonMax + kPadDeg;
    const double latMin = geom.geoLatMin - kPadDeg;
    const double latMax = geom.geoLatMax + kPadDeg;
    double finestIntersectSpan = 1e300;
    for (const auto &entry : terrainCache_) {
        const QuantizedMeshData &dem = entry.second;
        if (!(lonMax < dem.lonMin || lonMin > dem.lonMax || latMax < dem.latMin || latMin > dem.latMax)) {
            if (!largeDrapedFile) return true; // @Jin overlay 与旧文件层维持原策略，有任意相交 DEM 即可贴地。
            finestIntersectSpan = std::min(finestIntersectSpan,
                                           std::max(dem.lonMax - dem.lonMin, dem.latMax - dem.latMin));
        }
    }
    // @Jin drape36：在线大面和本地省级纯线都不要被全球 z0 粗 DEM 触发完整烘焙；等待约 1° 内精细地形。
    constexpr double kLargeDrapeMinFineCoverageDeg = 1.0;
    return largeDrapedFile ? finestIntersectSpan <= kLargeDrapeMinFineCoverageDeg : false;
}

// @Jin drape29/drape36：实验性 DEM 贴地烘焙现服务在线 GeoJSON 大面与普通本地纯线层；与内存 overlay 旧路径分离，可按层类型回退。
void Renderer::uploadGlobeRemoteDrapedChunks(VectorLayer &vl) {
    const VectorGeometry &geom = vl.geom;
    const double ox = geom.originWx;
    const double oy = geom.originWy;
    const Vec3 anchor = worldXYToCartesian(ox, oy, 0.0);
    vl.globeAnchor = anchor;
    const bool localDrapedLine = isExperimentalLocalDrapedLineLayer(vl); // @Jin drape36：本地纯线复用大图层 DEM 烘焙通路。
    const bool largeDrapedVector = vl.remoteVectorSource || localDrapedLine; // @Jin drape36：远程大面和本地大线共享运动延迟/精细索引策略。
    vl.globeDraped = false; // @Jin 先按旧深度策略初始化；若本层实际采到 DEM，再在烘焙结束后切为贴地绘制策略。
    vl.globeRemoteLargeDrapeMode = largeDrapedVector; // @Jin drape36：进入 draped 烘焙即代表当前处于低空贴地门限内。

    // @Jin 贴地偏移只取很小的固定值：真实起伏由逐顶点 DEM 高程承担，offset 仅防 z-fighting。
    constexpr double kDrapeLiftMeters = 1.5;
    constexpr int kMaxDrapeSubdivide = 9; // @Jin 小区域最多 512 等分；额外深度用于精确分离 DEM 缓存边界。
    constexpr double kDrapeMinEdgeMeters = 20.0; // @Jin 小区域样例的最密目标边长。
    constexpr double kDrapeMaxEdgeCapMeters = 32000.0; // @Jin 省级等大叠加层的最粗目标边长，防止固定 20m 顶点爆炸。
    constexpr size_t kDrapeFillVertexBudget = 180000; // @Jin 小范围叠加层仍允许较密贴地；远程大面使用下方更低的实验预算。
    constexpr size_t kDrapeStrokeCornerBudget = 120000; // @Jin 单层线/描边加密角点硬预算。
    constexpr size_t kDrapeSubdivideFillSourceLimit = 120000; // @Jin 省级大面需要在内部生成 DEM 采样点；边长判断已不采 DEM，18 万输出预算内可保留自适应细分。
    constexpr size_t kDrapeDensifyStrokeSourceLimit = 120000; // @Jin 省级大层也允许插点，但只对当前 DEM 覆盖内的线段加密。
    constexpr size_t kRemoteDrapeFillVertexBudget = 100000; // @Jin drape33 给省域细分预留安全余量，避免最后少量叶子落入平面 chunk 产生黑缝。
    constexpr double kRemoteDrapeDemEdgeScale = 0.35; // @Jin drape30 远程大面按约 1/3 DEM 边长加密；24km 叶子会跨地形块落入平面 chunk，近景只剩窄条。
    constexpr double kRemoteDrapeSpanDivisor = 44.0; // @Jin drape19 远程省域初始目标边约 25km，先保证手势流畅；小范围 overlay 不受此实验参数影响。
    const size_t drapeFillVertexBudget = largeDrapedVector ? kRemoteDrapeFillVertexBudget : kDrapeFillVertexBudget; // @Jin drape36：本地大线虽无填充，也与远程大面共用保守预算口径。

    // @Jin 计算叠加层相对坐标 bbox：贴地细分密度按覆盖跨度自适应。省级 GeoJSON 横跨数千公里时，
    // @Jin 目标边自动放到几十公里级；太白山样例仅数公里时保持约 20m。这样既保留小区域贴地精度，
    // @Jin 又避免全国大面在 3D 中被递归细分到数百万/千万三角形而冻结 GL 线程。
    double relXmin = 0.0, relYmin = 0.0, relXmax = 0.0, relYmax = 0.0;
    bool hasRelBounds = false;
    auto accRelXY = [&](float rx, float ry) {
        if (!hasRelBounds) { relXmin = relXmax = rx; relYmin = relYmax = ry; hasRelBounds = true; return; }
        relXmin = std::min(relXmin, static_cast<double>(rx)); relXmax = std::max(relXmax, static_cast<double>(rx));
        relYmin = std::min(relYmin, static_cast<double>(ry)); relYmax = std::max(relYmax, static_cast<double>(ry));
    };
    for (size_t i = 0; i + kColorFloatsPerVertex - 1 < geom.fillVerts.size(); i += kColorFloatsPerVertex)
        accRelXY(geom.fillVerts[i], geom.fillVerts[i + 1]);
    auto accStrokeBounds = [&](const std::vector<float> &src) {
        for (size_t i = 0; i + kLineFloatsPerVertex - 1 < src.size(); i += kLineFloatsPerVertex)
            accRelXY(src[i], src[i + 1]);
    };
    accStrokeBounds(geom.outlineVerts);
    accStrokeBounds(geom.lineVerts);
    for (size_t i = 0; i + 1 < geom.pointCoords.size(); i += 2) accRelXY(geom.pointCoords[i], geom.pointCoords[i + 1]);
    constexpr double kWorldCircumferenceMeters = 2.0 * 3.14159265358979323846 * 6378137.0;
    const double spanWorld = hasRelBounds ? std::max(relXmax - relXmin, relYmax - relYmin) : 0.0;
    const double spanMeters = spanWorld * kWorldCircumferenceMeters;
    const double spanDivisor = largeDrapedVector ? kRemoteDrapeSpanDivisor : 224.0; // @Jin drape36：本地省级线按远程大面密度自适应，overlay 小样例仍使用原细分密度。
    double targetEdgeMeters = std::max(kDrapeMinEdgeMeters, std::min(kDrapeMaxEdgeCapMeters, spanMeters / spanDivisor));
    const size_t sourceFillVertexCount = geom.fillVerts.size() / kColorFloatsPerVertex;
    const size_t sourceStrokeVertexCount = geom.outlineVerts.size() / kLineFloatsPerVertex + geom.lineVerts.size() / kLineFloatsPerVertex;
    const bool allowFillSubdivide = sourceFillVertexCount <= kDrapeSubdivideFillSourceLimit;
    const bool allowStrokeDensify = sourceStrokeVertexCount <= kDrapeDensifyStrokeSourceLimit;
    size_t fillDrapedVertexCount = 0;
    size_t strokeDrapedCornerCount = 0;
    bool fillBudgetHit = false;
    bool strokeBudgetHit = false;
    const auto drapeBuildStart = std::chrono::steady_clock::now(); // @Jin 记录省级大面 draped 烘焙总耗时，真机用 hilog 定位 3D 卡顿尖峰。
    if (!allowFillSubdivide) fillBudgetHit = true; // @Jin 大层走“只采原始顶点”保护路径，日志与预算命中共用标志。
    if (!allowStrokeDensify) strokeBudgetHit = true;

    struct DrapeVertex {
        double wx = 0.0, wy = 0.0;
        Vec3 planeCart{0.0, 0.0, 0.0}; // @Jin 椭球面位置只用于边长判断，避免为判断细分反复采样 DEM。
        float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        // @Jin 同一顶点在递归判断和最终烘焙间复用 DEM 命中结果，避免省级大面数万顶点重复哈希查找。
        mutable bool directDemResolved = false;
        mutable const QuantizedMeshData *directDem = nullptr;
    };
    auto toLonLat = [&](double wx, double wy, double &lon, double &lat) {
        worldXYToGeographic(ox + wx, oy + wy, lon, lat);
    };

    struct DemCoverageRef {
        double lonMin = 0.0, latMin = 0.0, lonMax = 0.0, latMax = 0.0;
        const QuantizedMeshData *dem = nullptr;
    };
    std::vector<DemCoverageRef> demCoverage;
    demCoverage.reserve(terrainCache_.size());
    double maxDemLonSpan = 1e-6;
    double maxDemLatSpan = 1e-6;
    double finestIntersectLonSpan = 1e300;
    double finestIntersectLatSpan = 1e300;
    for (const auto &entry : terrainCache_) {
        const QuantizedMeshData &dem = entry.second;
        const double demLonSpan = dem.lonMax - dem.lonMin;
        const double demLatSpan = dem.latMax - dem.latMin;
        demCoverage.push_back({dem.lonMin, dem.latMin, dem.lonMax, dem.latMax, &dem});
        maxDemLonSpan = std::max(maxDemLonSpan, demLonSpan);
        maxDemLatSpan = std::max(maxDemLatSpan, demLatSpan);
        if (geom.hasGeoBounds &&
            !(geom.geoLonMax < dem.lonMin || geom.geoLonMin > dem.lonMax ||
              geom.geoLatMax < dem.latMin || geom.geoLatMin > dem.latMax)) {
            finestIntersectLonSpan = std::min(finestIntersectLonSpan, demLonSpan); // @Jin 远程大面只按与本层相交的最高精度 DEM 决定密度。
            finestIntersectLatSpan = std::min(finestIntersectLatSpan, demLatSpan);
        }
    }
    if (geom.hasGeoBounds && largeDrapedVector && finestIntersectLonSpan < 1e299) { // @Jin drape36：本地省级线也按相交的最精细 DEM 决定沿线加密尺度。
        const double centerLat = (geom.geoLatMin + geom.geoLatMax) * 0.5;
        const double demLonMeters = finestIntersectLonSpan * 111320.0 * std::max(0.1, std::cos(centerLat * 3.14159265358979 / 180.0));
        const double demLatMeters = finestIntersectLatSpan * 111320.0;
        targetEdgeMeters = std::max(kDrapeMinEdgeMeters, std::min(targetEdgeMeters, std::max(demLonMeters, demLatMeters) * kRemoteDrapeDemEdgeScale));
    } else if (geom.hasGeoBounds) {
        const double centerLat = (geom.geoLatMin + geom.geoLatMax) * 0.5;
        const double demLonMeters = maxDemLonSpan * 111320.0 * std::max(0.1, std::cos(centerLat * 3.14159265358979 / 180.0));
        const double demLatMeters = maxDemLatSpan * 111320.0;
        // @Jin 近距离高 zoom DEM 瓦片更细，贴地加密目标同步细化；仍受 20m 下限和总预算保护。
        targetEdgeMeters = std::max(kDrapeMinEdgeMeters, std::min(targetEdgeMeters, std::max(demLonMeters, demLatMeters) * 0.25));
    }
    // @Jin drape36：远程大面和本地省级线都按“与本层相交的最精细 DEM”建立覆盖粗格；若仍用 z0 全球瓦片尺寸，
    // @Jin drape36：一块太白山 z11 会让整个陕西省的线/面源几何都误判为相交并参与细分。
    const bool useFineCoverageCells = geom.hasGeoBounds && largeDrapedVector && // @Jin drape36：本地省级线使用精细 DEM 格网，避免全域粗格造成采样退化。
                                      finestIntersectLonSpan < 1e299 && finestIntersectLatSpan < 1e299;
    const double demCellDeg = useFineCoverageCells
                                      ? std::max(finestIntersectLonSpan, finestIntersectLatSpan)
                                      : std::max(maxDemLonSpan, maxDemLatSpan);
    auto demCellKey = [](int cx, int cy) -> uint64_t {
        constexpr uint64_t kOffset = 1000000ULL;
        return (static_cast<uint64_t>(static_cast<int64_t>(cy) + kOffset) << 32) |
               (static_cast<uint64_t>(static_cast<int64_t>(cx) + kOffset) & 0xffffffffULL);
    };
    std::unordered_map<uint64_t, std::vector<size_t>> demGrid;
    std::unordered_set<uint64_t> demOccupiedCells;
    for (size_t di = 0; di < demCoverage.size(); ++di) {
        const DemCoverageRef &ref = demCoverage[di];
        int cx0 = static_cast<int>(std::floor(ref.lonMin / demCellDeg));
        int cx1 = static_cast<int>(std::floor(ref.lonMax / demCellDeg));
        int cy0 = static_cast<int>(std::floor(ref.latMin / demCellDeg));
        int cy1 = static_cast<int>(std::floor(ref.latMax / demCellDeg));
        // @Jin cell 尺寸取最大瓦片跨度，正常每块 DEM 只落 1~4 格；加保护防止异常 bbox 造成大量插入。
        cx1 = std::min(cx1, cx0 + 31);
        cy1 = std::min(cy1, cy0 + 31);
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                const uint64_t key = demCellKey(cx, cy);
                demGrid[key].push_back(di);
                demOccupiedCells.insert(key); // @Jin 递归细分只需要“该粗格是否可能有 DEM”。
            }
        }
    }
    auto clampToDemRange = [](double v, double lo, double hi) {
        return std::max(lo, std::min(hi, v));
    };
    const QuantizedMeshData *fallbackDemAtPoint = nullptr;
    double fallbackLon = 0.0;
    double fallbackLat = 0.0;
    auto demAtPoint = [&](double lon, double lat, bool allowNearestFallback) -> const QuantizedMeshData * {
        fallbackDemAtPoint = nullptr;
        fallbackLon = lon;
        fallbackLat = lat;
        const int cx = static_cast<int>(std::floor(lon / demCellDeg));
        const int cy = static_cast<int>(std::floor(lat / demCellDeg));
        const auto it = demGrid.find(demCellKey(cx, cy));
        if (it != demGrid.end()) {
            for (size_t di : it->second) {
                const DemCoverageRef &ref = demCoverage[di];
                if (lon >= ref.lonMin && lon <= ref.lonMax && lat >= ref.latMin && lat <= ref.latMax) {
                    return ref.dem;
                }
            }
        }
        if (!allowNearestFallback) return nullptr;
        // @Jin 仅对与 DEM 相交的加密三角形启用邻近兜底，避免远处整个省域都被错误抬升。
        double bestDist2 = 1e300;
        for (int ry = -2; ry <= 2; ++ry) {
            for (int rx = -2; rx <= 2; ++rx) {
                const auto nearIt = demGrid.find(demCellKey(cx + rx, cy + ry));
                if (nearIt == demGrid.end()) continue;
                for (size_t di : nearIt->second) {
                    const DemCoverageRef &ref = demCoverage[di];
                    const double cxClamped = clampToDemRange(lon, ref.lonMin, ref.lonMax);
                    const double cyClamped = clampToDemRange(lat, ref.latMin, ref.latMax);
                    // @Jin 用经纬度平方距离做近邻选择；当前只在数公里级加密边上使用，纬度缩放误差可接受。
                    const double dx = (lon - cxClamped) * std::cos(lat * 3.14159265358979 / 180.0);
                    const double dy = lat - cyClamped;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < bestDist2) {
                        bestDist2 = d2;
                        fallbackDemAtPoint = ref.dem;
                        fallbackLon = cxClamped;
                        fallbackLat = cyClamped;
                    }
                }
            }
        }
        return fallbackDemAtPoint;
    };
    auto boundsHaveDem = [&](double lonMin, double latMin, double lonMax, double latMax) {
        if (!(lonMax >= lonMin && latMax >= latMin)) return false;
        int cx0 = static_cast<int>(std::floor(lonMin / demCellDeg));
        int cx1 = static_cast<int>(std::floor(lonMax / demCellDeg));
        int cy0 = static_cast<int>(std::floor(latMin / demCellDeg));
        int cy1 = static_cast<int>(std::floor(latMax / demCellDeg));
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                const auto it = demGrid.find(demCellKey(cx, cy));
                if (it == demGrid.end()) continue;
                for (size_t di : it->second) {
                    const DemCoverageRef &ref = demCoverage[di];
                    if (!(lonMax < ref.lonMin || lonMin > ref.lonMax ||
                          latMax < ref.latMin || latMin > ref.latMax)) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    auto vertexLonLat = [&](const DrapeVertex &v, double &lon, double &lat) {
        toLonLat(v.wx, v.wy, lon, lat);
    };
    auto projectPolygonRange = [](const double poly[][2], size_t count, double ax, double ay,
                                  double &pmin, double &pmax) {
        pmin = pmax = poly[0][0] * ax + poly[0][1] * ay;
        for (size_t i = 1; i < count; ++i) {
            const double value = poly[i][0] * ax + poly[i][1] * ay;
            pmin = std::min(pmin, value);
            pmax = std::max(pmax, value);
        }
    };
    auto polygonAxesOverlap = [&](const double tri[][2], const double rect[][2],
                                  double ax, double ay) {
        double t0 = 0.0, t1 = 0.0, r0 = 0.0, r1 = 0.0;
        projectPolygonRange(tri, 3, ax, ay, t0, t1);
        projectPolygonRange(rect, 4, ax, ay, r0, r1);
        return !(t1 < r0 - 1e-9 || r1 < t0 - 1e-9);
    };
    auto triangleIntersectsRectangle = [&](const double tri[][2],
                                           double lonMin, double latMin,
                                           double lonMax, double latMax) {
        const double rect[4][2] = {{lonMin, latMin}, {lonMax, latMin},
                                   {lonMax, latMax}, {lonMin, latMax}};
        const double axes[5][2] = {
            {1.0, 0.0}, {0.0, 1.0},
            {tri[1][1] - tri[0][1], tri[0][0] - tri[1][0]},
            {tri[2][1] - tri[1][1], tri[1][0] - tri[2][0]},
            {tri[0][1] - tri[2][1], tri[2][0] - tri[0][0]}};
        for (const auto &axis : axes) {
            if (!polygonAxesOverlap(tri, rect, axis[0], axis[1])) return false;
        }
        return true;
    };
    auto triangleHasDemCoverage = [&](const DrapeVertex &a, const DrapeVertex &b, const DrapeVertex &c) {
        double tri[3][2];
        vertexLonLat(a, tri[0][0], tri[0][1]);
        vertexLonLat(b, tri[1][0], tri[1][1]);
        vertexLonLat(c, tri[2][0], tri[2][1]);
        const double lon0 = std::min({tri[0][0], tri[1][0], tri[2][0]});
        const double lon1 = std::max({tri[0][0], tri[1][0], tri[2][0]});
        const double lat0 = std::min({tri[0][1], tri[1][1], tri[2][1]});
        const double lat1 = std::max({tri[0][1], tri[1][1], tri[2][1]});
        // @Jin 递归阶段只做粗格占用判断，避免对每棵子树重复 SAT 求交；叶子是否真的贴地仍由三顶点直接命中严格裁决。
        const int cx0 = static_cast<int>(std::floor(lon0 / demCellDeg));
        const int cx1 = static_cast<int>(std::floor(lon1 / demCellDeg));
        const int cy0 = static_cast<int>(std::floor(lat0 / demCellDeg));
        const int cy1 = static_cast<int>(std::floor(lat1 / demCellDeg));
        const int spanCellsX = std::min(cx1 - cx0 + 1, 33);
        const int spanCellsY = std::min(cy1 - cy0 + 1, 33);
        if (spanCellsX * spanCellsY > 128) return true; // @Jin 横跨全省的根三角形直接细分，避免枚举上百个粗格。
        for (int cy = cy0; cy <= cy0 + spanCellsY - 1; ++cy) {
            for (int cx = cx0; cx <= cx0 + spanCellsX - 1; ++cx) {
                if (demOccupiedCells.find(demCellKey(cx, cy)) != demOccupiedCells.end()) return true;
            }
        }
        return false;
    };
    auto resolveDirectDem = [&](const DrapeVertex &v) -> const QuantizedMeshData * {
        if (v.directDemResolved) return v.directDem;
        double lon = 0.0, lat = 0.0;
        toLonLat(v.wx, v.wy, lon, lat);
        if (vl.isOverlay) {
            // @Jin drape28：内存点/线/面/图标沿用 3663589 已真机验证的直接遍历采样口径，避免粗格漏检把个别顶点留在椭球面。
            for (const auto &entry : terrainCache_) {
                const QuantizedMeshData &candidate = entry.second;
                if (lon >= candidate.lonMin && lon <= candidate.lonMax &&
                    lat >= candidate.latMin && lat <= candidate.latMax) {
                    v.directDem = &candidate;
                    break;
                }
            }
        } else {
            // @Jin 在线 GeoJSON 大面继续使用实验性粗格索引与严格直接命中策略，便于控制省域烘焙成本并独立回退。
            v.directDem = demAtPoint(lon, lat, false);
        }
        v.directDemResolved = true;
        return v.directDem;
    };
    auto vertexHasDirectDem = [&](const DrapeVertex &v) {
        return resolveDirectDem(v) != nullptr;
    };
    int drapedSampleHits = 0;
    int drapedSampleTotal = 0;
    double drapedAltMin = 1e300, drapedAltMax = -1e300;
    auto cartFor = [&](const DrapeVertex &v, bool allowNearestFallback, bool *directHit = nullptr) -> Vec3 {
        (void)allowNearestFallback; // @Jin drape11 明确关闭邻近兜底，保留参数以减少调用点改动、便于实验回退。
        const QuantizedMeshData *dem = resolveDirectDem(v);
        if (directHit != nullptr) *directHit = (dem != nullptr);
        double lon = 0.0, lat = 0.0;
        toLonLat(v.wx, v.wy, lon, lat);
        // @Jin demAtPoint 直接命中时 fallbackLon/fallbackLat 即输入经纬度；这里显式使用原坐标，避免缓存后读到旧值。
        const double demAlt = dem != nullptr ? dem->sampleHeight(lon, lat) : 0.0;
        const bool hit = dem != nullptr && demAlt != 0.0; // @Jin 海面真实 0 无法与未命中区分；当前实验数据为山地，不影响判断。
        ++drapedSampleTotal;
        if (hit) ++drapedSampleHits;
        if (dem != nullptr) {
            drapedAltMin = std::min(drapedAltMin, demAlt);
            drapedAltMax = std::max(drapedAltMax, demAlt);
        }
        if (vl.isOverlay) {
            // @Jin drape28：内存 overlay 即使局部暂未命中 DEM，也保持 mode=1 + 1.5m 抬升，与已知良好基线一致并由关闭深度测试保证可见。
            if (directHit != nullptr) *directHit = true;
            return worldXYToCartesian(ox + v.wx, oy + v.wy, demAlt + kDrapeLiftMeters);
        }
        const double alt = dem != nullptr ? (demAlt + kDrapeLiftMeters) : 0.0;
        return worldXYToCartesian(ox + v.wx, oy + v.wy, alt);
    };
    auto distanceWor = [&](const DrapeVertex &a, const DrapeVertex &b) {
        // @Jin 细分判断只用椭球面弦长；DEM 高度仅在最终输出顶点时采样一次。
        return (a.planeCart - b.planeCart).length();
    };
    auto mixVertex = [&](const DrapeVertex &a, const DrapeVertex &b, double t) {
        DrapeVertex m;
        m.wx = a.wx + (b.wx - a.wx) * t;
        m.wy = a.wy + (b.wy - a.wy) * t;
        for (int i = 0; i < 4; ++i) m.rgba[i] = a.rgba[i] + (b.rgba[i] - a.rgba[i]) * static_cast<float>(t);
        m.planeCart = a.planeCart + (b.planeCart - a.planeCart) * t; // @Jin 弦中点仅用于边长判断，不参与最终 DEM 烘焙坐标。
        return m;
    };
    auto pushDrapedFill = [&](std::vector<float> &verts, double bb[6], const DrapeVertex &v) {
        const Vec3 p = cartFor(v, false);
        globeAabbExtend(bb, p);
        // @Jin mode=1：高度已烘焙到顶点，shader 不再叠加 vecAlt，防止真实地形高度被二次抬升。
        pushGlobeFillVert(verts, static_cast<float>(p.x - anchor.x),
                          static_cast<float>(p.y - anchor.y),
                          static_cast<float>(p.z - anchor.z),
                          v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
    };
    auto pushFlatFill = [&](std::vector<float> &verts, double bb[6], const DrapeVertex &v) {
        // @Jin mode=0 + alt=0：非 DEM 叶子严格走普通矢量层路径，由 shader 统一施加 vecAlt，
        // @Jin 绝不能混入真实 DEM 高程，否则同一三角形会从 0m 斜拉到数千米并盖住山体。
        const Vec3 p = worldXYToCartesian(ox + v.wx, oy + v.wy, 0.0);
        globeAabbExtend(bb, p);
        pushGlobeFillVert(verts, static_cast<float>(p.x - anchor.x),
                          static_cast<float>(p.y - anchor.y),
                          static_cast<float>(p.z - anchor.z),
                          v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 0.0f);
    };
    auto emitTriangle = [&](std::vector<float> &drapedVerts, double drapedBB[6],
                            std::vector<float> &flatVerts, double flatBB[6],
                            const DrapeVertex &rootA, const DrapeVertex &rootB, const DrapeVertex &rootC) {
        struct PendingTri { DrapeVertex a, b, c; int depth; };
        std::vector<PendingTri> stack;
        stack.push_back({rootA, rootB, rootC, 0});
        while (!stack.empty()) {
            PendingTri cur = stack.back();
            stack.pop_back();
            const double ab = distanceWor(cur.a, cur.b);
            const double bc = distanceWor(cur.b, cur.c);
            const double ca = distanceWor(cur.c, cur.a);
            const bool edgeNeedsSubdivide = ab > targetEdgeMeters || bc > targetEdgeMeters || ca > targetEdgeMeters;
            const bool insideTerrainCoverage = triangleHasDemCoverage(cur.a, cur.b, cur.c);
            if (allowFillSubdivide && fillDrapedVertexCount + 3u <= drapeFillVertexBudget && cur.depth < kMaxDrapeSubdivide &&
                edgeNeedsSubdivide && insideTerrainCoverage) {
                // @Jin 只有与 DEM 覆盖相交的三角形继续加密，远处省域保持平面并由深度测试遮挡。
                if (ab >= bc && ab >= ca) {
                    DrapeVertex m = mixVertex(cur.a, cur.b, 0.5);
                    stack.push_back({m, cur.b, cur.c, cur.depth + 1});
                    stack.push_back({cur.a, m, cur.c, cur.depth + 1});
                } else if (bc >= ca) {
                    DrapeVertex m = mixVertex(cur.b, cur.c, 0.5);
                    stack.push_back({cur.a, m, cur.c, cur.depth + 1});
                    stack.push_back({cur.a, cur.b, m, cur.depth + 1});
                } else {
                    DrapeVertex m = mixVertex(cur.c, cur.a, 0.5);
                    stack.push_back({m, cur.b, cur.c, cur.depth + 1});
                    stack.push_back({cur.a, cur.b, m, cur.depth + 1});
                }
                continue;
            }
            const bool allVerticesInDem = vertexHasDirectDem(cur.a) &&
                                          vertexHasDirectDem(cur.b) &&
                                          vertexHasDirectDem(cur.c);
            const bool leafDraped = vl.isOverlay || (insideTerrainCoverage && allVerticesInDem); // @Jin drape28：内存小叠加层整层走已验证贴地 VBO；严格三分流只用于在线大面。
            std::vector<float> &targetVerts = leafDraped ? drapedVerts : flatVerts;
            double *targetBB = leafDraped ? drapedBB : flatBB;
            // @Jin 只有三个顶点均直接命中 DEM 的叶子三角形才抬高；跨缓存边界的三角形严格保持平面，
            // @Jin 两类顶点绝不混入同一个三角形/VBO 语义，避免边缘拉出蓝色大斜片。
            if (leafDraped) {
                pushDrapedFill(targetVerts, targetBB, cur.a);
                pushDrapedFill(targetVerts, targetBB, cur.b);
                pushDrapedFill(targetVerts, targetBB, cur.c);
            } else {
                pushFlatFill(targetVerts, targetBB, cur.a);
                pushFlatFill(targetVerts, targetBB, cur.b);
                pushFlatFill(targetVerts, targetBB, cur.c);
            }
            fillDrapedVertexCount += 3u;
            if (fillDrapedVertexCount >= drapeFillVertexBudget) fillBudgetHit = true;
        }
    };

    // @Jin 面填充拆成 DEM 覆盖 chunk 与普通平面 chunk；前者关闭地形深度测试，后者继续受地形遮挡。
    {
        std::vector<float> drapedVerts;
        std::vector<float> flatVerts;
        double drapedBB[6], flatBB[6];
        globeAabbInit(drapedBB);
        globeAabbInit(flatBB);
        auto flushFill = [&](std::vector<float> &verts, double bb[6], bool drapedChunk) {
            if (verts.empty()) return;
            VectorLayer::GlobeChunk chunk = uploadGlobeChunk(verts, bb, kGlobeFillFloatsPerVertex);
            chunk.terrainDraped = drapedChunk;
            vl.globeFillChunks.push_back(chunk);
            verts.clear();
            globeAabbInit(bb);
        };
        const size_t triCount = geom.fillVerts.size() / kColorFloatsPerVertex / 3u;
        for (size_t ti = 0; ti < triCount; ++ti) {
            DrapeVertex tri[3];
            for (int vi = 0; vi < 3; ++vi) {
                const size_t i = (ti * 3u + static_cast<size_t>(vi)) * kColorFloatsPerVertex;
                tri[vi].wx = geom.fillVerts[i];
                tri[vi].wy = geom.fillVerts[i + 1];
                tri[vi].rgba[0] = geom.fillVerts[i + 2];
                tri[vi].rgba[1] = geom.fillVerts[i + 3];
                tri[vi].rgba[2] = geom.fillVerts[i + 4];
                tri[vi].rgba[3] = geom.fillVerts[i + 5];
                tri[vi].planeCart = worldXYToCartesian(ox + tri[vi].wx, oy + tri[vi].wy, 0.0);
            }
            emitTriangle(drapedVerts, drapedBB, flatVerts, flatBB, tri[0], tri[1], tri[2]);
            if (drapedVerts.size() > 240000u) flushFill(drapedVerts, drapedBB, true);
            if (flatVerts.size() > 240000u) flushFill(flatVerts, flatBB, false);
        }
        flushFill(drapedVerts, drapedBB, true);
        flushFill(flatVerts, flatBB, false);
    }

    auto buildStroke = [&](const std::vector<float> &src, const std::vector<std::pair<int, int>> &ranges,
                          std::vector<VectorLayer::GlobeChunk> &chunks) {
        const bool markLocalLineChunksDraped = localDrapedLine; // @Jin drape36c：本地纯线最终采高 chunk 逐块关闭地形深度，避免整层状态切换时闪烁。
        // @Jin 洞环数量很少（省级样例 7 段、太白山样例 1 段）时，每段独立一个 GL_TRIANGLE_STRIP chunk，
        // @Jin 完全不做环间 degenerate bridge，从机制上消除外环/洞环之间的多余连线；大图层仍回退合批。
        const bool separateStrokeRanges = largeDrapedVector && ranges.size() <= 64; // @Jin drape36：本地 MultiLineString 各环也独立成 chunk，避免外环/洞线之间产生退化桥杂线。
        std::vector<float> batchVerts;
        double batchBB[6];
        globeAabbInit(batchBB);
        for (size_t ri = 0; ri < ranges.size(); ++ri) {
            const int firstVert = ranges[ri].first;
            const int sourceCornerCount = ranges[ri].second / 2;
            if (sourceCornerCount < 2) continue;
            const float *firstSrc = src.data() + static_cast<size_t>(firstVert) * kLineFloatsPerVertex;
            const float *lastSrc = src.data() + static_cast<size_t>(firstVert + 2 * (sourceCornerCount - 1)) *
                                                          kLineFloatsPerVertex;
            // @Jin 2D VectorBuilder 对闭合环会在 range 末尾重复一次首点；draped 路径先按唯一角点处理，
            // @Jin 随后单独对“末点→首点”闭合边加密并只追加一次首点，避免漏封口或把同一条边画两次。
            const bool sourceClosed = sourceCornerCount >= 4 && firstSrc[0] == lastSrc[0] && firstSrc[1] == lastSrc[1];
            const int uniqueSourceCount = sourceClosed ? sourceCornerCount - 1 : sourceCornerCount;
            std::vector<DrapeVertex> corners;
            corners.reserve(std::min<size_t>(static_cast<size_t>(uniqueSourceCount + 1) * 4u, 4096u)); // @Jin 为闭合环追加首点预留空间。
            auto readSourceVertex = [&](int c) -> DrapeVertex {
                const float *v = src.data() + static_cast<size_t>(firstVert + 2 * c) * kLineFloatsPerVertex;
                DrapeVertex cur;
                cur.wx = v[0];
                cur.wy = v[1];
                cur.planeCart = worldXYToCartesian(ox + cur.wx, oy + cur.wy, 0.0);
                for (int k = 0; k < 4; ++k) cur.rgba[k] = v[4 + k];
                return cur;
            };
            auto appendSegment = [&](const DrapeVertex &prev, const DrapeVertex &cur, size_t remainingRaw) {
                // @Jin 必须拷贝上一角点：下面会继续向 corners 插点，vector 扩容会导致引用悬空；
                // @Jin 悬空引用会让插值经纬度读到垃圾内存并生成跨屏幕的飞线。
                if (!allowStrokeDensify) { corners.push_back(cur); return; } // @Jin 超出实验源顶点阈值时保守不插点。
                const double len = distanceWor(prev, cur);
                double pLon = 0.0, pLat = 0.0, cLon = 0.0, cLat = 0.0;
                toLonLat(prev.wx, prev.wy, pLon, pLat);
                toLonLat(cur.wx, cur.wy, cLon, cLat);
                const bool segmentCovered = boundsHaveDem(std::min(pLon, cLon), std::min(pLat, cLat),
                                                          std::max(pLon, cLon), std::max(pLat, cLat));
                int inserts = segmentCovered ? std::max(0, static_cast<int>(std::ceil(len / targetEdgeMeters)) - 1) : 0;
                inserts = std::min(inserts, 64); // @Jin 单段加密上限，小区域足够贴合，超长线段分多段处理。
                // @Jin 扣除本段尚未写入的原始角点，保证线/描边总角点不超过硬预算。
                if (strokeDrapedCornerCount + remainingRaw >= kDrapeStrokeCornerBudget) inserts = 0;
                else inserts = std::min<size_t>(inserts, kDrapeStrokeCornerBudget - strokeDrapedCornerCount - remainingRaw);
                for (int q = 1; q <= inserts; ++q) corners.push_back(mixVertex(prev, cur, static_cast<double>(q) / (inserts + 1)));
                corners.push_back(cur);
            };
            corners.push_back(readSourceVertex(0));
            for (int c = 1; c < uniqueSourceCount; ++c) {
                const DrapeVertex cur = readSourceVertex(c);
                appendSegment(corners.back(), cur, static_cast<size_t>(uniqueSourceCount - c) + (sourceClosed ? 1u : 0u));
            }
            if (sourceClosed) {
                // @Jin 闭合边同样需要 DEM 加密；最后追加首点，GL_TRIANGLE_STRIP 相邻中心即形成末点到首点的 ribbon。
                appendSegment(corners.back(), readSourceVertex(0), 1);
            }
            strokeDrapedCornerCount += corners.size(); // @Jin 统计原始+加密角点；预算耗尽后后续线段不再加密。
            if (strokeDrapedCornerCount >= kDrapeStrokeCornerBudget) strokeBudgetHit = true;

            std::vector<Vec3> pts;
            std::vector<float> modes;
            double rangeBB[6];
            globeAabbInit(rangeBB);
            pts.reserve(corners.size());
            modes.reserve(corners.size());
            for (const auto &cv : corners) {
                bool directHit = false;
                const Vec3 p = cartFor(cv, false, &directHit);
                globeAabbExtend(rangeBB, p); // @Jin AABB 用绝对 ECEF 做视锥剔除。
                // @Jin 直接命中 DEM 的顶点 mode=1 采真实高程；未命中顶点 mode=0 走普通 vecAlt 平面路径，
                // @Jin 不再钳制到邻近 DEM 边缘，避免洞环/省界被错误抬到远处山上。
                pts.push_back(Vec3{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z});
                modes.push_back(directHit ? 1.0f : 0.0f);
            }

            std::vector<float> rangeVerts;
            rangeVerts.reserve(pts.size() * 2u * kGlobeStrokeFloatsPerVertex);
            for (size_t i = 0; i < pts.size(); ++i) {
                // @Jin corners 对闭合环已在末尾追加一次首点，因此这里线性输出即可闭合；首尾端的 prev/next 退化为自身形成端帽。
                const Vec3 &prevV = (i > 0) ? pts[i - 1] : pts[i];
                const Vec3 &curV = pts[i];
                const Vec3 &nextV = (i + 1 < pts.size()) ? pts[i + 1] : pts[i];
                const DrapeVertex &cv = corners[i];
                pushGlobeStrokeVert(rangeVerts, prevV, curV, nextV, +1.0f,
                                    cv.rgba[0], cv.rgba[1], cv.rgba[2], cv.rgba[3], modes[i]);
                pushGlobeStrokeVert(rangeVerts, prevV, curV, nextV, -1.0f,
                                    cv.rgba[0], cv.rgba[1], cv.rgba[2], cv.rgba[3], modes[i]);
            }
            if (rangeVerts.empty()) continue;

            if (separateStrokeRanges) {
                if (chunks.size() < kGlobeMaxChunks) {
                    VectorLayer::GlobeChunk chunk = uploadGlobeChunk(rangeVerts, rangeBB, kGlobeStrokeFloatsPerVertex);
                    chunk.terrainDraped = chunk.terrainDraped || markLocalLineChunksDraped; // @Jin drape36c：独立省界线 chunk 在地形加载过渡期也保持可见。
                    chunks.push_back(chunk);
                }
                continue;
            }

            // @Jin 大图层保留旧合批路径：仅在不同 ring 之间插入两条完整退化记录；AABB 同步合并。
            batchVerts.reserve(batchVerts.size() + rangeVerts.size() + 2u * kGlobeStrokeFloatsPerVertex);
            if (!batchVerts.empty()) {
                const size_t last = batchVerts.size() - kGlobeStrokeFloatsPerVertex;
                for (int k = 0; k < kGlobeStrokeFloatsPerVertex; ++k) batchVerts.push_back(batchVerts[last + k]);
                for (int k = 0; k < kGlobeStrokeFloatsPerVertex; ++k) batchVerts.push_back(rangeVerts[k]);
            }
            batchVerts.insert(batchVerts.end(), rangeVerts.begin(), rangeVerts.end());
            globeAabbExtend(batchBB, Vec3{rangeBB[0], rangeBB[2], rangeBB[4]}); // @Jin 合并 range 最小角。
            globeAabbExtend(batchBB, Vec3{rangeBB[1], rangeBB[3], rangeBB[5]}); // @Jin 合并 range 最大角。
        }
        if (!separateStrokeRanges && !batchVerts.empty() && chunks.size() < kGlobeMaxChunks) {
            VectorLayer::GlobeChunk chunk = uploadGlobeChunk(batchVerts, batchBB, kGlobeStrokeFloatsPerVertex);
            chunk.terrainDraped = chunk.terrainDraped || markLocalLineChunksDraped; // @Jin drape36c：合批省界线同样使用逐 chunk 贴地深度策略。
            chunks.push_back(chunk);
        }
    };
    // @Jin 面描边和线都沿线段加密，否则原始稀疏折线只会两端贴地、中段仍埋入山体。
    buildStroke(geom.outlineVerts, geom.outlineRanges, vl.globeOutlineChunks);
    buildStroke(geom.lineVerts, geom.lineRanges, vl.globeLineChunks);

    auto buildPoint = [&](const DrapeVertex &v, std::vector<float> &verts, double bb[6], bool icon) {
        bool directHit = false;
        const Vec3 p = cartFor(v, false, &directHit);
        const float pointMode = directHit ? 1.0f : 0.0f; // @Jin 点/图标同样只在直接命中 DEM 时烘焙高程。
        globeAabbExtend(bb, p);
        const Vec3 c{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z};
        if (icon) {
            // @Jin y 从中心构图的 [-1,+1] 改为 [0,+2]，让 PNG 底部针尖落在 DEM 采样点而不是图片中心。
            pushGlobeIconVert(verts, c, -1.0f, 2.0f, 0.0f, 0.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], pointMode);
            pushGlobeIconVert(verts, c, 1.0f, 2.0f, 1.0f, 0.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], pointMode);
            pushGlobeIconVert(verts, c, 1.0f, 0.0f, 1.0f, 1.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], pointMode);
            pushGlobeIconVert(verts, c, -1.0f, 2.0f, 0.0f, 0.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], pointMode);
            pushGlobeIconVert(verts, c, 1.0f, 0.0f, 1.0f, 1.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], pointMode);
            pushGlobeIconVert(verts, c, -1.0f, 0.0f, 0.0f, 1.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], pointMode);
        } else {
            static const std::vector<std::pair<float, float>> unitCircle = [] {
                std::vector<std::pair<float, float>> pts;
                pts.reserve(kMarkerSegments + 1);
                for (int s = 0; s <= kMarkerSegments; ++s) {
                    const float a = static_cast<float>(2.0 * 3.14159265358979 * s / kMarkerSegments);
                    pts.emplace_back(std::cos(a), std::sin(a));
                }
                return pts;
            }();
            for (int s = 0; s < kMarkerSegments; ++s) {
                pushGlobePointVert(verts, c, 0.0f, 0.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], pointMode);
                pushGlobePointVert(verts, c, unitCircle[s].first, unitCircle[s].second, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], pointMode);
                pushGlobePointVert(verts, c, unitCircle[s + 1].first, unitCircle[s + 1].second, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], pointMode);
            }
        }
    };
    const size_t ptN = geom.pointCoords.size() / 2u;
    if (ptN > 0) {
        std::vector<float> verts;
        double bb[6];
        globeAabbInit(bb);
        const bool icon = !vl.iconRgba.empty();
        for (size_t i = 0; i < ptN; ++i) {
            DrapeVertex v;
            v.wx = geom.pointCoords[2 * i];
            v.wy = geom.pointCoords[2 * i + 1];
            v.rgba[0] = vl.style.pointR; v.rgba[1] = vl.style.pointG; v.rgba[2] = vl.style.pointB; v.rgba[3] = vl.style.pointA;
            buildPoint(v, verts, bb, icon);
        }
        if (icon) {
            if (!vl.iconTex.isValid() && !vl.iconTex.uploadRGBA(vl.iconRgba.data(), vl.iconW, vl.iconH)) {
                // @Jin 图标纹理失败时保留空 chunk 安全状态，后续可回退普通点通路排查。
            } else {
                vl.globeIconChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeIconFloatsPerVertex));
            }
        } else {
            vl.globePointChunks.push_back(uploadGlobeChunk(verts, bb, kGlobePointFloatsPerVertex));
        }
    }

    vl.globeUploadFeat = geom.featSpans.size();
    vl.globeUploadPoint = ptN;
    vl.globeUploadExtrude = geom.extrudeVerts.size();
    vl.globeDrapedTerrainSignature = overlayDrapedTerrainSignature(vl); // @Jin 记录本层实际覆盖 DEM 的签名，而不是全局 revision。
    vl.globeUploadDone = true;
    vl.streamingUpload.store(false);
    const bool finiteAlt = std::isfinite(drapedAltMin) && std::isfinite(drapedAltMax);
    if (fillBudgetHit || strokeBudgetHit) {
        LOGI("[GlobeVec] @Jin draped large-layer guard spanKm=%.1f edgeM=%.1f fillVerts=%zu/%zu fillGuard=%d strokeCorners=%zu/%zu strokeGuard=%d", spanMeters / 1000.0, targetEdgeMeters, fillDrapedVertexCount, drapeFillVertexBudget, fillBudgetHit ? 1 : 0, strokeDrapedCornerCount, kDrapeStrokeCornerBudget, strokeBudgetHit ? 1 : 0);
    }
    vl.globeDraped = drapedSampleHits > 0; // @Jin 只有确实采到当前地形缓存时才关闭对地形的深度测试，避免无地形时穿透地球。
    const double drapeBuildMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - drapeBuildStart).count();
    LOGI("[GlobeVec] @Jin DEM draped upload overlay=%d remote=%d draped=%d sig=%llu hits=%d/%d alt=[%.1f,%.1f] fill=%zu outline=%zu line=%zu pts=%zu buildMs=%.2f",
         vl.isOverlay ? 1 : 0, vl.remoteVectorSource ? 1 : 0, vl.globeDraped ? 1 : 0,
         static_cast<unsigned long long>(vl.globeDrapedTerrainSignature), drapedSampleHits, drapedSampleTotal,
         finiteAlt ? drapedAltMin : 0.0, finiteAlt ? drapedAltMax : 0.0,
         vl.globeFillChunks.size(), vl.globeOutlineChunks.size(), vl.globeLineChunks.size(), ptN, drapeBuildMs);
}

// @Jin drape29：内存 overlay 使用 3663589 已验证的贴地烘焙函数，避免在线 GeoJSON 大面实验影响点/线/面/图标样例。
void Renderer::uploadGlobeOverlayDrapedChunks(VectorLayer &vl) {
    const VectorGeometry &geom = vl.geom;
    const double ox = geom.originWx;
    const double oy = geom.originWy;
    const Vec3 anchor = worldXYToCartesian(ox, oy, 0.0);
    vl.globeAnchor = anchor;

    // @Jin 贴地偏移只取很小的固定值：真实起伏由逐顶点 DEM 高程承担，offset 仅防 z-fighting。
    constexpr double kDrapeLiftMeters = 1.5;
    constexpr int kMaxDrapeSubdivide = 7; // @Jin 小区域最多 128 等分，保证沟谷/山脊贴合精度。
    constexpr double kDrapeMinEdgeMeters = 20.0; // @Jin 小区域样例的最密目标边长。
    constexpr double kDrapeMaxEdgeCapMeters = 32000.0; // @Jin 省级等大叠加层的最粗目标边长，防止固定 20m 顶点爆炸。
    constexpr size_t kDrapeFillVertexBudget = 180000; // @Jin 单层 fill 贴地输出顶点硬预算（约 6 万三角形）。
    constexpr size_t kDrapeStrokeCornerBudget = 120000; // @Jin 单层线/描边加密角点硬预算。
    constexpr size_t kDrapeSubdivideFillSourceLimit = 20000; // @Jin 原始 fill 顶点已很多的大 GeoJSON 不再递归细分，只采原始顶点。
    constexpr size_t kDrapeDensifyStrokeSourceLimit = 12000; // @Jin 原始线/描边角点已足够密的大图层不再插点。

    // @Jin 计算叠加层相对坐标 bbox：贴地细分密度按覆盖跨度自适应。省级 GeoJSON 横跨数千公里时，
    // @Jin 目标边自动放到几十公里级；太白山样例仅数公里时保持约 20m。这样既保留小区域贴地精度，
    // @Jin 又避免全国大面在 3D 中被递归细分到数百万/千万三角形而冻结 GL 线程。
    double relXmin = 0.0, relYmin = 0.0, relXmax = 0.0, relYmax = 0.0;
    bool hasRelBounds = false;
    auto accRelXY = [&](float rx, float ry) {
        if (!hasRelBounds) { relXmin = relXmax = rx; relYmin = relYmax = ry; hasRelBounds = true; return; }
        relXmin = std::min(relXmin, static_cast<double>(rx)); relXmax = std::max(relXmax, static_cast<double>(rx));
        relYmin = std::min(relYmin, static_cast<double>(ry)); relYmax = std::max(relYmax, static_cast<double>(ry));
    };
    for (size_t i = 0; i + kColorFloatsPerVertex - 1 < geom.fillVerts.size(); i += kColorFloatsPerVertex)
        accRelXY(geom.fillVerts[i], geom.fillVerts[i + 1]);
    auto accStrokeBounds = [&](const std::vector<float> &src) {
        for (size_t i = 0; i + kLineFloatsPerVertex - 1 < src.size(); i += kLineFloatsPerVertex)
            accRelXY(src[i], src[i + 1]);
    };
    accStrokeBounds(geom.outlineVerts);
    accStrokeBounds(geom.lineVerts);
    for (size_t i = 0; i + 1 < geom.pointCoords.size(); i += 2) accRelXY(geom.pointCoords[i], geom.pointCoords[i + 1]);
    constexpr double kWorldCircumferenceMeters = 2.0 * 3.14159265358979323846 * 6378137.0;
    const double spanWorld = hasRelBounds ? std::max(relXmax - relXmin, relYmax - relYmin) : 0.0;
    const double spanMeters = spanWorld * kWorldCircumferenceMeters;
    double targetEdgeMeters = std::max(kDrapeMinEdgeMeters, std::min(kDrapeMaxEdgeCapMeters, spanMeters / 224.0));
    const size_t sourceFillVertexCount = geom.fillVerts.size() / kColorFloatsPerVertex;
    const size_t sourceStrokeVertexCount = geom.outlineVerts.size() / kLineFloatsPerVertex + geom.lineVerts.size() / kLineFloatsPerVertex;
    const bool allowFillSubdivide = sourceFillVertexCount <= kDrapeSubdivideFillSourceLimit;
    const bool allowStrokeDensify = sourceStrokeVertexCount <= kDrapeDensifyStrokeSourceLimit;
    size_t fillDrapedVertexCount = 0;
    size_t strokeDrapedCornerCount = 0;
    bool fillBudgetHit = false;
    bool strokeBudgetHit = false;
    if (!allowFillSubdivide) fillBudgetHit = true; // @Jin 大层走“只采原始顶点”保护路径，日志与预算命中共用标志。
    if (!allowStrokeDensify) strokeBudgetHit = true;

    struct DrapeVertex {
        double wx = 0.0, wy = 0.0;
        float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };
    auto toLonLat = [&](double wx, double wy, double &lon, double &lat) {
        worldXYToGeographic(ox + wx, oy + wy, lon, lat);
    };
    int drapedSampleHits = 0;
    int drapedSampleTotal = 0;
    double drapedAltMin = 1e300, drapedAltMax = -1e300;
    auto cartFor = [&](const DrapeVertex &v) -> Vec3 {
        double lon = 0.0, lat = 0.0;
        toLonLat(v.wx, v.wy, lon, lat);
        const double demAlt = sampleTerrainHeightLonLat(lon, lat);
        const bool hit = (demAlt != 0.0); // @Jin 海面真实 0 无法与未命中区分；太白山样例非海面，先用 0 诊断未命中。
        ++drapedSampleTotal;
        if (hit) ++drapedSampleHits;
        drapedAltMin = std::min(drapedAltMin, demAlt);
        drapedAltMax = std::max(drapedAltMax, demAlt);
        const double alt = demAlt + kDrapeLiftMeters;
        return worldXYToCartesian(ox + v.wx, oy + v.wy, alt);
    };
    auto distanceWor = [&](const DrapeVertex &a, const DrapeVertex &b) {
        // @Jin 用 ECEF 弦长判断是否细分，经纬度和墨卡托在山坡小区域内都足够近似。
        return (cartFor(a) - cartFor(b)).length();
    };
    auto mixVertex = [&](const DrapeVertex &a, const DrapeVertex &b, double t) {
        DrapeVertex m;
        m.wx = a.wx + (b.wx - a.wx) * t;
        m.wy = a.wy + (b.wy - a.wy) * t;
        for (int i = 0; i < 4; ++i) m.rgba[i] = a.rgba[i] + (b.rgba[i] - a.rgba[i]) * static_cast<float>(t);
        return m;
    };
    auto pushFill = [&](std::vector<float> &verts, double bb[6], const DrapeVertex &v) {
        const Vec3 p = cartFor(v);
        globeAabbExtend(bb, p);
        pushGlobeFillVert(verts, static_cast<float>(p.x - anchor.x),
                          static_cast<float>(p.y - anchor.y),
                          static_cast<float>(p.z - anchor.z),
                          v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
    };
    auto emitTriangle = [&](std::vector<float> &verts, double bb[6],
                            const DrapeVertex &rootA, const DrapeVertex &rootB, const DrapeVertex &rootC) {
        if (!allowFillSubdivide) {
            // @Jin 大 GeoJSON 快速路径：原始三角网已经足够密，不再计算边长/递归，只逐顶点采 DEM。
            pushFill(verts, bb, rootA);
            pushFill(verts, bb, rootB);
            pushFill(verts, bb, rootC);
            fillDrapedVertexCount += 3u;
            return;
        }
        struct PendingTri { DrapeVertex a, b, c; int depth; };
        std::vector<PendingTri> stack;
        stack.push_back({rootA, rootB, rootC, 0});
        while (!stack.empty()) {
            PendingTri cur = stack.back();
            stack.pop_back();
            const double ab = distanceWor(cur.a, cur.b);
            const double bc = distanceWor(cur.b, cur.c);
            const double ca = distanceWor(cur.c, cur.a);
            if (allowFillSubdivide && fillDrapedVertexCount + 3u <= kDrapeFillVertexBudget && cur.depth < kMaxDrapeSubdivide &&
                (ab > targetEdgeMeters || bc > targetEdgeMeters || ca > targetEdgeMeters)) {
                // @Jin 最长边中点细分，避免递归 lambda 自引用，也避免大三角形内部横跨沟谷/山脊。
                if (ab >= bc && ab >= ca) {
                    DrapeVertex m = mixVertex(cur.a, cur.b, 0.5);
                    stack.push_back({m, cur.b, cur.c, cur.depth + 1});
                    stack.push_back({cur.a, m, cur.c, cur.depth + 1});
                } else if (bc >= ca) {
                    DrapeVertex m = mixVertex(cur.b, cur.c, 0.5);
                    stack.push_back({cur.a, m, cur.c, cur.depth + 1});
                    stack.push_back({cur.a, cur.b, m, cur.depth + 1});
                } else {
                    DrapeVertex m = mixVertex(cur.c, cur.a, 0.5);
                    stack.push_back({m, cur.b, cur.c, cur.depth + 1});
                    stack.push_back({cur.a, cur.b, m, cur.depth + 1});
                }
                continue;
            }
            pushFill(verts, bb, cur.a);
            pushFill(verts, bb, cur.b);
            pushFill(verts, bb, cur.c);
            fillDrapedVertexCount += 3u; // @Jin 统计实际贴地输出顶点；超出预算后后续三角形不再细分。
            if (fillDrapedVertexCount >= kDrapeFillVertexBudget) fillBudgetHit = true;
        }
    };

    // @Jin 面填充：源数据已经是 earcut 三角形，递归细分后逐顶点采 DEM，并以 mode=1 禁止 shader 再统一抬高。
    {
        std::vector<float> verts;
        double bb[6];
        globeAabbInit(bb);
        const size_t triCount = geom.fillVerts.size() / kColorFloatsPerVertex / 3u;
        for (size_t ti = 0; ti < triCount; ++ti) {
            DrapeVertex tri[3];
            for (int vi = 0; vi < 3; ++vi) {
                const size_t i = (ti * 3u + static_cast<size_t>(vi)) * kColorFloatsPerVertex;
                tri[vi].wx = geom.fillVerts[i];
                tri[vi].wy = geom.fillVerts[i + 1];
                tri[vi].rgba[0] = geom.fillVerts[i + 2];
                tri[vi].rgba[1] = geom.fillVerts[i + 3];
                tri[vi].rgba[2] = geom.fillVerts[i + 4];
                tri[vi].rgba[3] = geom.fillVerts[i + 5];
            }
            emitTriangle(verts, bb, tri[0], tri[1], tri[2]);
            if (verts.size() > 240000u) {
                // @Jin 超大叠加层按块上传，避免单 VBO 过大造成 GL 驱动卡顿。
                if (!verts.empty()) vl.globeFillChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeFillFloatsPerVertex));
                verts.clear();
                globeAabbInit(bb);
            }
        }
        if (!verts.empty()) vl.globeFillChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeFillFloatsPerVertex));
    }

    auto buildStroke = [&](const std::vector<float> &src, const std::vector<std::pair<int, int>> &ranges,
                          std::vector<VectorLayer::GlobeChunk> &chunks) {
        std::vector<float> verts;
        double bb[6];
        globeAabbInit(bb);
        for (size_t ri = 0; ri < ranges.size(); ++ri) {
            const int firstVert = ranges[ri].first;
            const int cornerCount = ranges[ri].second / 2;
            if (cornerCount < 2) continue;
            std::vector<DrapeVertex> corners;
            corners.reserve(std::min<size_t>(static_cast<size_t>(cornerCount) * 4u, 4096u)); // @Jin 预留从 16 倍收紧，大省界避免无效扩容。
            for (int c = 0; c < cornerCount; ++c) {
                const float *v = src.data() + static_cast<size_t>(firstVert + 2 * c) * kLineFloatsPerVertex;
                DrapeVertex cur;
                cur.wx = v[0];
                cur.wy = v[1];
                for (int k = 0; k < 4; ++k) cur.rgba[k] = v[4 + k];
                if (c > 0) {
                    if (!allowStrokeDensify) { corners.push_back(cur); continue; } // @Jin 大层快速路径：不计算线段长度、不插点。
                    // @Jin 必须拷贝上一角点：下面会继续向 corners 插点，vector 扩容会导致引用悬空；
                    // @Jin 悬空引用会让插值经纬度读到垃圾内存并生成跨屏幕的飞线。
                    const DrapeVertex prev = corners.back();
                    const double len = distanceWor(prev, cur);
                    int inserts = std::max(0, static_cast<int>(std::ceil(len / targetEdgeMeters)) - 1);
                    inserts = std::min(inserts, 64); // @Jin 单段加密上限，小区域足够贴合，超长线段分多段处理。
                    // @Jin 扣除本段尚未写入的原始角点，保证线/描边总角点不超过硬预算。
                    const size_t remainingRaw = static_cast<size_t>(cornerCount - c);
                    if (strokeDrapedCornerCount + remainingRaw >= kDrapeStrokeCornerBudget) inserts = 0;
                    else inserts = std::min<size_t>(inserts, kDrapeStrokeCornerBudget - strokeDrapedCornerCount - remainingRaw);
                    for (int q = 1; q <= inserts; ++q) corners.push_back(mixVertex(prev, cur, static_cast<double>(q) / (inserts + 1)));
                }
                corners.push_back(cur);
            }
            strokeDrapedCornerCount += corners.size(); // @Jin 统计原始+加密角点；预算耗尽后后续线段不再加密。
            if (strokeDrapedCornerCount >= kDrapeStrokeCornerBudget) strokeBudgetHit = true;
            std::vector<Vec3> pts;
            std::vector<DrapeVertex> coloredCorners = corners;
            pts.reserve(coloredCorners.size());
            for (const auto &cv : coloredCorners) {
                const Vec3 p = cartFor(cv);
                globeAabbExtend(bb, p); // @Jin AABB 用绝对 ECEF 做视锥剔除
                // @Jin 修复：stroke 三角带此前误存绝对 ECEF（约 6.37e6 米），shader 再叠加
                // @Jin uOffset=anchor-eye 后位置完全错误、整条线被视锥剔除而不可见；与 fill/point
                // @Jin 同口径改为存相对图层锚点的 RTC 坐标。
                pts.push_back(Vec3{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z});
            }
            if (!verts.empty() && !pts.empty()) {
                // @Jin chunk 内不同 range 继续用退化桥连接，保持单次 GL_TRIANGLE_STRIP 合批。
                const size_t last = verts.size() - kGlobeStrokeFloatsPerVertex;
                for (int k = 0; k < kGlobeStrokeFloatsPerVertex; ++k) verts.push_back(verts[last + k]);
                const float *bridgeColor = src.data() + static_cast<size_t>(firstVert) * kLineFloatsPerVertex;
                pushGlobeStrokeVert(verts, pts.front(), pts.front(), pts.front(), +1.0f,
                                    bridgeColor[4], bridgeColor[5], bridgeColor[6], bridgeColor[7], 1.0f);
            }
            for (size_t i = 0; i < pts.size(); ++i) {
                const Vec3 &prevV = (i > 0) ? pts[i - 1] : pts[i];
                const Vec3 &curV = pts[i];
                const Vec3 &nextV = (i + 1 < pts.size()) ? pts[i + 1] : pts[i];
                const DrapeVertex &cv = coloredCorners[i];
                pushGlobeStrokeVert(verts, prevV, curV, nextV, +1.0f,
                                    cv.rgba[0], cv.rgba[1], cv.rgba[2], cv.rgba[3], 1.0f);
                pushGlobeStrokeVert(verts, prevV, curV, nextV, -1.0f,
                                    cv.rgba[0], cv.rgba[1], cv.rgba[2], cv.rgba[3], 1.0f);
            }
        }
        if (!verts.empty()) chunks.push_back(uploadGlobeChunk(verts, bb, kGlobeStrokeFloatsPerVertex));
    };
    // @Jin drape35 实验（可回退）：面外边界改成真正铺在地表的窄三角带。
    // @Jin 原 globe stroke shader 为了恒定像素线宽，在屏幕空间横向扩边，两侧片元复用中心线深度，
    // @Jin 倾角旋转时视觉像一条面向屏幕的“飘带”。这里对内存 overlay 的 polygon outline 单独改为
    // @Jin 中心线加密后，左右边分别按经纬度偏移并重新采 DEM，使整条边界随山坡起伏。
    constexpr bool kExperimentalDrapedConformalOutline = true;
    constexpr size_t kDrapeConformalRibbonVertexBudget = 60000; // @Jin 每段 6 个顶点，约一万个边界中心点；保护异常复杂面边界。
    size_t conformalRibbonVertexCount = 0;
    size_t conformalRibbonRangeCount = 0;
    double conformalRibbonHalfWidthMeters = 0.0; // @Jin 记录实际世界半宽，便于真机日志核对实验线宽。
    auto buildConformalOutlineRibbons = [&]() -> bool {
        if (!kExperimentalDrapedConformalOutline || geom.outlineRanges.empty()) return false;
        if (vl.style.outlineA <= 0.0f || vl.style.outlineWidth <= 0.0f) return false;

        // @Jin 用构建时相机的斜距地面分辨率把像素线宽折算为米；缩放后不随相机实时变宽，是实验性固定世界宽度。
        const Camera camSnapshot = nav_.camera();
        const double tiltRad = camSnapshot.tilt * 3.14159265358979323846 / 180.0;
        const double slantMeters = camSnapshot.altitude / std::max(0.2, std::cos(tiltRad));
        const double metersPerPixel =
                2.0 * slantMeters * std::tan(camSnapshot.fieldOfView * 0.5 * 3.14159265358979323846 / 180.0) /
                static_cast<double>(std::max(1, viewportHeight_));
        double ribbonHalfWidthMeters = std::max(2.0, std::min(30.0,
                static_cast<double>(vl.style.outlineWidth) * metersPerPixel * 0.5));
        conformalRibbonHalfWidthMeters = ribbonHalfWidthMeters;

        auto cartForLonLat = [&](double lon, double lat, const DrapeVertex &colorSrc) -> Vec3 {
            constexpr double kPi = 3.14159265358979323846;
            const double latRad = lat * kPi / 180.0;
            const double absWx = (lon + 180.0) / 360.0;
            const double absWy = (1.0 - std::asinh(std::tan(latRad)) / kPi) * 0.5;
            DrapeVertex geoVertex = colorSrc;
            geoVertex.wx = absWx - ox;
            geoVertex.wy = absWy - oy;
            return cartFor(geoVertex);
        };

        for (size_t ri = 0; ri < geom.outlineRanges.size(); ++ri) {
            const int firstVert = geom.outlineRanges[ri].first;
            const int cornerCount = geom.outlineRanges[ri].second / 2;
            if (cornerCount < 2) continue;

            std::vector<DrapeVertex> raw;
            raw.reserve(static_cast<size_t>(cornerCount));
            for (int c = 0; c < cornerCount; ++c) {
                const float *v = geom.outlineVerts.data() +
                                 static_cast<size_t>(firstVert + 2 * c) * kLineFloatsPerVertex;
                DrapeVertex cur;
                cur.wx = v[0];
                cur.wy = v[1];
                for (int k = 0; k < 4; ++k) cur.rgba[k] = v[4 + k];
                raw.push_back(cur);
            }
            // @Jin polygon ring 一定闭合；若源顶点重复保存闭合点，先去掉再统一按环连接。
            if (raw.size() > 2) {
                const DrapeVertex &a = raw.front();
                const DrapeVertex &b = raw.back();
                if (std::abs(a.wx - b.wx) < 1e-10 && std::abs(a.wy - b.wy) < 1e-10) raw.pop_back();
            }
            if (raw.size() < 2) continue;

            std::vector<DrapeVertex> centers;
            centers.reserve(raw.size() * 4u);
            for (size_t i = 0; i < raw.size(); ++i) {
                const DrapeVertex &a = raw[i];
                const DrapeVertex &b = raw[(i + 1) % raw.size()];
                centers.push_back(a);
                if (allowStrokeDensify) {
                    // @Jin 中心线复用与原 stroke 相同的加密间隔，保证边界沿沟谷/山脊采高。
                    const double len = distanceWor(a, b);
                    int inserts = std::max(0, static_cast<int>(std::ceil(len / targetEdgeMeters)) - 1);
                    inserts = std::min(inserts, 512); // @Jin conformal ribbon 不复用旧屏幕线 64 点上限；10km 边长需要约 185 个间隔才能达到 50m 贴地密度。
                    const size_t remainingBudget =
                            kDrapeConformalRibbonVertexBudget > conformalRibbonVertexCount
                                    ? (kDrapeConformalRibbonVertexBudget - conformalRibbonVertexCount) / 6u : 0u;
                    inserts = std::min<size_t>(inserts, remainingBudget);
                    for (int q = 1; q <= inserts; ++q) {
                        centers.push_back(mixVertex(a, b, static_cast<double>(q) / (inserts + 1)));
                    }
                }
            }
            if (centers.size() < 2) continue;

            std::vector<Vec3> leftPts;
            std::vector<Vec3> rightPts;
            leftPts.resize(centers.size());
            rightPts.resize(centers.size());
            for (size_t i = 0; i < centers.size(); ++i) {
                const DrapeVertex &prev = centers[(i + centers.size() - 1) % centers.size()];
                const DrapeVertex &cur = centers[i];
                const DrapeVertex &next = centers[(i + 1) % centers.size()];
                double lonPrev = 0.0, latPrev = 0.0, lonCur = 0.0, latCur = 0.0, lonNext = 0.0, latNext = 0.0;
                toLonLat(prev.wx, prev.wy, lonPrev, latPrev);
                toLonLat(cur.wx, cur.wy, lonCur, latCur);
                toLonLat(next.wx, next.wy, lonNext, latNext);
                const double cosLat = std::max(0.15, std::cos(latCur * 3.14159265358979323846 / 180.0));
                double east = (lonNext - lonPrev) * cosLat * 111320.0;
                double north = (latNext - latPrev) * 111320.0;
                const double horizontalLen = std::sqrt(east * east + north * north);
                if (horizontalLen > 1e-6) {
                    east /= horizontalLen;
                    north /= horizontalLen;
                } else {
                    east = 1.0;
                    north = 0.0;
                }
                const double leftEast = -north;
                const double leftNorth = east;
                const double leftLon = lonCur +
                        leftEast * ribbonHalfWidthMeters / (111320.0 * cosLat);
                const double leftLat = latCur +
                        leftNorth * ribbonHalfWidthMeters / 111320.0;
                const double rightLon = lonCur -
                        leftEast * ribbonHalfWidthMeters / (111320.0 * cosLat);
                const double rightLat = latCur -
                        leftNorth * ribbonHalfWidthMeters / 111320.0;
                leftPts[i] = cartForLonLat(leftLon, leftLat, cur);
                rightPts[i] = cartForLonLat(rightLon, rightLat, cur);
            }

            std::vector<float> verts;
            double bb[6];
            globeAabbInit(bb);
            auto pushRibbonPoint = [&](const Vec3 &p, const DrapeVertex &cv) {
                globeAabbExtend(bb, p);
                pushGlobeFillVert(verts, static_cast<float>(p.x - anchor.x),
                                  static_cast<float>(p.y - anchor.y),
                                  static_cast<float>(p.z - anchor.z),
                                  cv.rgba[0], cv.rgba[1], cv.rgba[2], cv.rgba[3], 1.0f);
            };
            for (size_t i = 0; i < centers.size(); ++i) {
                const size_t j = (i + 1) % centers.size();
                if (conformalRibbonVertexCount + 6u > kDrapeConformalRibbonVertexBudget) break;
                const DrapeVertex &c0 = centers[i];
                const DrapeVertex &c1 = centers[j];
                pushRibbonPoint(leftPts[i], c0);
                pushRibbonPoint(rightPts[i], c0);
                pushRibbonPoint(leftPts[j], c1);
                pushRibbonPoint(rightPts[i], c0);
                pushRibbonPoint(rightPts[j], c1);
                pushRibbonPoint(leftPts[j], c1);
                conformalRibbonVertexCount += 6u;
            }
            if (!verts.empty()) {
                // @Jin 复用 fill 程序和 chunk：窄三角带本身就是贴地 GL_TRIANGLES，追加在填充之后以边界色覆盖。
                vl.globeFillChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeFillFloatsPerVertex));
                ++conformalRibbonRangeCount;
            }
        }
        return conformalRibbonRangeCount > 0;
    };

    // @Jin 独立线要素仍走原屏幕恒定像素宽线；面外边界优先走实验性地表三角带，失败再回退旧 stroke。
    const bool conformalOutlineReady = buildConformalOutlineRibbons();
    if (!conformalOutlineReady) {
        buildStroke(geom.outlineVerts, geom.outlineRanges, vl.globeOutlineChunks);
    }
    buildStroke(geom.lineVerts, geom.lineRanges, vl.globeLineChunks);

    auto buildPoint = [&](const DrapeVertex &v, std::vector<float> &verts, double bb[6], bool icon) {
        const Vec3 p = cartFor(v);
        globeAabbExtend(bb, p);
        const Vec3 c{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z};
        if (icon) {
            // @Jin y 从中心构图的 [-1,+1] 改为 [0,+2]，让 PNG 底部针尖落在 DEM 采样点而不是图片中心。
            pushGlobeIconVert(verts, c, -1.0f, 2.0f, 0.0f, 0.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
            pushGlobeIconVert(verts, c, 1.0f, 2.0f, 1.0f, 0.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
            pushGlobeIconVert(verts, c, 1.0f, 0.0f, 1.0f, 1.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
            pushGlobeIconVert(verts, c, -1.0f, 2.0f, 0.0f, 0.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
            pushGlobeIconVert(verts, c, 1.0f, 0.0f, 1.0f, 1.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
            pushGlobeIconVert(verts, c, -1.0f, 0.0f, 0.0f, 1.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
        } else {
            static const std::vector<std::pair<float, float>> unitCircle = [] {
                std::vector<std::pair<float, float>> pts;
                pts.reserve(kMarkerSegments + 1);
                for (int s = 0; s <= kMarkerSegments; ++s) {
                    const float a = static_cast<float>(2.0 * 3.14159265358979 * s / kMarkerSegments);
                    pts.emplace_back(std::cos(a), std::sin(a));
                }
                return pts;
            }();
            for (int s = 0; s < kMarkerSegments; ++s) {
                pushGlobePointVert(verts, c, 0.0f, 0.0f, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
                pushGlobePointVert(verts, c, unitCircle[s].first, unitCircle[s].second, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
                pushGlobePointVert(verts, c, unitCircle[s + 1].first, unitCircle[s + 1].second, v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3], 1.0f);
            }
        }
    };
    const size_t ptN = geom.pointCoords.size() / 2u;
    if (ptN > 0) {
        std::vector<float> verts;
        double bb[6];
        globeAabbInit(bb);
        const bool icon = !vl.iconRgba.empty();
        for (size_t i = 0; i < ptN; ++i) {
            DrapeVertex v;
            v.wx = geom.pointCoords[2 * i];
            v.wy = geom.pointCoords[2 * i + 1];
            v.rgba[0] = vl.style.pointR; v.rgba[1] = vl.style.pointG; v.rgba[2] = vl.style.pointB; v.rgba[3] = vl.style.pointA;
            buildPoint(v, verts, bb, icon);
        }
        if (icon) {
            if (!vl.iconTex.isValid() && !vl.iconTex.uploadRGBA(vl.iconRgba.data(), vl.iconW, vl.iconH)) {
                // @Jin 图标纹理失败时保留空 chunk 安全状态，后续可回退普通点通路排查。
            } else {
                vl.globeIconChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeIconFloatsPerVertex));
            }
        } else {
            vl.globePointChunks.push_back(uploadGlobeChunk(verts, bb, kGlobePointFloatsPerVertex));
        }
    }

    vl.globeUploadFeat = geom.featSpans.size();
    vl.globeUploadPoint = ptN;
    vl.globeUploadExtrude = geom.extrudeVerts.size();
    vl.globeDrapedTerrainSignature = overlayDrapedTerrainSignature(vl); // @Jin 记录本层实际覆盖 DEM 的签名，而不是全局 revision。
    vl.globeDraped = drapedSampleHits > 0; // @Jin drape29：当前渲染 Pass 按该标志关闭地形深度测试；旧 overlay 几何路径保持已验证行为。
    vl.globeUploadDone = true;
    vl.streamingUpload.store(false);
    const bool finiteAlt = std::isfinite(drapedAltMin) && std::isfinite(drapedAltMax);
    if (fillBudgetHit || strokeBudgetHit) {
        LOGI("[GlobeVec] @Jin draped large-layer guard spanKm=%.1f edgeM=%.1f fillVerts=%zu/%zu fillGuard=%d strokeCorners=%zu/%zu strokeGuard=%d", spanMeters / 1000.0, targetEdgeMeters, fillDrapedVertexCount, kDrapeFillVertexBudget, fillBudgetHit ? 1 : 0, strokeDrapedCornerCount, kDrapeStrokeCornerBudget, strokeBudgetHit ? 1 : 0);
    }
    LOGI("[GlobeVec] @Jin overlay DEM draped upload sig=%llu hits=%d/%d alt=[%.1f,%.1f] fill=%zu outline=%zu line=%zu pts=%zu conformalOutlineRanges=%zu ribbonVerts=%zu ribbonHalfM=%.1f",
         static_cast<unsigned long long>(vl.globeDrapedTerrainSignature), drapedSampleHits, drapedSampleTotal,
         finiteAlt ? drapedAltMin : 0.0, finiteAlt ? drapedAltMax : 0.0,
         vl.globeFillChunks.size(), vl.globeOutlineChunks.size(), vl.globeLineChunks.size(), ptN,
         conformalRibbonRangeCount, conformalRibbonVertexCount, conformalRibbonHalfWidthMeters);
}

void Renderer::uploadGlobeVecChunks(VectorLayer &vl) {
    if (vl.globeUploadDone) return;
    // @Jin drape36：动态 overlay、在线 GeoJSON 大面、本地纯线层仅在当前缓存存在精细相交 DEM 时烘焙；地形加载前先走平面路径。
    const bool localDrapedLine = isExperimentalLocalDrapedLineLayer(vl); // @Jin drape36：本地纯线实验候选。
    const bool largeDrapedVector = vl.remoteVectorSource || localDrapedLine; // @Jin drape36：两类大跨度文件层共享门限和运动延迟。
    const bool candidateTerrainDrape =
            vl.isOverlay || (kExperimentalRemoteVectorTerrainDrape && vl.remoteVectorSource) ||
            localDrapedLine; // @Jin drape36：普通本地纯线进入 DEM 贴地候选。
    const double currentCameraAlt = nav_.camera().altitude;
    const bool cameraWithinLargeDrapeAltitude =
            vl.isOverlay || currentCameraAlt <= kRemoteDrapeMaxCameraAltMeters; // @Jin drape36：overlay 不受高度门限影响；远程大面/本地大线高空降级。
    const bool deferLargeDrapeDuringMotion = largeDrapedVector &&
                                             (nav_.isInteracting() || cameraFlightBudgetActive()); // @Jin drape36：手势/flyTo 中先上低成本平面 VBO，停下后再烘焙 DEM。
    const bool provisionalLocalLineAboveTerrain =
            localDrapedLine && cameraWithinLargeDrapeAltitude && !terrainCache_.empty(); // @Jin drape36c：已有地形数据但精细 DEM 未齐/仍在 flyTo 时，临时平面线先盖过地形，避免一批批地形三角面把线反复埋住。
    const bool wantsTerrainDrape = candidateTerrainDrape && cameraWithinLargeDrapeAltitude &&
                                   !deferLargeDrapeDuringMotion && overlayHasTerrainCoverage(vl);
    if (wantsTerrainDrape) {
        const bool hasPartialPlaneChunks =
                !vl.globeFillChunks.empty() || !vl.globeOutlineChunks.empty() ||
                !vl.globeLineChunks.empty() || !vl.globePointChunks.empty() ||
                !vl.globeIconChunks.empty() || !vl.globeExtrudeChunks.empty();
        if (hasPartialPlaneChunks) {
            // @Jin 手势/flyTo 中可能已经上传了一部分平面 VBO；动作停止转为 DEM draped 时必须先删除，
            // @Jin 避免两套 chunk 同时绘制而产生重复面、杂线或深度竞争。
            resetVectorUpload(vl, /*del=*/true);
        }
        if (!vl.isOverlay) vl.globeRemoteLargeDrapeMode = largeDrapedVector; // @Jin drape36：记录本批文件层 VBO 是否为低空 draped 结果。
        if (vl.isOverlay) {
            // @Jin drape29：内存点/线/面/图标走已验证旧函数；URL 大面才走实验性远程贴地函数。
            uploadGlobeOverlayDrapedChunks(vl);
        } else {
            // @Jin drape36：该函数仍保留旧函数名，实际同时服务在线 GeoJSON 大面和普通本地纯线层。
            uploadGlobeRemoteDrapedChunks(vl);
        }
        return;
    }
    vl.globeDraped = false; // @Jin 未走贴地烘焙时恢复旧绘制状态，避免图层源切换或上下文重建后残留深度策略。
    if (!vl.isOverlay) vl.globeRemoteLargeDrapeMode = (currentCameraAlt <= kRemoteDrapeMaxCameraAltMeters); // @Jin drape36：高空记录为平面模式；低空但暂无 DEM 也标记为低空等待态，避免每帧按高度门限误重置。
    const VectorGeometry &geom = vl.geom;
    const double ox = geom.originWx;
    const double oy = geom.originWy;
    // 固定锚点 = 几何原点的 ECEF（贴地 alt=0，与相机/高度完全无关）：顶点烘焙为 (ecef − anchor)，纯几何，
    // 平移与缩放都零重建零重传；eye 平移经每帧 uOffset=anchor−eye、贴地高度经 uVecAlt 沿法向在着色器补回
    // （shader 位置 = (ecef−anchor)+(anchor−eye)+normalize(ecef)*uVecAlt = (ecef−eye)+法向*vecAlt，与旧「减 eye +
    //   g2c(alt=vecAlt)」等价；ecef≈aPos+anchor 仅用于求单位法向，float 量级 0.5m 误差对法向无感）。
    const Vec3 anchor = worldXYToCartesian(ox, oy, 0.0);
    vl.globeAnchor = anchor;
    // 本层是否带逐顶点高程：true 时按真实 alt 烘焙 ECEF、并逐顶点传 mode 门控着色器 uVecAlt；
    // false 时并行高程数组皆空，走 alt=0/mode=0（等价旧「整层贴地抬升」，零额外内存/耗时）。
    const bool useElev = geom.hasElevation();

    // 分帧预算：与 2D 同「每组必成 + 超时即停」——每帧至少完整推进一个要素组（或点组）再查
    // deadline，超了让位本帧，由调用侧（renderGlobeVectors3D Pass 1）置 needRedraw 下帧续传。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUploadFrameBudgetMs);
    const size_t nSpans = geom.featSpans.size();

    // ---- 路 A：要素组流式上传（按 geom.featSpans 推进，同要素 fill/描边/线同组落块 → 图斑整块长出）----
    for (;;) {
        if (vl.globeUploadFeat >= nSpans) break;
        // 组界 [first, next)：按源(2D RTC) floats 累计 ≈ kGlobeChunkSrcFloats；至少含一个要素
        //（巨型单要素超预算也自成一组，保证每帧有进展），越预算即停不拆要素。
        const size_t first = vl.globeUploadFeat;
        size_t next = first;
        size_t srcFloats = 0;
        do {
            const FeatureSpans &s = geom.featSpans[next];
            size_t add = static_cast<size_t>(s.fillCount);
            for (int i = 0; i < s.outRangeCount; ++i)
                add += static_cast<size_t>(geom.outlineRanges[s.outRangeStart + i].second) * kLineFloatsPerVertex;
            for (int i = 0; i < s.lineRangeCount; ++i)
                add += static_cast<size_t>(geom.lineRanges[s.lineRangeStart + i].second) * kLineFloatsPerVertex;
            if (next > first && srcFloats + add > kGlobeChunkSrcFloats) break;
            srcFloats += add;
            ++next;
        } while (next < nSpans && srcFloats < kGlobeChunkSrcFloats);
        const FeatureSpans &sF = geom.featSpans[first];
        const FeatureSpans &sL = geom.featSpans[next - 1];

        // fill: [x,y,z,r,g,b,a] GL_TRIANGLES——组切片逐顶点转 (ECEF − anchor)，同步累加世界 ECEF AABB
        const size_t fStart = static_cast<size_t>(sF.fillStart);
        const size_t fEnd = static_cast<size_t>(sL.fillStart) + static_cast<size_t>(sL.fillCount);
        if (fEnd > fStart) {
            std::vector<float> verts;
            double bb[6];
            globeAabbInit(bb);
            verts.reserve((fEnd - fStart) / kColorFloatsPerVertex * kGlobeFillFloatsPerVertex);
            for (size_t i = fStart; i + (size_t)kColorFloatsPerVertex - 1 < fEnd; i += kColorFloatsPerVertex) {
                const size_t vi = i / kColorFloatsPerVertex;
                const float alt = useElev ? geom.fillAlt[vi] : 0.0f;
                const float md = useElev ? geom.fillMode[vi] : 0.0f;
                const Vec3 p = worldXYToCartesian(ox + static_cast<double>(geom.fillVerts[i]),
                                                  oy + static_cast<double>(geom.fillVerts[i + 1]), alt);
                globeAabbExtend(bb, p);
                pushGlobeFillVert(verts, static_cast<float>(p.x - anchor.x),
                                  static_cast<float>(p.y - anchor.y),
                                  static_cast<float>(p.z - anchor.z), geom.fillVerts[i + 2],
                                  geom.fillVerts[i + 3], geom.fillVerts[i + 4], geom.fillVerts[i + 5], md);
            }
            // 硬上限安全网：触顶丢块但游标照常推进（预算已随几何有界，此处仅防程序错误）
            if (vl.globeFillChunks.size() < kGlobeMaxChunks)
                vl.globeFillChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeFillFloatsPerVertex));
        }

        // stroke (outline / line): [pPrev3, pCur3, pNext3, side, color4] GL_TRIANGLE_STRIP——从 2D miter
        // 三角带反推 corner 中心序列（每角 2 顶点同 (x,y)，side=±(mx,my)），逐 range 转换后组内合批：
        // 相邻 range 间插「上段末顶点 + 本段首顶点」两重复顶点退化桥（同 2D buildStripBatch 配方），
        // 整 chunk 单条 strip 一次 draw（替代旧逐 range draw：10 万图斑 = 每帧 10 万次 draw call）。
        // 桥三角形恒含两个 gl_Position 完全相同的顶点 → 零面积；stroke 着色器的 cur 塌缩只移动第三个
        // 顶点救不活它，故旧配方「末顶点点重复×2」的跨 range 斜穿撕裂不会复发。
        // 闭合环检测/封口/端点退化沿用旧版：首末 corner 同点规为 closed，只发 cornerN-1 个角再重复
        // 角 0 封口、prev/next 环绕；开放线端点 prev/next 退化到 cur。
        auto buildStrokeGroup = [&](const std::vector<float> &srcVerts,
                                    const std::vector<std::pair<int, int>> &ranges,
                                    size_t fromRange, size_t toRange,
                                    const std::vector<float> &altArr,
                                    const std::vector<float> &modeArr,
                                    std::vector<VectorLayer::GlobeChunk> &chunks,
                                    bool separateRanges) {
            std::vector<float> verts;
            std::vector<float> rangeVerts;
            std::vector<Vec3> pts; // 单 range 的 corner 序列（逐 range 复用）
            double bb[6];
            globeAabbInit(bb);
            bool firstRange = true;
            for (size_t ri = fromRange; ri < toRange; ++ri) {
                double rangeBB[6]; // @Jin 独立 strip chunk 需要各自 AABB，视锥剔除不能沿用整层合批包围盒。
                globeAabbInit(rangeBB);
                const int firstVert = ranges[ri].first;
                const int vertCount = ranges[ri].second;
                const int cornerN = vertCount / 2;
                if (cornerN < 2) continue; // 退化段（零面积要素的空 range）：整体跳过
                rangeVerts.clear();
                const float *vFirst = srcVerts.data() + size_t(firstVert) * kLineFloatsPerVertex;
                const float *vLast = srcVerts.data() + size_t(firstVert + 2 * (cornerN - 1)) * kLineFloatsPerVertex;
                const bool closed = (cornerN >= 3) &&
                                    (vFirst[0] == vLast[0]) && (vFirst[1] == vLast[1]);
                const int emitN = closed ? (cornerN - 1) : cornerN;
                pts.resize(static_cast<size_t>(cornerN));
                for (int c = 0; c < cornerN; ++c) {
                    const int vertIdx = firstVert + 2 * c; // 每角首顶点（角两顶点共享同高程）
                    const float *v = srcVerts.data() + size_t(vertIdx) * kLineFloatsPerVertex;
                    const float alt = useElev ? altArr[vertIdx] : 0.0f;
                    const Vec3 p = worldXYToCartesian(ox + static_cast<double>(v[0]),
                                                      oy + static_cast<double>(v[1]), alt);
                    globeAabbExtend(bb, p);
                    if (separateRanges) globeAabbExtend(rangeBB, p); // @Jin 独立环只累计本环 AABB。
                    pts[c] = Vec3{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z};
                }
                // 闭合环封口：strip 线性序列 n 角只生 n-1 段 ribbon，缺「末角→首角」封口边；闭合时
                // 额外再发角 0（与角 0 同三元组 → 带尾四边形把 pts[n-1] 连回 pts[0]），对齐 2D 口径。
                const int emitCount = closed ? (emitN + 1) : emitN;
                for (int c = 0; c < emitCount; ++c) {
                    const int ci = closed ? (c % emitN) : c; // 闭合的最后一帧 c==emitN → ci=0（重复首角）
                    const Vec3 &cur = pts[ci];
                    Vec3 prevV, nextV;
                    if (closed) {
                        prevV = (ci > 0) ? pts[ci - 1] : pts[emitN - 1];   // 开头环回到倒数第二个真实角
                        nextV = (ci + 1 < emitN) ? pts[ci + 1] : pts[0];   // 末尾环回到角 0
                    } else {
                        prevV = (ci > 0) ? pts[ci - 1] : cur;              // 开放线：退化到 cur（shader AB=0→同 BC）
                        nextV = (ci + 1 < emitN) ? pts[ci + 1] : cur;
                    }
                    const int vertIdx = firstVert + 2 * ci;
                    const float *v = srcVerts.data() + size_t(vertIdx) * kLineFloatsPerVertex;
                    const float md = useElev ? modeArr[vertIdx] : 0.0f;
                    pushGlobeStrokeVert(rangeVerts, prevV, cur, nextV, +1.0f, v[4], v[5], v[6], v[7], md);
                    pushGlobeStrokeVert(rangeVerts, prevV, cur, nextV, -1.0f, v[4], v[5], v[6], v[7], md);
                }
                if (rangeVerts.empty()) continue;
                if (separateRanges) {
                    if (chunks.size() < kGlobeMaxChunks) {
                        VectorLayer::GlobeChunk chunk = uploadGlobeChunk(rangeVerts, rangeBB, kGlobeStrokeFloatsPerVertex); // @Jin 外环/洞环物理分离，不插入任何跨环顶点。
                        chunk.terrainDraped = chunk.terrainDraped || provisionalLocalLineAboveTerrain; // @Jin drape36c：过渡期平面线 chunk 临时关闭对地形的深度测试。
                        chunks.push_back(chunk);
                    }
                    continue;
                }
                verts.reserve(verts.size() + rangeVerts.size() + 2u * kGlobeStrokeFloatsPerVertex);
                if (!firstRange) {
                    // 退化桥：复制「上段末顶点（verts 尾部）+ 本段首顶点（rangeVerts 头部）」。用
                    // push_back 尾索引读取（读恒落旧元素范围，即便扩容中途也安全），不用自范围 insert。
                    const size_t prevLast = verts.size() - kGlobeStrokeFloatsPerVertex;
                    for (int k = 0; k < kGlobeStrokeFloatsPerVertex; ++k)
                        verts.push_back(verts[prevLast + k]);
                    for (int k = 0; k < kGlobeStrokeFloatsPerVertex; ++k)
                        verts.push_back(rangeVerts[k]);
                }
                verts.insert(verts.end(), rangeVerts.begin(), rangeVerts.end());
                firstRange = false;
            }
            if (!verts.empty() && chunks.size() < kGlobeMaxChunks) {
                VectorLayer::GlobeChunk chunk = uploadGlobeChunk(verts, bb, kGlobeStrokeFloatsPerVertex);
                chunk.terrainDraped = chunk.terrainDraped || provisionalLocalLineAboveTerrain; // @Jin drape36c：过渡期合批平面线同样临时保持在地表之上。
                chunks.push_back(chunk);
            }
        };
        // 描边/线组切片 = 首末要素的 range 区间（零长要素无数据，跨越无害，同 2D 口径）
        const size_t oStart = static_cast<size_t>(sF.outRangeStart);
        const size_t oEnd = static_cast<size_t>(sL.outRangeStart) + static_cast<size_t>(sL.outRangeCount);
        const bool separateOutlineRanges = (vl.remoteVectorSource || localDrapedLine) && (oEnd - oStart) <= 64; // @Jin drape36：运动中临时平面线也保持分环独立。
        buildStrokeGroup(geom.outlineVerts, geom.outlineRanges, oStart, oEnd,
                         geom.outlineAlt, geom.outlineMode, vl.globeOutlineChunks, separateOutlineRanges);
        const size_t lStart = static_cast<size_t>(sF.lineRangeStart);
        const size_t lEnd = static_cast<size_t>(sL.lineRangeStart) + static_cast<size_t>(sL.lineRangeCount);
        const bool separateLineRanges = (vl.remoteVectorSource || localDrapedLine) && (lEnd - lStart) <= 64; // @Jin drape36：本地纯线的临时平面 VBO 也不做跨段桥接。
        buildStrokeGroup(geom.lineVerts, geom.lineRanges, lStart, lEnd,
                         geom.lineAlt, geom.lineMode, vl.globeLineChunks, separateLineRanges);
        vl.globeUploadFeat = next;
        if (std::chrono::steady_clock::now() >= deadline) break; // 超时即停：已成组不回退
    }

    // ---- 路 B：点圆 / 图标流式（两路互斥，共用 globeUploadPoint 游标逐点推进）----
    // 点圆：每点展开 kMarkerSegments 个三角形（圆心 + 相邻两环点），pos = 点 ECEF − anchor、corner = 单位
    // 方向，屏幕半径/贴地高度每帧 uniform → 顶点纯几何，平移缩放零重建（口径同旧）。
    const size_t ptN = geom.pointCoords.size() / 2;
    if (vl.iconRgba.empty()) {
        static const std::vector<std::pair<float, float>> unitCircle = [] {
            std::vector<std::pair<float, float>> pts;
            pts.reserve(kMarkerSegments + 1);
            for (int s = 0; s <= kMarkerSegments; ++s) {
                const float a = static_cast<float>(2.0 * 3.14159265358979 * s / kMarkerSegments);
                pts.emplace_back(std::cos(a), std::sin(a));
            }
            return pts;
        }();
        const float pr = vl.style.pointR, pg = vl.style.pointG, pb = vl.style.pointB, pa = vl.style.pointA;
        while (vl.globeUploadPoint < ptN) {
            const size_t ptFirst = vl.globeUploadPoint;
            const size_t ptEnd = std::min(ptFirst + kGlobeChunkPoints, ptN);
            std::vector<float> verts;
            double bb[6];
            globeAabbInit(bb);
            verts.reserve((ptEnd - ptFirst) * kMarkerSegments * 3 * kGlobePointFloatsPerVertex);
            for (size_t i = ptFirst; i < ptEnd; ++i) {
                const float alt = useElev ? geom.pointAlt[i] : 0.0f;
                const float md = useElev ? geom.pointMode[i] : 0.0f;
                const Vec3 p = worldXYToCartesian(ox + static_cast<double>(geom.pointCoords[2 * i]),
                                                  oy + static_cast<double>(geom.pointCoords[2 * i + 1]), alt);
                globeAabbExtend(bb, p);
                const Vec3 c{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z};
                for (int s = 0; s < kMarkerSegments; ++s) {
                    pushGlobePointVert(verts, c, 0.0f, 0.0f, pr, pg, pb, pa, md);
                    pushGlobePointVert(verts, c, unitCircle[s].first, unitCircle[s].second, pr, pg, pb, pa, md);
                    pushGlobePointVert(verts, c, unitCircle[s + 1].first, unitCircle[s + 1].second, pr, pg, pb, pa, md);
                }
            }
            vl.globeUploadPoint = ptEnd;
            if (vl.globePointChunks.size() < kGlobeMaxChunks)
                vl.globePointChunks.push_back(uploadGlobeChunk(verts, bb, kGlobePointFloatsPerVertex));
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    } else {
        // 图标 billboard [pos3, corner2, uv2, color4]：每点 2 三角形 6 顶点（角点∈[-1,1]，uv v=0 配
        // corner.y=+1 保正立），仅 iconRgba 非空层进入（与点圆互斥，分流口径同旧版）。
        while (vl.globeUploadPoint < ptN) {
            const size_t ptFirst = vl.globeUploadPoint;
            const size_t ptEnd = std::min(ptFirst + kGlobeChunkPoints, ptN);
            std::vector<float> verts;
            double bb[6];
            globeAabbInit(bb);
            verts.reserve((ptEnd - ptFirst) * 6 * kGlobeIconFloatsPerVertex);
            for (size_t i = ptFirst; i < ptEnd; ++i) {
                const float alt = useElev ? geom.pointAlt[i] : 0.0f;
                const float md = useElev ? geom.pointMode[i] : 0.0f;
                const Vec3 p = worldXYToCartesian(ox + static_cast<double>(geom.pointCoords[2 * i]),
                                                  oy + static_cast<double>(geom.pointCoords[2 * i + 1]), alt);
                globeAabbExtend(bb, p);
                const Vec3 c{p.x - anchor.x, p.y - anchor.y, p.z - anchor.z};
                pushGlobeIconVert(verts, c, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 左上
                pushGlobeIconVert(verts, c, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, md);  // 右上
                pushGlobeIconVert(verts, c, 1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 右下
                pushGlobeIconVert(verts, c, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 左上
                pushGlobeIconVert(verts, c, 1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 右下
                pushGlobeIconVert(verts, c, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, md); // 左下
            }
            vl.globeUploadPoint = ptEnd;
            if (vl.globeIconChunks.size() < kGlobeMaxChunks)
                vl.globeIconChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeIconFloatsPerVertex));
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }

    // ---- 路 C：extrude 侧墙流式（独立游标 globeUploadExtrude，按 geom.extrudeVerts float 数推进）----
    // 布局同 fill：[x,y,r,g,b,a] + 并行 alt/mode → 逐顶点 worldXYToCartesian(alt) 烘 ECEF−anchor，
    // mode 恒 1（baked 真实高程，不叠加 uVecAlt：墙底 alt0 落地面、墙顶落真实高程）。数据量通常很小
    //（一栋建筑几十顶点），仍按 kGlobeChunkSrcFloats 分块保证大数据不冻结单帧。
    {
        const size_t exN = geom.extrudeVerts.size();
        while (vl.globeUploadExtrude < exN) {
            std::vector<float> verts;
            double bb[6];
            globeAabbInit(bb);
            size_t i = vl.globeUploadExtrude;
            size_t budget = 0;
            for (; i + (size_t)kColorFloatsPerVertex - 1 < exN; i += kColorFloatsPerVertex) {
                const size_t vi = i / kColorFloatsPerVertex;
                const float alt = geom.extrudeAlt[vi];
                const float md = geom.extrudeMode[vi];
                const Vec3 p = worldXYToCartesian(ox + static_cast<double>(geom.extrudeVerts[i]),
                                                  oy + static_cast<double>(geom.extrudeVerts[i + 1]), alt);
                globeAabbExtend(bb, p);
                pushGlobeFillVert(verts, static_cast<float>(p.x - anchor.x),
                                  static_cast<float>(p.y - anchor.y),
                                  static_cast<float>(p.z - anchor.z), geom.extrudeVerts[i + 2],
                                  geom.extrudeVerts[i + 3], geom.extrudeVerts[i + 4], geom.extrudeVerts[i + 5], md);
                budget += kColorFloatsPerVertex;
                if (budget >= kGlobeChunkSrcFloats) break;
            }
            vl.globeUploadExtrude = i;
            if (!verts.empty() && vl.globeExtrudeChunks.size() < kGlobeMaxChunks)
                vl.globeExtrudeChunks.push_back(uploadGlobeChunk(verts, bb, kGlobeFillFloatsPerVertex));
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }

    // ---- 完成判定：要素组与点组两游标均到尾（不动 2D uploadDone/glReady，回 2D 仍按原口径流式）----
    if (vl.globeUploadFeat >= nSpans && vl.globeUploadPoint >= ptN &&
        vl.globeUploadExtrude >= geom.extrudeVerts.size()) {
        vl.globeUploadDone = true;
        if (candidateTerrainDrape) {
            // @Jin 无 DEM 时生成的是平面 VBO，也要记录当前签名；后续第一批地形到达后可稳定触发重建。
            vl.globeDrapedTerrainSignature = overlayDrapedTerrainSignature(vl);
            vl.globeDrapedPendingSignature = 0;
            vl.globeDrapedSignaturePending = false;
        }
        // streamingUpload 为两通路共用的 UI 标志：几何接管时置位，此处归零 → 加载胶囊覆盖 3D 流式全程
        vl.streamingUpload.store(false);
        LOGI("[GlobeVec] 3D 流式上传完成: feats=%zu fillChunks=%zu outlineChunks=%zu lineChunks=%zu pointChunks=%zu iconChunks=%zu pts=%zu cost=%lldms",
             nSpans, vl.globeFillChunks.size(), vl.globeOutlineChunks.size(),
             vl.globeLineChunks.size(), vl.globePointChunks.size(), vl.globeIconChunks.size(), ptN,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - vl.uploadStartedAt).count());
    }
}

bool Renderer::renderGlobeVectors3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc) {
    if (vectorLayers_.empty()) return false;
    bool needRedraw = false;
    const int camLevel = nav_.displayLevel();
    const float halfVpX = viewportWidth_ > 0 ? static_cast<float>(viewportWidth_) : 1.0f;
    const float halfVpY = viewportHeight_ > 0 ? static_cast<float>(viewportHeight_) : 1.0f;

    // 贴地高度（米）按弦切矢高口径动态取 camAlt²/2e7 钳 [2,500]（缘由见 kGlobeVecAltMeters 注释），
    // 每帧作 uniform uVecAlt 传着色器沿法向抬升（顶点纯几何 alt=0 不含此高度）→ 缩放零重建。
    // 低空仅米级：tilt 斜视下贴地层视觉落在椭球面上，不再「悬浮」。
    const double camAlt = (cam.eye - cam.center).length();
    double rawVecAlt = camAlt * camAlt / 2.0e7;
    if (rawVecAlt > kGlobeVecAltMeters) rawVecAlt = kGlobeVecAltMeters;
    if (rawVecAlt < 2.0) rawVecAlt = 2.0;
    const float vecAlt = static_cast<float>(rawVecAlt);

    // Pass 1：逐可见矢量层 drainReady → P2.e 分帧流式上传（未完每帧推进一个预算片并续画一帧）
    for (auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (vl.dead) continue; // 墓碑层交给 2D drawVectorLayers 回收（同一条 resetVectorUpload）
        if (vl.loader) {
            VectorGeometry g;
            if (vl.loader->drainReady(g)) {
                resetVectorUpload(vl, /*del=*/true); // 2D/3D chunk 与游标一并回收，下帧起据新 geom 流式上传
                vl.geom = std::move(g);
                if (!vl.geom.empty() && vectorLayerShown(vl, camLevel)) {
                    vl.uploadStartedAt = std::chrono::steady_clock::now();
                    vl.streamingUpload.store(true);
                }
            }
            if (vl.loader->hasPendingOrReady()) needRedraw = true;
        }
        if (!vectorLayerShown(vl, camLevel) || vl.geom.empty()) continue;
        const bool localDrapedLine = isExperimentalLocalDrapedLineLayer(vl); // @Jin drape36：每帧用几何内容判断是否为本地纯线实验层。
        const bool largeDrapedVector = vl.remoteVectorSource || localDrapedLine; // @Jin drape36：大面/大线共用签名防抖和高度门限。
        const bool usesTerrainDrapedChunks =
            vl.isOverlay || (kExperimentalRemoteVectorTerrainDrape && vl.remoteVectorSource) ||
            localDrapedLine; // @Jin drape36：精细地形到达后允许触发本地纯线重建。
        // @Jin DEM 批量到达时签名会连续变化；大图层等待签名稳定后再重建，避免每到一块地形就烘焙一次省级面/省界。
        if (usesTerrainDrapedChunks && vl.globeUploadDone) {
            const uint64_t nextSignature = overlayDrapedTerrainSignature(vl);
            const auto now = std::chrono::steady_clock::now();
            const bool zoomAllowsLargeDrape = vl.isOverlay || camAlt <= kRemoteDrapeMaxCameraAltMeters; // @Jin drape36：overlay 不限高度，大面/本地大线使用同一低空门限。
            // @Jin drape36：记录相机是否仍在运动；大面/本地省级线只在手势停止后重建，避免地形加载滑动时同步卡帧。
            const Vec3 camMove = cam.eye - vl.globeLastDrapeCameraEye;
            const Vec3 targetMove = cam.center - vl.globeLastDrapeCameraCenter;
            const bool cameraMoved = !vl.globeDrapeCameraMoveInitialized ||
                                     camMove.length() > 0.5 || targetMove.length() > 0.5;
            if (cameraMoved) {
                vl.globeLastCameraMoveAt = now;
                vl.globeLastDrapeCameraEye = cam.eye;
                vl.globeLastDrapeCameraCenter = cam.center;
                vl.globeDrapeCameraMoveInitialized = true;
            }
            if (largeDrapedVector && zoomAllowsLargeDrape != vl.globeRemoteLargeDrapeMode) { // @Jin drape36：跨越 500km 门限时重建远程大面或本地大线。
                // @Jin drape36：高空切低成本平面，低空停顿后切 DEM draped，避免 flyTo 跨越门限时立即重烘焙。
                const bool largeVectorMoving =
                        now - vl.globeLastCameraMoveAt < std::chrono::milliseconds(450); // @Jin drape36：运动中仅请求下一帧，停止后再重建。
                if (!(zoomAllowsLargeDrape && largeVectorMoving)) {
                    resetVectorUpload(vl, /*del=*/true);
                    vl.globeDrapedTerrainSignature = 0;
                    vl.globeDrapedSignaturePending = false;
                    vl.globeDrapedPendingSignature = 0;
                    needRedraw = true;
                } else {
                    needRedraw = true;
                }
            }
            if (nextSignature != vl.globeDrapedTerrainSignature) {
                if (!vl.globeDrapedSignaturePending || vl.globeDrapedPendingSignature != nextSignature) {
                    vl.globeDrapedSignaturePending = true;
                    vl.globeDrapedPendingSignature = nextSignature;
                    vl.globeDrapedSignatureChangedAt = now;
                }
                constexpr auto kRemoteVectorDrapeRebuildDelay = std::chrono::milliseconds(1200); // @Jin 地形批量到达后等待签名稳定。
                const bool largeVectorMoving = largeDrapedVector &&
                                                  now - vl.globeLastCameraMoveAt < std::chrono::milliseconds(450); // @Jin drape36：本地省级线也等待手势停下后再付出烘焙成本。
                if (!largeVectorMoving &&
                    now - vl.globeDrapedSignatureChangedAt >= kRemoteVectorDrapeRebuildDelay) {
                    resetVectorUpload(vl, /*del=*/true);
                    vl.globeDrapedTerrainSignature = 0;
                    vl.globeDrapedSignaturePending = false;
                    vl.globeDrapedPendingSignature = 0;
                }
                needRedraw = true;
            } else if (vl.globeDrapedSignaturePending) {
                vl.globeDrapedSignaturePending = false;
                vl.globeDrapedPendingSignature = 0;
            }
        }
        // P2.e 分帧流式上传：未完成每帧推进一个预算片并请求续绘（图斑整块整块长出）；
        // 完成后顶点纯几何（相对固定锚点、静态高程烘焙），平移与缩放均零重建零重传。
        if (!vl.globeUploadDone) {
            uploadGlobeVecChunks(vl);
            if (usesTerrainDrapedChunks && vl.globeUploadDone) {
                // @Jin 重建完成后清掉防抖状态；实际签名由具体上传通路写入。
                vl.globeDrapedSignaturePending = false;
                vl.globeDrapedPendingSignature = 0;
            }
            needRedraw = true;
        }
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    // 参照 参考实现 DrawableShape 的 enableDepthWrite=false + 画家序原理：矢量层之间**关深度写入、开深度测试**。
    // 深度测试仍对着地球瓦片网格 → 背半球/穿地球的矢量照样被挡；但矢量彼此不写深度，重叠区不再逐像素
    // 随机胜负（旧 bug：两层 fill 压到同一 vecAlt 面且都写深度 → GL_LEQUAL 共面 fight），改为纯按图层
    // 加入顺序后画覆盖前画合成。fill 的 polygonOffset(+2) 仅作 fill 相对瓦片的稳定偏置（stroke 因不测
    // 矢量深度恒在 fill 上，双保险）。
    glDepthMask(GL_FALSE);

    // Pass 2：fill（GL_TRIANGLES，开 polygon offset）
    if (globeFillProgram_.program() != 0) {
        globeFillProgram_.use();
        glUniformMatrix4fv(gfUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glEnable(GL_POLYGON_OFFSET_FILL);
        // 方向关键：GL 深度 0=near 1=far，polygonOffset 把 (factor*slope + units*min_res) **加**到
        // 片元深度上。fill 与 stroke 同 alt=500m，Pass 2 先画 fill 写 depth=fill_actual+offset，
        // Pass 3 后画 stroke 用 GL_LEQUAL 比较 stroke_actual ≤ stored 才通得过——必须 stored 稍
        // 远（正 offset），否则 stroke 被自己那层 fill 挡回去（现象：多边形 interior 描边全丢，
        // 只剩最外圈贴瓦片的那条边可见，看起来像 "要素撕裂")。
        // 之前用 (-1, -1) 意图 "fill 推得更近避免与瓦片 z-fight" 是错的方向：fill 已经在 alt=500m
        // 远高于瓦片 alt=0，本就不需负 offset 兜底；反而害了 stroke。改成 (+2, +2)：正 push 让 fill
        // 稍远，stroke 稳过；量级 2 units + 2*slope 远小于 500m alt 对应的深度差，不会被瓦片吃回。
        glPolygonOffset(2.0f, 2.0f);
        const GLsizei stride = kGlobeFillFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gfAPos_);
        glEnableVertexAttribArray(gfAColor_);
        glEnableVertexAttribArray(gfAMode_);
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel) || vl.globeFillChunks.empty()) continue;
            // @Jin 实验（可回退）：贴地动态叠加层与影像地形网格共面，密采沟谷的面片元会被粗地形三角面
            // @Jin 深度剔除，表现为“面破碎、一块显示一块被遮”。叠加层 depthMask=FALSE 走画家序，
            // @Jin 对 isOverlay 关闭地形深度测试让其稳定盖在地表；文件矢量仍保留深度测试。
            glUniform3f(gfUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(gfUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(gfUVecAlt_, vecAlt);
            // 逐 chunk + 世界 ECEF AABB 对 Gribb 6 面保守剔除（含 uVecAlt 抬升余量）：
            // 背半球/视外远处 chunk 不进 draw call，拉近俯仰视角下收益最大
            for (const auto &ch : vl.globeFillChunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                // @Jin drape34：内存 overlay 的整块 VBO 已按 DEM 烘焙，但不携带远程大面专用 terrainDraped 标记；
                // @Jin 因此这里对 isOverlay 整层关闭地形深度测试，避免太白山小面被较粗的地形三角面局部遮挡而“破碎”。
                // @Jin 在线 GeoJSON 仍保持逐 chunk 判断：只有真正采到 DEM 的 chunk 关闭深度，平面 chunk 继续接受地形遮挡。
                const bool skipTerrainDepthForFill = vl.globeDraped && (vl.isOverlay || ch.terrainDraped);
                if (skipTerrainDepthForFill) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST);
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(gfAPos_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gfAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(gfAMode_, 1, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(7 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, ch.vertexCount);
            }
        }
        glEnable(GL_DEPTH_TEST); // @Jin 实验后恢复深度，供 Pass 2b extrude 侧墙使用
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisableVertexAttribArray(gfAPos_);
        glDisableVertexAttribArray(gfAColor_);
        glDisableVertexAttribArray(gfAMode_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // Pass 2b：extrude 侧墙（GL_TRIANGLES，复用 fill 程序/布局）。与 flat 填充唯一区别：开【深度写入】，
    // 使同一盒体近墙遮挡远墙 → 实心不外窥（修「凹型」）。深度测试本就对着地球瓦片网格开启，背面
    // 半球/穿地的墙自然被挡；画完立即把 depthMask 复位 FALSE，后续 stroke/point 画家序合成不变。
    if (globeFillProgram_.program() != 0) {
        globeFillProgram_.use();
        glUniformMatrix4fv(gfUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glDepthMask(GL_TRUE); // 仅此 pass 写深度（当前深度函数 GL_LEQUAL、深度测试已开，与瓦片同口径）
        const GLsizei stride = kGlobeFillFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gfAPos_);
        glEnableVertexAttribArray(gfAColor_);
        glEnableVertexAttribArray(gfAMode_);
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel) || vl.globeExtrudeChunks.empty()) continue;
            glUniform3f(gfUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(gfUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(gfUVecAlt_, vecAlt);
            for (const auto &ch : vl.globeExtrudeChunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(gfAPos_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gfAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(gfAMode_, 1, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(7 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, ch.vertexCount);
            }
        }
        glDisableVertexAttribArray(gfAPos_);
        glDisableVertexAttribArray(gfAColor_);
        glDisableVertexAttribArray(gfAMode_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDepthMask(GL_FALSE); // 复位：与 fill/stroke/point 画家序保持一致
    }

    // Pass 3：outline + line（GL_TRIANGLE_STRIP，不开 polygon offset；miter 靠 shader 屏幕空间）
    if (globeStrokeProgram_.program() != 0) {
        globeStrokeProgram_.use();
        glUniformMatrix4fv(gsUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glUniform2f(gsUViewportPx_, halfVpX, halfVpY);
        glUniform1f(gsUMiterLimit_, kGlobeMiterLimit);
        const GLsizei stride = kGlobeStrokeFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gsAPrev_);
        glEnableVertexAttribArray(gsACur_);
        glEnableVertexAttribArray(gsANext_);
        glEnableVertexAttribArray(gsASide_);
        glEnableVertexAttribArray(gsAColor_);
        glEnableVertexAttribArray(gsAMode_);
        // 逐层画 outline 与 line（各自线宽）；stroke program 内 side 已带符号，线宽以像素传入。
        // P2.e：chunk 内各 range 已在上传期以退化桥合批 → 整 chunk 单次 glDrawArrays（旧逐 range
        // draw 在 10 万图斑层 = 每帧 10 万次 draw call，驱动开销即帧率主瓶颈）。
        auto drawStripChunks = [&](std::vector<VectorLayer::GlobeChunk> &chunks, float halfWidthPx,
                                    bool wholeLayerDraped) { // @Jin drape36c：整层贴地状态由逐图层循环显式传入。
            if (chunks.empty() || halfWidthPx <= 0.0f) return;
            glUniform1f(gsUHalfWidthPx_, halfWidthPx);
            for (const auto &ch : chunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                const bool skipTerrainDepthForStroke = wholeLayerDraped || ch.terrainDraped; // @Jin drape36c：最终 drape 层按整层跳过，过渡平面线按 chunk 跳过，普通线仍保留地形遮挡。
                if (skipTerrainDepthForStroke) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST);
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(gsAPrev_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gsACur_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(gsANext_, 3, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(6 * sizeof(float)));
                glVertexAttribPointer(gsASide_, 1, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(9 * sizeof(float)));
                glVertexAttribPointer(gsAColor_, 4, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(10 * sizeof(float)));
                glVertexAttribPointer(gsAMode_, 1, GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<const void *>(14 * sizeof(float)));
                glDrawArrays(GL_TRIANGLE_STRIP, 0, ch.vertexCount);
            }
        };
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel)) continue;
            // @Jin drape36c：默认恢复深度测试；具体 overlay/远程大面按 vl.globeDraped，本地纯线过渡 chunk 按 ch.terrainDraped 在 drawStripChunks 内逐块切换。
            glEnable(GL_DEPTH_TEST);
            const bool provisionalLocalLineDepthSkip = isExperimentalLocalDrapedLineLayer(vl) &&
                                                       !terrainCache_.empty(); // @Jin drape36e：z3~z5 等高空视角不做全省 DEM 重建，但只要地形已启用就临时跳过线的地形深度测试，避免粗地形瓦片遮挡省界。
            // 描边/线宽与 2D 同口径：style.outlineWidth/lineWidth 直接当像素宽度处理（无 density），
            // 取一半得像素半宽，shader 里乘 side 与 miter 后除视口得 NDC 偏移。
            // 逐层 uOffset = 本层固定锚点 − eye（平移只改此 uniform，不重建顶点）
            glUniform3f(gsUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(gsUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(gsUVecAlt_, vecAlt);
            const float oHalfPx = static_cast<float>(vl.style.outlineWidth) * 0.5f;
            const float lHalfPx = static_cast<float>(vl.style.lineWidth) * 0.5f;
            drawStripChunks(vl.globeOutlineChunks, oHalfPx, vl.globeDraped); // @Jin drape36d：本地纯线无 outline，overlay/远程大面继续使用整层策略。
            drawStripChunks(vl.globeLineChunks, lHalfPx, vl.globeDraped || provisionalLocalLineDepthSkip); // @Jin drape36d：最终 drape 或第一批 DEM 到达后的过渡期均关闭对线的地形深度遮挡。
        }
        glDisableVertexAttribArray(gsAPrev_);
        glDisableVertexAttribArray(gsACur_);
        glDisableVertexAttribArray(gsANext_);
        glDisableVertexAttribArray(gsASide_);
        glDisableVertexAttribArray(gsAColor_);
        glDisableVertexAttribArray(gsAMode_);
        glEnable(GL_DEPTH_TEST); // @Jin 实验后恢复深度测试，供后续注记/marker 等通道使用
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // Pass 4：点要素（屏幕固定圆 billboard，GL_TRIANGLES）。与 fill/stroke 同处 depthMask=FALSE（彼此
    // 不写深度、画家序合成），GL_DEPTH_TEST 仍开→背面半球点被地球瓦片深度剔除；圆心深度取 vecAlt
    // （与 fill 同高且 fill 未写深度），不会被自家 fill 遮挡。屏幕半径 = pointRadiusDp × density（px）。
    if (globePointProgram_.program() != 0) {
        globePointProgram_.use();
        glUniformMatrix4fv(gpUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glUniform2f(gpUViewportPx_, halfVpX, halfVpY);
        const float density = static_cast<float>(nav_.displayDensity());
        const GLsizei pstride = kGlobePointFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gpAPos_);
        glEnableVertexAttribArray(gpACorner_);
        glEnableVertexAttribArray(gpAColor_);
        glEnableVertexAttribArray(gpAMode_);
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel)) continue;
            if (!vl.iconRgba.empty()) continue; // 有图标层交给图标 billboard，此处不画圆
            if (vl.globePointChunks.empty()) continue;
            // @Jin 实验（可回退）：贴地点圆叠层关闭对地形的深度测试，锚点圆不再被近景坡面剔除。
            if (vl.globeDraped) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST); // @Jin DEM 烘焙层避免被粗地形三角面深度遮挡。
            const float radiusPx = static_cast<float>(vl.style.pointRadiusDp) * density;
            if (radiusPx <= 0.0f) continue;
            glUniform2f(gpUHalfPx_, radiusPx, radiusPx);
            glUniform3f(gpUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(gpUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(gpUVecAlt_, vecAlt);
            // AABB 仅含圆心（corner 展开在屏幕空间），剔除余量覆盖屏幕半径量级
            for (const auto &ch : vl.globePointChunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(gpAPos_, 3, GL_FLOAT, GL_FALSE, pstride,
                                      reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gpACorner_, 2, GL_FLOAT, GL_FALSE, pstride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(gpAColor_, 4, GL_FLOAT, GL_FALSE, pstride,
                                      reinterpret_cast<const void *>(5 * sizeof(float)));
                glVertexAttribPointer(gpAMode_, 1, GL_FLOAT, GL_FALSE, pstride,
                                      reinterpret_cast<const void *>(9 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, ch.vertexCount);
            }
        }
        glDisableVertexAttribArray(gpAPos_);
        glDisableVertexAttribArray(gpACorner_);
        glDisableVertexAttribArray(gpAColor_);
        glDisableVertexAttribArray(gpAMode_);
        glEnable(GL_DEPTH_TEST); // @Jin 实验后恢复深度测试，供后续注记/marker 等通道使用
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // Pass 5：点要素图标（屏幕固定像素尺寸 billboard，GL_TRIANGLES）。同处 depthMask=FALSE（彼此画家序、
    // 不写深度），GL_DEPTH_TEST 仍开→背半球图标被地球瓦片剔除。逐层懒上传图标纹理 + 逐 chunk 绘制。
    // 像素半尺寸 = iconW×0.5（位图像素已含密度，与 2D drawVectorIcons 同口径，无额外 density）。
    if (globeIconProgram_.program() != 0) {
        globeIconProgram_.use();
        glUniformMatrix4fv(giUMvp_, 1, GL_FALSE, viewProjRtc.data());
        glUniform2f(giUViewportPx_, halfVpX, halfVpY);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(giUTexture_, 0);
        const GLsizei istride = kGlobeIconFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(giaPos_);
        glEnableVertexAttribArray(giACorner_);
        glEnableVertexAttribArray(giATexCoord_);
        glEnableVertexAttribArray(giAColor_);
        glEnableVertexAttribArray(giaMode_);
        for (auto &vlp : vectorLayers_) {
            VectorLayer &vl = *vlp;
            if (!vectorLayerShown(vl, camLevel)) continue;
            if (vl.iconRgba.empty() || vl.globeIconChunks.empty()) continue;
            // @Jin 实验（可回退）：图标叠层与点同口径处理（图标当前也贴地）。
            if (vl.globeDraped) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST); // @Jin DEM 烘焙层避免被粗地形三角面深度遮挡。
            // 懒上传/上下文重建后重传图标纹理（与 2D 同口径；iconRgba 构造后只读、无竞争）
            if (!vl.iconTex.isValid() &&
                !vl.iconTex.uploadRGBA(vl.iconRgba.data(), vl.iconW, vl.iconH))
                continue;
            const float halfWpx = static_cast<float>(vl.iconW) * 0.5f;
            const float halfHpx = static_cast<float>(vl.iconH) * 0.5f;
            if (halfWpx <= 0.0f || halfHpx <= 0.0f) continue;
            glUniform2f(giUHalfPx_, halfWpx, halfHpx);
            glUniform3f(giUOffset_, static_cast<float>(vl.globeAnchor.x - cam.eye.x),
                        static_cast<float>(vl.globeAnchor.y - cam.eye.y),
                        static_cast<float>(vl.globeAnchor.z - cam.eye.z));
            glUniform3f(giUAnchor_, static_cast<float>(vl.globeAnchor.x),
                        static_cast<float>(vl.globeAnchor.y), static_cast<float>(vl.globeAnchor.z));
            glUniform1f(giUVecAlt_, vecAlt);
            vl.iconTex.bind();
            for (const auto &ch : vl.globeIconChunks) {
                if (ch.vbo == 0 || !globeChunkVisible(ch, cam.frustumPlanes)) continue;
                glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                glVertexAttribPointer(giaPos_, 3, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void *>(0));
                glVertexAttribPointer(giACorner_, 2, GL_FLOAT, GL_FALSE, istride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glVertexAttribPointer(giATexCoord_, 2, GL_FLOAT, GL_FALSE, istride,
                                      reinterpret_cast<const void *>(5 * sizeof(float)));
                glVertexAttribPointer(giAColor_, 4, GL_FLOAT, GL_FALSE, istride,
                                      reinterpret_cast<const void *>(7 * sizeof(float)));
                glVertexAttribPointer(giaMode_, 1, GL_FLOAT, GL_FALSE, istride,
                                      reinterpret_cast<const void *>(11 * sizeof(float)));
                glDrawArrays(GL_TRIANGLES, 0, ch.vertexCount);
            }
        }
        glDisableVertexAttribArray(giaPos_);
        glDisableVertexAttribArray(giACorner_);
        glDisableVertexAttribArray(giATexCoord_);
        glDisableVertexAttribArray(giAColor_);
        glDisableVertexAttribArray(giaMode_);
        glEnable(GL_DEPTH_TEST); // @Jin 实验后恢复深度测试，供后续注记/marker 等通道使用
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE); // 恢复深度写入，交回瓦片/后续通路
    return needRedraw;
}

// 3D 绝对 ECEF→屏幕像素投影 helper（供标注通路；地理坐标入口由调用方自行 g2c 后传入）。
bool Renderer::projectEcef3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc,
                             const Vec3 &ecef, float &outSxPx, float &outSyPx) const {
    if (viewportWidth_ <= 0 || viewportHeight_ <= 0) return false;
    // RTC：绝对 ECEF 减眼点（double 差后转 float，同瓦片/矢量通路口径），再走 viewProjRtc 到 clip。
    const float x = static_cast<float>(ecef.x - cam.eye.x);
    const float y = static_cast<float>(ecef.y - cam.eye.y);
    const float z = static_cast<float>(ecef.z - cam.eye.z);
    const float *m = viewProjRtc.data(); // 列主序 m[col*4+row]；clip_row_i = Σ_j m[j*4+i]*v_j，v=(x,y,z,1)
    const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= 1e-4f) return false; // 相机背后/近平面外
    const float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    const float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    const float ndcx = cx / cw;
    const float ndcy = cy / cw;
    outSxPx = (ndcx * 0.5f + 0.5f) * static_cast<float>(viewportWidth_);
    outSyPx = (0.5f - ndcy * 0.5f) * static_cast<float>(viewportHeight_); // y 向下、原点左上
    return true;
}

void Renderer::drawLocationMarker3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc) {
    bool visible;
    double lon, lat, heading;
    {
        std::lock_guard<std::mutex> lock(markerMtx_);
        visible = markerVisible_;
        lon = markerLon_;
        lat = markerLat_;
        heading = markerHeading_;
    }
    if (!visible || viewportHeight_ <= 0) return;

    const double density = nav_.displayDensity();
    const Wgs84Globe &globe = Wgs84Globe::instance();

    // 地表点 ECEF（海拔 0）+ 椭球单位法向（局部「上」）。
    Vec3 ground{0.0, 0.0, 0.0};
    globe.geographicToCartesian(lon, lat, 0.0, ground);
    Vec3 up{0.0, 0.0, 0.0};
    globe.geographicNormal(lon, lat, up);
    up = up.normalized();

    // 背半球剔除：定位点随球面转到地球背面（法向与眼向夹角 > 90°）→ 整块不画，免穿透地球显示。
    if (up.dot(cam.eye - ground) <= 0.0) return;

    // 局部切平面正交基：east = 极轴 × up（+东），north = up × east（+北）。图标沿 east/north 铺成
    // 平行于地表的贴地矩形（指真北），随相机俯仰自然透视压扁（根治旧版正对屏幕的「立面」观感）。
    const Vec3 east = Vec3{0.0, 0.0, 1.0}.cross(up).normalized();
    const Vec3 north = up.cross(east).normalized();

    // 屏幕恒定尺寸换算（对齐 参考实现 pixelSizeAtDistance）：该处「米/像素」= 眼距·2·tan(fov/2)/视口高。
    const double dist = (ground - cam.eye).length();
    const double fovRad = nav_.fieldOfViewDeg() * 3.14159265358979323846 / 180.0;
    const double metersPerPx = dist * 2.0 * std::tan(fovRad * 0.5) / static_cast<double>(viewportHeight_);
    if (!(metersPerPx > 0.0)) return;

    // 引线：图标平面沿法向抬高固定屏幕长度悬于地面点上方 → 斜视显出连线、俯视沿视线坍缩为点不可见。
    const Vec3 center = ground + up * (kMarkerLeaderLenDp * density * metersPerPx);

    // 贴地顶点统一在 CPU 端沿切平面铺成后 double 减眼点转 float，走 viewProjRtc 透视投影。
    auto toRtc = [&](const Vec3 &p) { return p - cam.eye; };

    glDisable(GL_DEPTH_TEST); // 定位标识始终画在最上层（同旧版纯屏幕叠加），收尾复开
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float iconHalfWm = 0.0f, iconHalfHmm = 0.0f;
    bool hasIcon = false;

    // 1) 引线（地面点 → 图标中心，箭蓝色，画在最底）：单条 GL_LINES，俯视沿视线投影为点故不可见。
    {
        std::vector<float> line;
        line.reserve(2 * kGlobeColorFloatsPerVertex);
        pushGlobeColorVert(line, toRtc(ground), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(line, toRtc(center), kArrowR, kArrowG, kArrowB, 1.0f);
        globeColorProgram_.use();
        glUniformMatrix4fv(gc3UMvp_, 1, GL_FALSE, viewProjRtc.data());
        glBindBuffer(GL_ARRAY_BUFFER, vboGlobeMesh_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(line.size() * sizeof(float)),
                     line.data(), GL_DYNAMIC_DRAW);
        const GLsizei lstride = kGlobeColorFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gc3APos_);
        glVertexAttribPointer(gc3APos_, 3, GL_FLOAT, GL_FALSE, lstride, reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(gc3AColor_);
        glVertexAttribPointer(gc3AColor_, 4, GL_FLOAT, GL_FALSE, lstride,
                              reinterpret_cast<const void *>(3 * sizeof(float)));
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, 0, 2);
        glLineWidth(1.0f);
        glDisableVertexAttribArray(gc3APos_);
        glDisableVertexAttribArray(gc3AColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // 2) 图标贴地四边形（有罗盘图标）：图像顶行(v=0)映射 +north → 指真北、贴地正立，随俯仰透视压扁。
    if (!markerIconRgba_.empty() && markerIconW_ > 0 && markerIconH_ > 0) {
        if (!markerIconTex_.isValid()) {
            markerIconTex_.uploadRGBA(markerIconRgba_.data(), markerIconW_, markerIconH_);
        }
        iconHalfWm = static_cast<float>(markerIconW_ * kMarkerIconScale * density * 0.5 * metersPerPx);
        iconHalfHmm = static_cast<float>(markerIconH_ * kMarkerIconScale * density * 0.5 * metersPerPx);
        if (markerIconTex_.isValid() && iconHalfWm > 0.0f && iconHalfHmm > 0.0f) {
            hasIcon = true;
            static const float kBase[6][4] = {
                {-1.0f, -1.0f, 0.0f, 0.0f}, {1.0f, -1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
                {-1.0f, -1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 0.0f, 1.0f},
            };
            std::vector<float> quad;
            quad.reserve(6 * kGlobeFlatIconFloatsPerVertex);
            for (int i = 0; i < 6; ++i) {
                const float sx = kBase[i][0], sy = kBase[i][1];
                const Vec3 w = center + east * (static_cast<double>(sx) * iconHalfWm)
                                       + north * (static_cast<double>(-sy) * iconHalfHmm);
                pushGlobeFlatIconVert(quad, toRtc(w), kBase[i][2], kBase[i][3],
                                      1.0f, 1.0f, 1.0f, 1.0f);
            }
            globeFlatIconProgram_.use();
            glUniformMatrix4fv(gfiMvp_, 1, GL_FALSE, viewProjRtc.data());
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(gfiTexture_, 0);
            markerIconTex_.bind();
            glBindBuffer(GL_ARRAY_BUFFER, vboIcon_);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(quad.size() * sizeof(float)),
                         quad.data(), GL_DYNAMIC_DRAW);
            const GLsizei istride = kGlobeFlatIconFloatsPerVertex * sizeof(float);
            glEnableVertexAttribArray(gfiPos_);
            glVertexAttribPointer(gfiPos_, 3, GL_FLOAT, GL_FALSE, istride, reinterpret_cast<const void *>(0));
            glEnableVertexAttribArray(gfiTexCoord_);
            glVertexAttribPointer(gfiTexCoord_, 2, GL_FLOAT, GL_FALSE, istride,
                                  reinterpret_cast<const void *>(3 * sizeof(float)));
            glEnableVertexAttribArray(gfiColor_);
            glVertexAttribPointer(gfiColor_, 4, GL_FLOAT, GL_FALSE, istride,
                                  reinterpret_cast<const void *>(5 * sizeof(float)));
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glDisableVertexAttribArray(gfiPos_);
            glDisableVertexAttribArray(gfiTexCoord_);
            glDisableVertexAttribArray(gfiColor_);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    // 3) 无图标回退：贴地蓝点白边圆（切平面内三角扇，半径 = dp·density·metersPerPx）。
    if (!hasIcon) {
        const float ringRm = static_cast<float>(kMarkerRingRadiusDp * density * metersPerPx);
        const float dotRm = static_cast<float>(kMarkerDotRadiusDp * density * metersPerPx);
        if (ringRm > 0.0f) {
            static const std::vector<std::pair<float, float>> unitCircle = [] {
                std::vector<std::pair<float, float>> pts;
                pts.reserve(kMarkerSegments + 1);
                for (int s = 0; s <= kMarkerSegments; ++s) {
                    const float a = static_cast<float>(2.0 * 3.14159265358979 * s / kMarkerSegments);
                    pts.emplace_back(std::cos(a), std::sin(a));
                }
                return pts;
            }();
            globeColorProgram_.use();
            glUniformMatrix4fv(gc3UMvp_, 1, GL_FALSE, viewProjRtc.data());
            glBindBuffer(GL_ARRAY_BUFFER, vboGlobeMesh_);
            glEnableVertexAttribArray(gc3APos_);
            glEnableVertexAttribArray(gc3AColor_);
            const GLsizei cstride = kGlobeColorFloatsPerVertex * sizeof(float);
            auto drawDisc = [&](float radiusM, float r, float g, float b) {
                std::vector<float> verts;
                verts.reserve(static_cast<size_t>(kMarkerSegments + 2) * kGlobeColorFloatsPerVertex);
                pushGlobeColorVert(verts, toRtc(center), r, g, b, 1.0f); // 扇心
                for (int s = 0; s <= kMarkerSegments; ++s) {
                    const Vec3 w = center + east * (static_cast<double>(unitCircle[s].first) * radiusM)
                                           + north * (static_cast<double>(unitCircle[s].second) * radiusM);
                    pushGlobeColorVert(verts, toRtc(w), r, g, b, 1.0f);
                }
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                             verts.data(), GL_DYNAMIC_DRAW);
                glVertexAttribPointer(gc3APos_, 3, GL_FLOAT, GL_FALSE, cstride, reinterpret_cast<const void *>(0));
                glVertexAttribPointer(gc3AColor_, 4, GL_FLOAT, GL_FALSE, cstride,
                                      reinterpret_cast<const void *>(3 * sizeof(float)));
                glDrawArrays(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(verts.size() / kGlobeColorFloatsPerVertex));
            };
            drawDisc(ringRm, 1.0f, 1.0f, 1.0f);   // 白边
            drawDisc(dotRm, 0.20f, 0.47f, 0.96f); // 蓝心（≈ #3378F5）
            glDisableVertexAttribArray(gc3APos_);
            glDisableVertexAttribArray(gc3AColor_);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    // 4) 移动方向箭头（贴地，画在图标之上，heading>=0）：切平面内沿 heading（顺时针自北）方向的
    //    凹口箭头，几何同 2D drawLocationMarker（0.2.8 createHeadingArrowIcon 同款），以标示中心
    //    固定 dp 尺寸，不随罗盘图标放大。
    if (heading >= 0.0) {
        const double hr = heading * 3.14159265358979323846 / 180.0;
        const Vec3 dirH = north * std::cos(hr) + east * std::sin(hr);
        const Vec3 perpH = up.cross(dirH); // 切平面内垂直（单位）
        const double dpM = density * metersPerPx; // dp→px→米
        const double fwdM = kArrowTipDp * dpM;    // 箭尖（前）
        const double backM = kArrowBackDp * dpM;  // 底角（后）
        const double halfM = kArrowHalfDp * dpM;  // 底角横向半宽
        const double notchM = kArrowNotchDp * dpM; // 凹口（后）
        const Vec3 tipW = center + dirH * fwdM;
        const Vec3 brW = center - dirH * backM + perpH * halfM;
        const Vec3 blW = center - dirH * backM - perpH * halfM;
        const Vec3 nkW = center - dirH * notchM;
        std::vector<float> arrow;
        arrow.reserve(6 * kGlobeColorFloatsPerVertex);
        pushGlobeColorVert(arrow, toRtc(tipW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(brW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(nkW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(tipW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(nkW), kArrowR, kArrowG, kArrowB, 1.0f);
        pushGlobeColorVert(arrow, toRtc(blW), kArrowR, kArrowG, kArrowB, 1.0f);
        globeColorProgram_.use();
        glUniformMatrix4fv(gc3UMvp_, 1, GL_FALSE, viewProjRtc.data());
        glBindBuffer(GL_ARRAY_BUFFER, vboGlobeMesh_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(arrow.size() * sizeof(float)),
                     arrow.data(), GL_DYNAMIC_DRAW);
        const GLsizei astride = kGlobeColorFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(gc3APos_);
        glVertexAttribPointer(gc3APos_, 3, GL_FLOAT, GL_FALSE, astride, reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(gc3AColor_);
        glVertexAttribPointer(gc3AColor_, 4, GL_FLOAT, GL_FALSE, astride,
                              reinterpret_cast<const void *>(3 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(gc3APos_);
        glDisableVertexAttribArray(gc3AColor_);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST); // 交回默认态（onDrawFrame3D 收尾会再关，此处显式恢复避免泄漏禁用态）
}

void Renderer::drawVectorLabels3D(const Navigator::Camera3D &cam, const Matrix4 &viewProjRtc) {
    if (vectorLayers_.empty() || viewportWidth_ <= 0 || viewportHeight_ <= 0) return;
    if (!fontAtlas_.isLoaded() || textProgram_.program() == 0 || vboText_ == 0) return;

    const float density = static_cast<float>(nav_.displayDensity());
    const float bakePx = static_cast<float>(fontAtlas_.bakePx());
    if (bakePx <= 0.0f) return;
    const float vCenterPx = (fontAtlas_.ascentPx() + fontAtlas_.descentPx()) * 0.5f; // 视觉中心相对基线（上正）

    // 相机整数级别：级别不足隐藏的层其标注一并隐藏（与 2D / 矢量层同口径，免幽灵标注）。
    const int camLevel = nav_.displayLevel();

    // 屏幕像素空间正交投影（y 向下、原点左上），字形坐标直接用像素（与 marker 同范式）。
    const Matrix4 screenOrtho = Matrix4::ortho(0.0f, static_cast<float>(viewportWidth_),
                                               static_cast<float>(viewportHeight_), 0.0f);

    // poor-man's 描边：屏幕空间 8 向偏移（偏移量 ≈ 1dp 像素，无 worldPerPx）。
    std::vector<float> outlineVerts;
    std::vector<float> fillVerts;
    const float o = 1.0f * density; // 轮廓偏移（像素）
    static const float kDir[8][2] = {
        {1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f},
        {0.7071f, 0.7071f}, {-0.7071f, 0.7071f}, {0.7071f, -0.7071f}, {-0.7071f, -0.7071f},
    };

    for (const auto &vlp : vectorLayers_) {
        VectorLayer &vl = *vlp;
        if (!vectorLayerShown(vl, camLevel) || vl.geom.labels.empty()) continue;
        const double ox = vl.geom.originWx;
        const double oy = vl.geom.originWy;
        for (size_t idx = 0; idx < vl.geom.labels.size(); ++idx) {
            const LabelItem &li = vl.geom.labels[idx];
            // 屏幕像素/烘焙像素比例：使文字屏幕高 = 16dp×density×逐条字号（与 2D 同口径，不随缩放变）
            const float sPx = (16.0f * density * li.size) / bakePx;
            if (sPx <= 0.0f) continue;
            const VectorLayer::LabelLayout *lay = ensureLabelLayout(vl, idx);
            if (lay == nullptr) continue;
            // @Jin quick-summit：overlay 的动态标注在 3D 地形开启时按同点 DEM 采高，并额外上抬避免文字压进山体。
            double labelAltitude = 0.0;
            if (vl.isOverlay && !terrainCache_.empty()) {
                double labelLon = 0.0;
                double labelLat = 0.0;
                MercatorProjection::worldToLonLat(ox + static_cast<double>(li.x),
                                                  oy + static_cast<double>(li.y),
                                                  labelLon, labelLat);
                labelLat = MercatorProjection::clampLatitude(labelLat);
                labelAltitude = sampleTerrainHeightLonLat(labelLon, labelLat) + 120.0; // @Jin 文字略高于峰顶，POI 图标针尖仍精确落在地表。
            }
            // @Jin 锚点（相对图层原点世界坐标）→ ECEF；overlay 标注使用 DEM 高程，普通文件矢量仍贴球面以保持旧行为。
            const Vec3 p = worldXYToCartesian(ox + static_cast<double>(li.x),
                                              oy + static_cast<double>(li.y), labelAltitude);
            // CPU 地平线剔除：背面半球（表面法向背向相机）的字不画，免纯屏幕叠加穿地球。
            // 球近似：法向≈normalize(p)，可见当且仅当 dot(p, eye−p)>0（两侧长度均正，比符号即可）。
            const Vec3 toEye{cam.eye.x - p.x, cam.eye.y - p.y, cam.eye.z - p.z};
            if (p.x * toEye.x + p.y * toEye.y + p.z * toEye.z <= 0.0) continue;
            float axp, ayp;
            if (!projectEcef3D(cam, viewProjRtc, p, axp, ayp)) continue; // 相机背后/近平面外
            if (vl.isOverlay) {
                // @Jin quick-summit：文字显示在 POI 图标上方，避免长中文标题压住峰顶图标；约 14dp 偏移随屏幕密度缩放。
                ayp -= 14.0f * density;
            }
            if (li.outline) {
                for (const auto &d : kDir)
                    emitLabelQuads(*lay, axp + d[0] * o, ayp + d[1] * o, sPx, vCenterPx,
                                   li.outlineColor[0], li.outlineColor[1],
                                   li.outlineColor[2], li.outlineColor[3], outlineVerts);
            }
            emitLabelQuads(*lay, axp, ayp, sPx, vCenterPx,
                           li.color[0], li.color[1], li.color[2], li.color[3], fillVerts);
        }
    }

    if (outlineVerts.empty() && fillVerts.empty()) return;

    // 图集上传/重传：排版期可能烘焙了新字形（dirty），或上下文重建后 atlasTex_ 已失效
    if (!atlasTex_.isValid() || fontAtlas_.dirty()) {
        atlasTex_.uploadRGBA(fontAtlas_.pixels(), fontAtlas_.size(), fontAtlas_.size());
        fontAtlas_.clearDirty();
    }
    if (!atlasTex_.isValid()) return;

    // 纯屏幕叠加：关深度测试，画在最上层（与 marker 一致）；收尾复开避免泄漏禁用态。
    glDisable(GL_DEPTH_TEST);
    textProgram_.use();
    glUniformMatrix4fv(txUMvp_, 1, GL_FALSE, screenOrtho.data());
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(txUTexture_, 0);
    atlasTex_.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, vboText_);
    const GLsizei tstride = kTextFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(txAPos_);
    glEnableVertexAttribArray(txATexCoord_);
    glEnableVertexAttribArray(txAColor_);
    glVertexAttribPointer(txAPos_, 2, GL_FLOAT, GL_FALSE, tstride, reinterpret_cast<const void *>(0));
    glVertexAttribPointer(txATexCoord_, 2, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<const void *>(2 * sizeof(float)));
    glVertexAttribPointer(txAColor_, 4, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<const void *>(4 * sizeof(float)));
    auto drawBuf = [&](const std::vector<float> &buf) {
        if (buf.empty()) return;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(buf.size() * sizeof(float)),
                     buf.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(buf.size() / kTextFloatsPerVertex));
    };
    drawBuf(outlineVerts); // 轮廓在下
    drawBuf(fillVerts);    // 文字在上
    glDisableVertexAttribArray(txAPos_);
    glDisableVertexAttribArray(txATexCoord_);
    glDisableVertexAttribArray(txAColor_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::release() {
    clearTileTextures();
    releaseVectorGl();
    releaseRasterImageGl();
    clearTerrain();
    geoQuadtree_.clear(); // #11 清除四叉树帧间缓存
    // @Jin 释放渲染器时同时清掉 exp1f 的临时 LOD 状态，重新创建上下文后从首帧开始学习。
    lastIdleGlobeTileLevel_ = -1;
    lastIdleGlobeTileMaxLevel_ = -1;
    if (vboDynamic_ != 0) {
        glDeleteBuffers(1, &vboDynamic_);
        vboDynamic_ = 0;
    }
    if (vboUnitQuad_ != 0) {
        glDeleteBuffers(1, &vboUnitQuad_);
        vboUnitQuad_ = 0;
    }
    if (vboText_ != 0) {
        glDeleteBuffers(1, &vboText_);
        vboText_ = 0;
    }
    if (vboIcon_ != 0) {
        glDeleteBuffers(1, &vboIcon_);
        vboIcon_ = 0;
    }
    if (vboGlobeMesh_ != 0) {
        glDeleteBuffers(1, &vboGlobeMesh_);
        vboGlobeMesh_ = 0;
    }
    atlasTex_.release();
    markerIconTex_.release();
    colorProgram_.release();
    lineProgram_.release();
    texProgram_.release();
    textProgram_.release();
    iconProgram_.release();
    globeTexProgram_.release();
    globeColorProgram_.release();
    globeFillProgram_.release();
    globeStrokeProgram_.release();
    globePointProgram_.release();
    globeIconProgram_.release();
    globeFlatIconProgram_.release();
    // GPU 拾取通路资源
    pickColorProgram_.release();
    pickLineProgram_.release();
    if (pickFbo_ != 0) { glDeleteFramebuffers(1, &pickFbo_); pickFbo_ = 0; }
    if (pickTex_ != 0) { glDeleteTextures(1, &pickTex_); pickTex_ = 0; }
    pickW_ = 0; pickH_ = 0;
    // #9 RHI：释放后端实例（unique_ptr 析构，GLES2Backend 无额外 GL 源须显式销毁——
    // 其资源均由 create/destroy 配对管理，后端对象本身无 GL 句柄成员）
    rhi_.reset();
}

} // namespace globecore
