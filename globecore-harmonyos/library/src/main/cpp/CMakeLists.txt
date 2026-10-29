# globecore-harmonyos/library —— HarmonyOS NEXT native 构建脚本（对应 Android globecore cpp/CMakeLists.txt）
#
# 架构口径（1.1.0 双平台改造）：一套引擎源码，两端产物。
# 引擎源（core/globe/navigator/layer/net/vector/raster/render/geom/util/bridge）不复制不搬迁，
# 经 GLOBECORE_ENGINE_ROOT 直接引用 ../globecore/src/main/cpp；本目录只保留 ohos/ 鸿蒙专属源
# （EGL 上下文 / XComponent 桥 / NAPI 导出）。平台差异仅三处：Log.h 分派、GL 宿主、JNI↔NAPI 桥接。
# 第三方 .a 由 `build-scripts/build-all.sh --target ohos` 产出到 build-scripts/out/ohos/<abi>/，
# 三方头与引擎共用 globecore cpp/include/（install_headers 同一落位，与 .a 版本配套）。
cmake_minimum_required(VERSION 3.22.1)

project("globecore")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 符号默认隐藏（与 Android 侧同口径）：libcurl.a 内部静态打包 OpenSSL/zlib，
# 默认导出会与 libgdal 依赖链发生全局符号抢占。NAPI 注册经 constructor 属性显式导出，不受影响。
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

