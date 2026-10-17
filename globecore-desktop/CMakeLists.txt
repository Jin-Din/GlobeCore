# globecore-desktop —— Windows / Linux（含麒麟·UOS + 龙芯/飞腾）桌面宿主构建脚本
#
# 口径完全对齐 globecore-harmonyos/library：一套引擎源码，多端产物。
# 引擎源（core/globe/navigator/layer/net/vector/raster/render/geom/util/bridge）不复制不搬迁，
# 经 GLOBECORE_ENGINE_ROOT 直接引用 ../globecore/src/main/cpp；本目录只有 Qt 宿主（GL 窗口 + Widgets UI）。
# 平台差异仍仅三处：Log.h 分派（GLOBECORE_DESKTOP 分支）、GL 宿主（QOpenGLWidget 替 GLSurfaceView/XComponent）、
# 桥接绑定（桌面直接调 gcbridge，无需 JNI/NAPI 薄封装，故 bridge 的 JNI 导出段在 GLOBECORE_DESKTOP 下编译为空）。
cmake_minimum_required(VERSION 3.22.1)

project(globecore-desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)   # MapWidget/MainWindow 带 Q_OBJECT，需 moc

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ==================== 引擎源根 ====================
if(NOT DEFINED GLOBECORE_ENGINE_ROOT)
    get_filename_component(GLOBECORE_ENGINE_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/../globecore/src/main/cpp" ABSOLUTE)
endif()
if(NOT EXISTS "${GLOBECORE_ENGINE_ROOT}/core/GlobeEngine.h")
    message(FATAL_ERROR "GLOBECORE_ENGINE_ROOT 未指向引擎源：${GLOBECORE_ENGINE_ROOT}")
endif()
message(STATUS "globecore-desktop: GLOBECORE_ENGINE_ROOT=${GLOBECORE_ENGINE_ROOT}")

# ==================== GL 后端 ====================
# gles2 : 真 OpenGL ES 2.0。Windows 走 ANGLE（vcpkg install angle），Linux 走 Mesa libGLESv2。
#         引擎内嵌 GLSL ES 1.00 原生可编译 —— Windows 必须用这一条。
# glew  : 桌面 OpenGL 2.1 + GLEW，需驱动提供 GL_ARB_ES2_compatibility（Mesa 支持，Windows 原生不支持）。
#         仅为 Linux 上不便装 GLES 开发包时的退路。
set(GLOBECORE_DESKTOP_GL "gles2" CACHE STRING "桌面 GL 后端：gles2 | glew")
set_property(CACHE GLOBECORE_DESKTOP_GL PROPERTY STRINGS gles2 glew)

# ==================== 第三方依赖定位 ====================
# 【重要前提】引擎以「子目录形式」包含三方头：<curl/curl.h>、"gdal/gdal_priv.h"、"proj/proj.h"，
# 这些头来自 globecore/src/main/cpp/include/（版本与移动端 .a 严格配套，GDAL 头含大量 inline/C++ ABI）。
# 因此桌面端必须链「同版本」的 GDAL/PROJ/curl 等二进制，否则会头/库 ABI 错配。
#
# 模式 A（推荐，结构上最稳）：把 build-scripts 交叉编译出的静态库放此目录，与移动端同源同版本。
#   现状：build-scripts/build-all.sh 目前仅支持 --target android|ohos（Linux 交叉编译脚本），
#   尚不产 windows/linux 桌面产物 —— 需先扩展它支持 --target windows|linux，输出到
#   build-scripts/out/{windows|linux}/<arch>/lib*.a。这是本骨架真正的「前置工序」，非本 CMake 能凭空变出。
#   约定：GLOBECORE_THIRD_PARTY_DIR 指向 out/<平台>，下面再按 <arch> 分目录。
# 模式 B（便利但需自担版本/布局匹配）：vcpkg(Windows)/系统包(Linux, 含麒麟·UOS) find_package。
#   仅当三方包提供 gdal/、proj/ 子目录头布局且版本与引擎头一致时可用，否则回退到模式 A。
if(NOT DEFINED GLOBECORE_ARCH)
    if(MSVC)
        # MSVC 产物名用 .lib，架构目录名与 vcpkg 三元组手工对齐
        set(GLOBECORE_ARCH "x64")
    else()
        set(GLOBECORE_ARCH ${CMAKE_SYSTEM_PROCESSOR})
    endif()
endif()

if(NOT DEFINED GLOBECORE_THIRD_PARTY_DIR)
    if(WIN32)
        set(_gc_platform_dir windows)
    else()
        set(_gc_platform_dir linux)
    endif()
    get_filename_component(GLOBECORE_THIRD_PARTY_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/../build-scripts/out/${_gc_platform_dir}" ABSOLUTE)
endif()
set(GLOBECORE_DEPS_DIR ${GLOBECORE_THIRD_PARTY_DIR}/${GLOBECORE_ARCH})
message(STATUS "globecore-desktop: 第三方库目录=${GLOBECORE_DEPS_DIR}")

# 模式 A：build-scripts 静态产物齐全 → 直接链接（推荐，与移动端同源同版本）
set(_gc_static_ok TRUE)
foreach(_l libgdal.a libproj.a libsqlite3.a libcurl.a)
    if(NOT EXISTS "${GLOBECORE_DEPS_DIR}/${_l}")
        set(_gc_static_ok FALSE)
    endif()
endforeach()

add_library(globecore_deps INTERFACE)

if(_gc_static_ok)
    message(STATUS "globecore-desktop: 依赖模式 = build-scripts 静态库（与 Android/鸿蒙同版本）")
    # GDAL 依赖链顺序敏感：gdal 必须排在 proj/sqlite/kml*/uriparser/minizip/expat/z 之前
    set(_gc_link_order gdal proj sqlite3 kmlengine kmldom kmlbase kmlxsd
            kmlconvenience kmlregionator uriparser minizip expat z curl)
    foreach(_n ${_gc_link_order})
        set(_f "${GLOBECORE_DEPS_DIR}/lib${_n}.a")
        if(NOT EXISTS "${_f}")
            # 可选库缺失（如未启用 KML 子系统）：跳过而非报错，保持链接行完整
            message(STATUS "  跳过缺失的可选库：lib${_n}.a")
            continue()
        endif()
        add_library(gcdep_${_n} STATIC IMPORTED)
        set_target_properties(gcdep_${_n} PROPERTIES IMPORTED_LOCATION "${_f}")
        target_link_libraries(globecore_deps INTERFACE gcdep_${_n})
    endforeach()
    if(MSVC)
        # 静态 GDAL/curl 需要的系统库（与 build-scripts 的 Windows 产物配套）
        target_link_libraries(globecore_deps INTERFACE ws2_32 crypt32 wldap32 winmm normaliz)
    else()
        target_link_libraries(globecore_deps INTERFACE m pthread)
    endif()
else()
    # 模式 B：退到系统/ vcpkg 包。注意引擎以 "gdal/gdal_priv.h" 子目录形式包含 GDAL 头，
    # 故 GDAL_INCLUDE_DIR 须是其父目录（OSGeo4W 布局符合；vcpkg 平铺布局需自行加一层 gdal/ 转发）。
    message(STATUS "globecore-desktop: 依赖模式 = find_package（未在 ${GLOBECORE_DEPS_DIR} 找到静态库）")
    find_package(GDAL REQUIRED)
    find_package(CURL REQUIRED)
    find_package(SQLite3 REQUIRED)
    find_package(Threads REQUIRED)
    # 引擎直接调用 PROJ（bridge/srs.cpp）与 zlib（raster/RasterRenderer.cpp 的 PNG 编码 compress/crc32）。
    # 它们本就是 GDAL 的依赖，但 MSVC 下 gdal.lib 是 DLL 导入库、不携带其依赖，Linux 靠 .so 的 DT_NEEDED
    # 自动解析——故 Windows 必须显式补链 proj 与 zlib 两个导入库。vcpkg 的 share/proj、share/zlib 提供
    # 标准 CMake 配置包，导出 PROJ::proj、ZLIB::ZLIB。
    find_package(PROJ REQUIRED)
    find_package(ZLIB REQUIRED)
    # SQLite 导入目标名按来源不同：vcpkg 的 config 包导出 SQLite3::SQLite3（Windows 实测用此），
    # 而 CMake 自带 FindSQLite3 模块导出 SQLite::SQLite3（Linux 系统包走这条）。二者取存在的那个，
    # 保持两端链接行一致且各自平台不受影响。
    if(TARGET SQLite3::SQLite3)
        set(GC_SQLITE_LIB SQLite3::SQLite3)
    elseif(TARGET SQLite::SQLite3)
        set(GC_SQLITE_LIB SQLite::SQLite3)
    else()
        message(FATAL_ERROR "未找到 SQLite 导入目标（SQLite3::SQLite3 / SQLite::SQLite3 均无）")
    endif()
    target_link_libraries(globecore_deps INTERFACE
            GDAL::GDAL PROJ::proj CURL::libcurl ${GC_SQLITE_LIB} ZLIB::ZLIB Threads::Threads)
endif()

# ==================== Qt ====================
# 桌面 gles2 后端要求真正的 GLES 2.0 上下文：Windows 侧仅 Qt5 提供 AA_UseOpenGLES→ANGLE 通路
# （Qt6 明确移除 Windows ANGLE 支持，QOpenGLWidget 拿不到 ES 上下文）。因此本骨架 Windows 桌面版
# 首选 Qt5；Linux 端 Qt5 走 Mesa、Qt6 亦可（Mesa 提供原生 EGL/GLESv2）。
# 模块布局差异：Qt6 把 QOpenGLWidget 拆到 Qt6::OpenGLWidgets；Qt5 内嵌于 Qt5::OpenGL。
find_package(Qt NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets OpenGL)
if(Qt_VERSION_MAJOR GREATER_EQUAL 6)
    find_package(Qt${Qt_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
    set(GC_QT_LIBS
            Qt${Qt_VERSION_MAJOR}::Core
            Qt${Qt_VERSION_MAJOR}::Gui
            Qt${Qt_VERSION_MAJOR}::Widgets
            Qt${Qt_VERSION_MAJOR}::OpenGL
            Qt${Qt_VERSION_MAJOR}::OpenGLWidgets)
else()
    find_package(Qt${Qt_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets OpenGL)
    set(GC_QT_LIBS
            Qt${Qt_VERSION_MAJOR}::Core
            Qt${Qt_VERSION_MAJOR}::Gui
            Qt${Qt_VERSION_MAJOR}::Widgets
            Qt${Qt_VERSION_MAJOR}::OpenGL)
endif()
message(STATUS "globecore-desktop: Qt${Qt_VERSION_MAJOR}=${Qt_VERSION}")

# ==================== GL 后端实现 ====================
add_library(globecore_gl INTERFACE)
if(GLOBECORE_DESKTOP_GL STREQUAL "glew")
    find_package(GLEW REQUIRED)
    target_link_libraries(globecore_gl INTERFACE GLEW::GLEW)
    # 本目录置于最前，令引擎的 <GLES2/gl2.h> 命中 glshim 兼容头
    target_include_directories(globecore_gl INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/glshim)
    target_compile_definitions(globecore_gl INTERFACE GLOBECORE_DESKTOP_GLEW)
    message(STATUS "globecore-desktop: GL 后端 = 桌面 GL 2.1 + GLEW（需 ARB_ES2_compatibility）")
else()
    if(WIN32)
        # ANGLE：GLOBECORE_ANGLE_DIR 指向含 include/EGL、include/GLES2、lib/*.lib 的目录。
        # 实测可用来源：Qt5(5.15.2 msvc2019_64) 自带 libEGL/libGLESv2 的 .lib+.dll（Qt6 起不再捆绑），
        # 头文件用 vcpkg egl-registry/opengl-registry 装的那份（纯头、不碰 googlesource）。
        # vcpkg install angle 在本机网络下会被 chromium.googlesource.com 阻断，勿依赖。
        if(NOT DEFINED GLOBECORE_ANGLE_DIR)
            message(FATAL_ERROR
                    "Windows 的 gles2 后端需 ANGLE。请设 -DGLOBECORE_ANGLE_DIR=<ANGLE 根>（见上注释），"
                    "或改用 -DGLOBECORE_DESKTOP_GL=glew 仅调试非 Windows。")
        endif()
        find_path(GC_GLES2_INC GLES2/gl2.h HINTS ${GLOBECORE_ANGLE_DIR}/include REQUIRED)
        find_library(GC_GLESV2 NAMES libGLESv2 GLESv2 HINTS ${GLOBECORE_ANGLE_DIR}/lib REQUIRED)
        find_library(GC_EGL NAMES libEGL EGL HINTS ${GLOBECORE_ANGLE_DIR}/lib REQUIRED)
        target_include_directories(globecore_gl INTERFACE ${GC_GLES2_INC})
        target_link_libraries(globecore_gl INTERFACE ${GC_GLESV2} ${GC_EGL})
        message(STATUS "globecore-desktop: GL 后端 = ANGLE (GLES2)")
    else()
        # Linux/国产OS：Mesa 提供 libGLESv2 + libEGL，GLES2 头随 libgles-dev / 厂商 SDK
        find_path(GC_GLES2_INC GLES2/gl2.h
                PATHS /usr/include /usr/local/include ENV GLOBECORE GLES2_INC)
        find_library(GC_GLESV2 NAMES GLESv2 libGLESv2)
        find_library(GC_EGL NAMES EGL libEGL)
        if(NOT GC_GLES2_INC OR NOT GC_GLESV2 OR NOT GC_EGL)
            message(FATAL_ERROR
                    "未找到 GLESv2/EGL 开发包。Debian/Ubuntu: apt install libgles2-mesa-dev libegl1-mesa-dev；"
                    "麒麟/UOS 同理，或指定 -DGLOBECORE_GLES2_INC=<include> -DGLOBECORE_GLESV2=<lib>。")
        endif()
        target_include_directories(globecore_gl INTERFACE ${GC_GLES2_INC})
        target_link_libraries(globecore_gl INTERFACE ${GC_GLESV2} ${GC_EGL})
        message(STATUS "globecore-desktop: GL 后端 = 系统 GLESv2 + EGL (Mesa)")
    endif()
endif()

# ==================== 引擎源（与鸿蒙清单同一集合，仅去 jni/）====================
add_library(globecore STATIC
        ${GLOBECORE_ENGINE_ROOT}/core/GlobeEngine.cpp
        ${GLOBECORE_ENGINE_ROOT}/navigator/Navigator.cpp
        ${GLOBECORE_ENGINE_ROOT}/globe/MercatorProjection.cpp
        ${GLOBECORE_ENGINE_ROOT}/globe/Wgs84Globe.cpp
        ${GLOBECORE_ENGINE_ROOT}/globe/Tessellator.cpp
        ${GLOBECORE_ENGINE_ROOT}/globe/GeographicQuadtree.cpp
        # #9 RHI 抽象层：后端无关渲染接口 + GLES2/Vulkan 双后端
        ${GLOBECORE_ENGINE_ROOT}/rhi/RHIFactory.cpp
        ${GLOBECORE_ENGINE_ROOT}/rhi/GLES2Backend.cpp
        ${GLOBECORE_ENGINE_ROOT}/rhi/VulkanBackend.cpp
        ${GLOBECORE_ENGINE_ROOT}/terrain/QuantizedMesh.cpp
        ${GLOBECORE_ENGINE_ROOT}/gltf/GltfDecoder.cpp
        ${GLOBECORE_ENGINE_ROOT}/layer/TilesetJson.cpp
        ${GLOBECORE_ENGINE_ROOT}/layer/TileCache.cpp
        ${GLOBECORE_ENGINE_ROOT}/layer/TileLoader.cpp
        ${GLOBECORE_ENGINE_ROOT}/layer/VectorLoader.cpp
        ${GLOBECORE_ENGINE_ROOT}/layer/RasterImageLoader.cpp
        ${GLOBECORE_ENGINE_ROOT}/net/HttpClient.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/GdalBootstrap.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/VectorReader.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/KmlStyle.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/VectorBuilder.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/MvtDecoder.cpp
        ${GLOBECORE_ENGINE_ROOT}/vector/MapboxStyle.cpp
        ${GLOBECORE_ENGINE_ROOT}/util/JsonParser.cpp
        ${GLOBECORE_ENGINE_ROOT}/raster/RasterRenderer.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/Renderer.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/ShaderProgram.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/Texture.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/ImageDecoder.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/Ktx2Decoder.cpp
        ${GLOBECORE_ENGINE_ROOT}/render/FontAtlas.cpp
        # bridge：核心 gcbridge 参与编译，JNI 导出段在 GLOBECORE_DESKTOP 下编译为空
        ${GLOBECORE_ENGINE_ROOT}/bridge/srs.cpp
        ${GLOBECORE_ENGINE_ROOT}/bridge/gdal_info.cpp
        ${GLOBECORE_ENGINE_ROOT}/bridge/layer_info.cpp
        ${GLOBECORE_ENGINE_ROOT}/bridge/vector_io.cpp)

# 引擎以引擎根为基准包含自身头（"core/GlobeEngine.h"），三方头以 include/ 为基准（<curl/curl.h>、"gdal/xxx.h"）
target_include_directories(globecore PUBLIC
        ${GLOBECORE_ENGINE_ROOT}
        ${GLOBECORE_ENGINE_ROOT}/include)
target_compile_definitions(globecore PUBLIC GLOBECORE_DESKTOP)
target_link_libraries(globecore PUBLIC globecore_deps globecore_gl)

# Linux/国产OS：引擎自带 include/ 里的 GDAL/PROJ/curl 头是移动端 Android 配置版（GDAL 3.7.0/PROJ 9.0/curl ...），
# 桌面走系统包（GDAL 3.8.4/PROJ 9.4/curl 8.5）——GDAL/PROJ C++ 类 vtable 与 struct 布局跨 minor 会漂，
# 用错版头链新版库会 segfault（首次 Linux 冒烟即在 readVectorFile 崩）。MSVC 侧用 junction 解决同类问题，
# 这里 UNIX 侧用「影子 include 目录」等价方案：把系统 GDAL/PROJ/curl 目录以 gdal/、proj/、curl/ 子目录名
# 软链到 build 目录下（不污染源树），置于 ENGINE_ROOT/include 之前，令 "gdal/x.h"、"proj/proj.h"、
# "curl/curl.h" 命中新版；其余（stb/openssl/zlib/kml 等）仍回落到 bundle（本进程链系统 libcurl，其内
# 部 <openssl/*> 走 /usr/include 默认路径即可，无需干预）。
if(UNIX)
    find_path(GC_SYS_GDAL_LEAF gdal_priv.h PATH_SUFFIXES gdal REQUIRED)
    find_path(GC_SYS_PROJ_LEAF  proj.h      PATH_SUFFIXES proj REQUIRED)
    find_path(GC_SYS_CURL_LEAF  curl.h      PATH_SUFFIXES curl x86_64-linux-gnu/curl REQUIRED)
    set(GC_LINUXINC "${CMAKE_BINARY_DIR}/linuxinc")
    file(MAKE_DIRECTORY "${GC_LINUXINC}")
    foreach(_p gdal proj curl)
        set(_link "${GC_LINUXINC}/${_p}")
        if(EXISTS "${_link}" OR IS_SYMLINK "${_link}")
            file(REMOVE "${_link}")
        endif()
    endforeach()
    file(CREATE_LINK "${GC_SYS_GDAL_LEAF}" "${GC_LINUXINC}/gdal" SYMBOLIC)
    file(CREATE_LINK "${GC_SYS_PROJ_LEAF}" "${GC_LINUXINC}/proj" SYMBOLIC)
    file(CREATE_LINK "${GC_SYS_CURL_LEAF}" "${GC_LINUXINC}/curl" SYMBOLIC)
    target_include_directories(globecore BEFORE PUBLIC "${GC_LINUXINC}")
    message(STATUS "globecore-desktop: Linux 三方头影子 = ${GC_LINUXINC} "
                   "(gdal→${GC_SYS_GDAL_LEAF}, proj→${GC_SYS_PROJ_LEAF}, curl→${GC_SYS_CURL_LEAF})")
endif()

# Windows(MSVC) POSIX 兼容层：引擎源码按 Android/鸿蒙 POSIX 写，含 mkdir/access/
# <unistd.h>/<dirent.h>/clock_gettime 等桌面缺口。用 compat 目录影子这些头并 /FI 强制
# win_compat.h 注入补全宏。仅 MSVC 生效；移动端编译根本不含本目录，零影响。
if(MSVC)
    # 兼容层仅作用于引擎自身 .cpp（globecore），PRIVATE 不外泄到宿主：
    # win_compat.h 里 `#define mkdir(p,m)` 若经 PUBLIC 强制包含进宿主 TU，会污染 Qt 的
    # QDir::mkdir 声明（函数式宏按名字替换、无视参数个数）→ qdir.h 报重定义。
    target_include_directories(globecore BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
    target_compile_options(globecore PRIVATE /FIwin_compat.h)

    # GDAL/PROJ/curl 头必须用「Windows 配置版」（vcpkg 装的），不能用引擎自带的移动端 bundle 头：
    # 后者 cpl_config.h 里 VSI_STAT64=stat64 等是 Linux/Android 专配，与 Windows gdal.lib ABI 不符。
    # 这里用一个 junction 目录把 vcpkg 平铺头转发成引擎所需的 gdal/、proj/、curl/、sqlite/ 子目录布局，
    # 并置于自带 include 之前，使 "gdal/gdal_priv.h" 命中 Windows 版；stb/openssl/zlib 等仍回落 bundle。
    set(GLOBECORE_3RDP_INC "${CMAKE_CURRENT_SOURCE_DIR}/thirdp" CACHE PATH "Windows 版三方头子目录转发根（含 gdal/ proj/ curl/ sqlite/）")
    if(EXISTS "${GLOBECORE_3RDP_INC}/gdal/gdal_priv.h")
        target_include_directories(globecore BEFORE PUBLIC ${GLOBECORE_3RDP_INC})
        message(STATUS "globecore-desktop: 使用 Windows 配置版 GDAL/PROJ/curl 头 = ${GLOBECORE_3RDP_INC}")
    else()
        message(WARNING "globecore-desktop: 未找到 ${GLOBECORE_3RDP_INC}/gdal/gdal_priv.h，将回退引擎自带 GDAL 头（可能与 Windows gdal.lib ABI 不符）")
    endif()
endif()

# MSVC 下引擎源码含 UTF-8 中文注释/日志字面量，须显式声明源字符集，否则 C4819/乱码
if(MSVC)
    target_compile_options(globecore PUBLIC /utf-8)
endif()

# ==================== 宿主 + 应用 ====================
add_executable(globecore-desktop WIN32 MACOSX_BUNDLE
        src/host/MapWidget.cpp
        src/app/LayerPanel.cpp
        src/app/MainWindow.cpp
        src/app/main.cpp)
target_include_directories(globecore-desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(globecore-desktop PRIVATE
        globecore
        ${GC_QT_LIBS})

# 宿主 TU 经 Qt 拉入 <windows.h>，其 wingdi.h 的 `#define ABSOLUTE 1` 会破坏引擎
# geom/Camera.h 的 `enum class AltitudeMode { ABSOLUTE = 0 }`（详见 compat/win_host_compat.h）。
# 仅对宿主强制包含该头：先含 windows.h 置卫哨再 #undef 冲突宏，之后 Qt 的二次包含成空操作。
# PRIVATE：只影响本宿主（含 AUTOMOC 生成的 mocs_compilation.cpp），不泄回 globecore 库。
if(MSVC)
    target_include_directories(globecore-desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
    target_compile_options(globecore-desktop PRIVATE /FIwin_host_compat.h)
endif()

# ==================== 运行时资源：proj.db ====================
# PROJ 须能检索 proj.db；引擎经 gcbridge::initProjDataPath(宿主传入目录) 设置搜索路径。
# Windows: 引擎自带 Android assets/proj 里的 proj.db 与 vcpkg PROJ 版本兼容，拷到产物旁，随包发布。
# Linux:   系统 libproj-dev 已装 /usr/share/proj/proj.db（与本进程链的 libproj 严格同版本）；
#          再带 Android 那份（版本可能低若干 minor 号）会触发 proj_create "DATABASE.LAYOUT.VERSION.MINOR
#          不匹配" 报错并让 GDAL 坐标变换段错误。故 Linux 跳过拷贝，走 libproj 内置搜索路径。
# GLOBECORE_ENGINE_ROOT = globecore/src/main/cpp，故 proj.db 在 ../assets/proj（cpp→main→main/assets）。
set(GC_PROJ_SRC "${GLOBECORE_ENGINE_ROOT}/../assets/proj")
if(WIN32 AND EXISTS "${GC_PROJ_SRC}/proj.db")
    add_custom_command(TARGET globecore-desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${GC_PROJ_SRC}"
                    "$<TARGET_FILE_DIR:globecore-desktop>/proj"
            COMMENT "复制 proj.db 数据目录到产物旁")
endif()