# 引擎源根：从本模块 cpp 目录向上派生 5 级（cpp → main → src → library → globecore-harmonyos → 仓库根），
# 与 Android 模块相对位置固定，不依赖构建时 cwd。可用 -DGLOBECORE_ENGINE_ROOT 覆盖。
if(NOT DEFINED GLOBECORE_ENGINE_ROOT)
    get_filename_component(GLOBECORE_ENGINE_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../globecore/src/main/cpp" ABSOLUTE)
endif()
if(NOT EXISTS "${GLOBECORE_ENGINE_ROOT}/core/GlobeEngine.h")
    message(FATAL_ERROR "GLOBECORE_ENGINE_ROOT 未指向引擎源：${GLOBECORE_ENGINE_ROOT}（检查 globecore 模块是否在同仓库）")
endif()
message(STATUS "globecore-harmonyos: GLOBECORE_ENGINE_ROOT=${GLOBECORE_ENGINE_ROOT}")

# 引擎内 #include "core/GlobeEngine.h" 等以引擎根为基准（与 Android 构建完全一致）
include_directories(${GLOBECORE_ENGINE_ROOT})
# 第三方头（curl/gdal/proj/sqlite/zlib 等），双平台共用同一份
include_directories(${GLOBECORE_ENGINE_ROOT}/include)
# @Jin OHOS 专属 NAPI/桩实现头文件目录（LITE GeoJSON reader 与 napi_* 同层引用）。
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# 目标 ABI 变量名差异：Android NDK 用 ANDROID_ABI，OHOS toolchain 用 OHOS_ARCH
set(GLOBECORE_ABI ${OHOS_ARCH})

# ==================== 第三方静态库（与 Android 侧同名同序，目录换 ohos 产物）====================
# 目录名须与 OHOS_ARCH 一致：arm64-v8a / x86_64
if(NOT DEFINED GLOBECORE_THIRD_PARTY_DIR)
    get_filename_component(GLOBECORE_THIRD_PARTY_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../build-scripts/out/ohos" ABSOLUTE)
endif()
set(OHOS_LIBS_DIR ${GLOBECORE_THIRD_PARTY_DIR}/${GLOBECORE_ABI})
# 布局兼容：上游 build-scripts 把 .a 平铺到 <abi>/，而本地 build-ohos-deps-win.sh 采用
# install 惯例落在 <abi>/lib/。两个布局都支持，省去手工搬文件。
if(NOT EXISTS ${OHOS_LIBS_DIR}/libcurl.a AND EXISTS ${OHOS_LIBS_DIR}/lib/libcurl.a)
    set(OHOS_LIBS_DIR ${OHOS_LIBS_DIR}/lib)
endif()
message(STATUS "globecore-harmonyos: OHOS_LIBS_DIR=${OHOS_LIBS_DIR}")

# 注意：curl/sqlite/minizip/expat/z 在 LITE 与非 LITE 下都需要；
# gdal/proj/kml×6/uriparser 仅非 LITE 需要（LITE 下产物目录里可能根本没有，故一并跳过定义）
add_library(curl STATIC IMPORTED)
set_target_properties(curl PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libcurl.a)

# curl 的 TLS 后端 mbedTLS。两点必须注意：
# ① 链接顺序：静态库按命令行出现次序解析未定义符号，libcurl.a 引用 mbedtls_*，
#    所以这三个库必须排在 curl 之后（见下方 GC_3RD_LIBS），放前面会 undefined symbol。
#    三者内部依赖为 mbedtls → mbedx509 → mbedcrypto，故按此序排列。
# ② 条件引入：curl 若以 CURL_USE_SSL=OFF 编译（无 TLS 后端），这三个 .a 根本不会产出，
#    此时直接 add_library 会让 CMake 配置失败。故按文件存在性决定，两种 curl 都能编过。
set(GC_MBEDTLS_LIBS "")
foreach(_mb mbedtls mbedx509 mbedcrypto)
    if(EXISTS ${OHOS_LIBS_DIR}/lib${_mb}.a)
        add_library(${_mb} STATIC IMPORTED)
        set_target_properties(${_mb} PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/lib${_mb}.a)
        list(APPEND GC_MBEDTLS_LIBS ${_mb})
    endif()
endforeach()
if(GC_MBEDTLS_LIBS)
    message(STATUS "globecore-harmonyos: curl TLS backend = mbedTLS (${GC_MBEDTLS_LIBS})")
else()
    message(WARNING "globecore-harmonyos: 未找到 mbedTLS —— curl 大概率是无 SSL 编译的，"
            "所有 https:// 请求会以 CURLE_UNSUPPORTED_PROTOCOL 失败（底图将取不到）")
endif()

add_library(sqlite STATIC IMPORTED)
set_target_properties(sqlite PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libsqlite3.a)
add_library(minizip STATIC IMPORTED)
set_target_properties(minizip PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libminizip.a)
add_library(expatstatic STATIC IMPORTED)
set_target_properties(expatstatic PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libexpat.a)
add_library(zstatic STATIC IMPORTED)
set_target_properties(zstatic PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libz.a)

if(NOT GLOBECORE_LITE)
add_library(proj STATIC IMPORTED)
set_target_properties(proj PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libproj.a)
add_library(gdal STATIC IMPORTED)
set_target_properties(gdal PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libgdal.a)
add_library(kmlbase STATIC IMPORTED)
set_target_properties(kmlbase PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlbase.a)
add_library(kmldom STATIC IMPORTED)
set_target_properties(kmldom PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmldom.a)
add_library(kmlengine STATIC IMPORTED)
set_target_properties(kmlengine PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlengine.a)
add_library(kmlxsd STATIC IMPORTED)
set_target_properties(kmlxsd PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlxsd.a)
add_library(kmlconvenience STATIC IMPORTED)
set_target_properties(kmlconvenience PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlconvenience.a)
add_library(kmlregionator STATIC IMPORTED)
set_target_properties(kmlregionator PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/libkmlregionator.a)
add_library(uriparser STATIC IMPORTED)
set_target_properties(uriparser PROPERTIES IMPORTED_LOCATION ${OHOS_LIBS_DIR}/liburiparser.a)
endif() # NOT GLOBECORE_LITE

# ==================== LITE 构建开关（feature/terrain-pre）====================
# ON = 裁掉 GDAL / PROJ / libkml 依赖链，只保留「在线瓦片 + 三维地形」所需能力。
#
# 为什么需要：GDAL 3.7 + PROJ 9 交叉编译成本高（依赖链长、耗时长），而地形验证与在线底图
# 完全不经过 GDAL。引擎的分层恰好支持这种裁剪——依赖 GDAL/PROJ 的只有下面 8 个 .cpp，
# 头文件（RasterRenderer.h / VectorReader.h / GdalBootstrap.h / bridge_api.h）全是纯 std 声明，
# 不含任何 gdal/proj 头。因此 LITE 下不编这 8 个文件、不链对应 .a，改由
# ohos/lite_gdal_stubs.cpp 提供同名符号的「不可用」实现，调用方零改动。
#
# 代价（LITE 下不可用）：本地矢量文件（shp/geojson 文件/kml/dwg）直读、本地 GeoTIFF/img
# 栅格直读、PROJ 坐标系变换、GDAL 版本/驱动查询。在线瓦片、MVT 矢量瓦片、三维地形均不受影响。
#
# 开启方式：hvigor library/build-profile.json5 → externalNativeOptions.arguments 追加
#           "-DGLOBECORE_LITE=ON"，或命令行自行 cmake 时传入。
option(GLOBECORE_LITE "裁剪 GDAL/PROJ/KML 依赖链（仅在线瓦片+三维地形）" OFF)

if(GLOBECORE_LITE)
    message(STATUS "globecore-harmonyos: GLOBECORE_LITE=ON —— 禁用 GDAL/PROJ/KML，改用 lite_gdal_stubs + 原生 GeoJSON reader")
    # @Jin LITE 保留无 GDAL 依赖的 GeoJSON 能力，使 HarmonyOS NEXT SDK 可直接加载在线 GeoJSON URL。
    set(GC_GDAL_SOURCES
            ohos/lite_gdal_stubs.cpp
            ohos/lite_geojson_reader.cpp)
else()
    set(GC_GDAL_SOURCES
            # GDAL 引导 + OGR 矢量读取 + KML 样式
            ${GLOBECORE_ENGINE_ROOT}/vector/GdalBootstrap.cpp
            ${GLOBECORE_ENGINE_ROOT}/vector/VectorReader.cpp
            ${GLOBECORE_ENGINE_ROOT}/vector/KmlStyle.cpp
            # 栅格渲染器（GDAL 重投影即时生成瓦片 PNG）
            ${GLOBECORE_ENGINE_ROOT}/raster/RasterRenderer.cpp
            # 数据 IO 桥核心（PROJ/GDAL）
            ${GLOBECORE_ENGINE_ROOT}/bridge/srs.cpp
            ${GLOBECORE_ENGINE_ROOT}/bridge/gdal_info.cpp
            ${GLOBECORE_ENGINE_ROOT}/bridge/layer_info.cpp
            ${GLOBECORE_ENGINE_ROOT}/bridge/vector_io.cpp)
endif()

add_library(${CMAKE_PROJECT_NAME} SHARED
        # ── 鸿蒙专属源（承担 Android 上 GLSurfaceView + JNI_OnLoad 的职责）──
        # EGL display/surface/context 封装 + MapRenderHost 渲染线程
        ohos/EglContext.cpp
        # OH_NativeXComponent surface 回调 ↔ 渲染线程接线
        ohos/XComponentBridge.cpp
        # NAPI 模块注册（对应 JNI_OnLoad）
        ohos/napi_init.cpp
        # GlobeEngine 全导出 NAPI 版（对应 jni/GlobeEngineJni.cpp，GL 生命周期改由 attach/detach 接管）
        ohos/napi_globe_engine.cpp
        # bridge 的 NAPI 版（核心在引擎 gcbridge，此处仅编解码薄封装）
        ohos/napi_srs.cpp
        ohos/napi_gdal_info.cpp
        ohos/napi_layer_info.cpp
        ohos/napi_vector_io.cpp
        # @Jin Cesium quantized-mesh 地形核心 NAPI（layer.json + 网格数学 + available 判断）
        ohos/napi_terrain_core.cpp
        # ── 引擎源：与 Android CMakeLists 同一清单，仅去掉 jni/ 桥接 ──
        # 顶层渲染引擎 + 相机/导航器
        ${GLOBECORE_ENGINE_ROOT}/core/GlobeEngine.cpp
        ${GLOBECORE_ENGINE_ROOT}/navigator/Navigator.cpp
        # 投影与瓦片网格数学 + 3D 球体地基
        ${GLOBECORE_ENGINE_ROOT}/globe/MercatorProjection.cpp
        ${GLOBECORE_ENGINE_ROOT}/globe/Wgs84Globe.cpp
        ${GLOBECORE_ENGINE_ROOT}/globe/Tessellator.cpp
        # 带帧间一致性的地理四叉树（相机微动复用上帧叶集 + 极冠外推覆盖 85°~90°）
        ${GLOBECORE_ENGINE_ROOT}/globe/GeographicQuadtree.cpp
        # RHI 抽象层：后端无关渲染接口 + GLES2/Vulkan 双后端（Vulkan 经 GLOBECORE_VULKAN_BACKEND 守卫）
        ${GLOBECORE_ENGINE_ROOT}/rhi/RHIFactory.cpp
        ${GLOBECORE_ENGINE_ROOT}/rhi/GLES2Backend.cpp
        ${GLOBECORE_ENGINE_ROOT}/rhi/VulkanBackend.cpp
        # quantized-mesh 地形 DEM 解码器（量化顶点+三角索引→高度采样）
        ${GLOBECORE_ENGINE_ROOT}/terrain/QuantizedMesh.cpp
        # @Jin Cesium 地形 layer.json 解析与 4326/TMS ↔ 墨卡托 bbox 求交核心
        ${GLOBECORE_ENGINE_ROOT}/terrain/CesiumTerrain.cpp
        # @Jin C++ 视口地形调度、下载缓存、去重重试与注入 Provider
        ${GLOBECORE_ENGINE_ROOT}/terrain/CesiumTerrainProvider.cpp
        # binary glTF (.glb) v2 解码器
        ${GLOBECORE_ENGINE_ROOT}/gltf/GltfDecoder.cpp
        # 3D Tiles tileset.json 解析器
        ${GLOBECORE_ENGINE_ROOT}/layer/TilesetJson.cpp
        # 瓦片缓存/加载器 + 矢量加载器
        ${GLOBECORE_ENGINE_ROOT}/layer/TileCache.cpp
        ${GLOBECORE_ENGINE_ROOT}/layer/TileLoader.cpp
        ${GLOBECORE_ENGINE_ROOT}/layer/VectorLoader.cpp
        # 全幅栅格加载器（tif 整幅直读为一张 RGBA 位图，后台线程交付 GL 上传）
        ${GLOBECORE_ENGINE_ROOT}/layer/RasterImageLoader.cpp
        # 网络取瓦片（libcurl 封装）
        ${GLOBECORE_ENGINE_ROOT}/net/HttpClient.cpp
        # @Jin P0 在线 GeoJSON Source 的 URL 解析与下载缓存；LITE 下由 ohos/lite_geojson_reader.cpp 解析缓存文件。
        ${GLOBECORE_ENGINE_ROOT}/vector/VectorSourceResolver.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/RemoteVectorCache.cpp
        # 几何构建（GdalBootstrap/VectorReader/KmlStyle 见 GC_GDAL_SOURCES，LITE 下由桩替代）
        ${GLOBECORE_ENGINE_ROOT}/vector/VectorBuilder.cpp
        # MVT/PBF 矢量瓦片解码器（手写 protobuf wire-format）
        ${GLOBECORE_ENGINE_ROOT}/vector/MvtDecoder.cpp
        # Mapbox Style JSON 解析+映射
        ${GLOBECORE_ENGINE_ROOT}/vector/MapboxStyle.cpp
        # 轻量 JSON 解析器（无外部依赖，递归下降）
        ${GLOBECORE_ENGINE_ROOT}/util/JsonParser.cpp
        # OpenGL ES 渲染器、着色器、纹理、图片解码、字形图集
        ${GLOBECORE_ENGINE_ROOT}/render/Renderer.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/ShaderProgram.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/Texture.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/ImageDecoder.cpp
        # KTX2/ETC2/ASTC 纹理压缩解码
        ${GLOBECORE_ENGINE_ROOT}/render/Ktx2Decoder.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/FontAtlas.cpp
        # GDAL/PROJ 依赖源（LITE 下替换为 lite_gdal_stubs.cpp 桩）
        ${GC_GDAL_SOURCES})

# XComponent 桩生成器感知：本目标直接产出全量引擎库，无需 "世界窗口" 桩 .so
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE GLOBECORE_LIBRARY_FULL)

find_library(ace-napi-lib libace_napi.z.so)
find_library(ace-ndk-lib libace_ndk.z.so)
find_library(native-window-lib libnative_window.so)
find_library(native-buffer-lib libnative_buffer.so)
find_library(hilog-lib libhilog_ndk.z.so)
find_library(egl-lib libEGL.so)
find_library(gles-lib libGLESv3.so)

# GLESv3（向下兼容 ES 2.0 绘制指令，render/ 层零改动）+ EGL：鸿蒙 GL 宿主；
# ace_napi：NAPI 运行时；ace_ndk + native_window/buffer：XComponent 与 OHNativeWindow 格式设置；
# hilog_ndk：Log.h 鸿蒙分支。GDAL 依赖链顺序参照 Android：gdal 在其依赖之前。
# curl.a 内置 zlib 与 libz.a 并存的重复符号：静态归档择一解析，hidden 可见性已隔离对外符号；
# OHOS 链接器对多重定义默认报错，追加 --allow-multiple-definition 兜底（与 Android lld 行为一致）。
if(GLOBECORE_LITE)
    # LITE：在线瓦片 + 三维地形所需最小集合（curl 负责取瓦片与 DEM，sqlite 供 MBTiles）
    # ${GC_MBEDTLS_LIBS} 紧跟 curl：curl 的 TLS 后端，顺序不可调换（详见上方定义处注释）
    set(GC_3RD_LIBS curl ${GC_MBEDTLS_LIBS} sqlite minizip expatstatic zstatic)
else()
    # 完整：GDAL 依赖链顺序参照 Android——gdal 在其依赖之前
    set(GC_3RD_LIBS
            curl
            ${GC_MBEDTLS_LIBS}
            gdal
            proj
            sqlite
            kmlengine
            kmldom
            kmlbase
            kmlxsd
            kmlconvenience
            kmlregionator
            uriparser
            minizip
            expatstatic
            zstatic)
endif()

target_link_libraries(${CMAKE_PROJECT_NAME}
        ${egl-lib}
        ${gles-lib}
        ${ace-napi-lib}
        ${ace-ndk-lib}
        ${native-window-lib}
        ${native-buffer-lib}
        ${hilog-lib}
        ${GC_3RD_LIBS}
        m
        pthread)

target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,--allow-multiple-definition)

# libglobecore 产物名：entry/HAR 依赖 "libglobecore": "file:../library"，
# ArkTS import 'libglobecore' 解析到 libglobecore.so（nm_modname=globecore 与之对应）
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME globecore)
