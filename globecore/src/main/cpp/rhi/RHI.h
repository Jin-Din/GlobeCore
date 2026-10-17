#ifndef GLOBECORE_RHI_RHI_H
#define GLOBECORE_RHI_RHI_H

#include <cstddef>
#include <cstdint>
#include <memory>

namespace globecore {

// ============================================================================
// RHI（Rendering Hardware Interface）—— 后端无关的渲染硬件抽象层
//
// 将散落在 Renderer 中的 GLES2 直接调用抽象为统一接口，使引擎可在
// GLES2 / Vulkan / Metal 等不同图形后端上运行，切换后端时无需改动
// 渲染逻辑代码。当前实现：
//   - GLES2Backend：封装现有 OpenGL ES 2.0 调用（生产后端）
//   - VulkanBackend：骨架实现（编译守卫 GLOBECORE_VULKAN_BACKEND，无 SDK 时空实现）
//
// 设计原则：
//   1. 资源句柄为轻量值类型（uint32_t id），后端自行映射到原生对象
//   2. 所有操作必须在渲染线程（GL 线程）调用，调用方保证线程安全
//   3. 现有 ShaderProgram / Texture 薄封装保留不动，RHI 在更高层抽象
//      状态管理与 draw call 流程，逐步替代散落的 GL 调用
// ============================================================================

// ---------------- 资源句柄类型（后端无关，0 表示无效） ----------------

/// 顶点/索引缓冲区句柄
struct RHIBuffer {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
};

/// 纹理句柄（含宽高元数据，供采样计算用）
struct RHITexture {
    uint32_t id = 0;
    int width = 0;
    int height = 0;
    bool valid() const { return id != 0 && width > 0 && height > 0; }
};

/// 着色器程序句柄
struct RHIShaderProgram {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
};

/// 帧缓冲区句柄（含尺寸元数据）
struct RHIFramebuffer {
    uint32_t id = 0;
    int width = 0;
    int height = 0;
    bool valid() const { return id != 0; }
};

// ---------------- 枚举类型 ----------------

/// 图元拓扑类型
enum class RHIPrimitive {
    Triangles,      ///< GL_TRIANGLES / VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    TriangleStrip,  ///< GL_TRIANGLE_STRIP / VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
    Lines,          ///< GL_LINES / VK_PRIMITIVE_TOPOLOGY_LINE_LIST
    LineStrip,      ///< GL_LINE_STRIP / VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
    Points,         ///< GL_POINTS / VK_PRIMITIVE_TOPOLOGY_POINT_LIST
};

/// 索引数据类型
enum class RHIIndexType {
    U16,  ///< 16 位无符号短整
    U32,  ///< 32 位无符号整
};

/// 深度比较函数
enum class RHIDepthFunc {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

/// 混合因子
enum class RHIBlendFactor {
    Zero,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstColor,
    OneMinusDstColor,
};

/// 清除缓冲区位掩码
enum : uint32_t {
    RHI_CLEAR_COLOR   = 0x1,  ///< glClear(GL_COLOR_BUFFER_BIT)
    RHI_CLEAR_DEPTH   = 0x2,  ///< glClear(GL_DEPTH_BUFFER_BIT)
    RHI_CLEAR_STENCIL = 0x4,  ///< glClear(GL_STENCIL_BUFFER_BIT)
};

/// 缓冲区用途（提示后端选择内存类型）
enum class RHIBufferUsage {
    StaticDraw,   ///< 一次性上传、多次绘制（GL_STATIC_DRAW）
    DynamicDraw,  ///< 频繁更新（GL_DYNAMIC_DRAW）
    StreamDraw,   ///< 一次性上传、少量绘制（GL_STREAM_DRAW）
};

// ============================================================================
// 抽象后端接口
// ============================================================================
class RHIBackend {
public:
    virtual ~RHIBackend() = default;

    /// 后端名称（如 "GLES2"、"Vulkan"），供日志/调试
    virtual const char* name() const = 0;

    // -------------------- 生命周期 --------------------

    /// 初始化后端（查询能力上限等）。须在上下文建立后、任何资源创建前调用。
    virtual void initialize() = 0;

    // -------------------- 着色器程序 --------------------

    /// 编译顶点+片元着色器并链接为程序。失败返回 id==0。
    virtual RHIShaderProgram createShaderProgram(const char* vertexSrc,
                                                  const char* fragmentSrc) = 0;

    /// 销毁着色器程序（须在渲染线程调用）
    virtual void destroyShaderProgram(RHIShaderProgram& prog) = 0;

    /// 激活程序为当前渲染管线
    virtual void useProgram(const RHIShaderProgram& prog) = 0;

    /// 查询属性位置（-1 表示不存在/未链接）
    virtual int attribLocation(const RHIShaderProgram& prog, const char* name) = 0;

    /// 查询 uniform 位置（-1 表示不存在/被编译器优化掉）
    virtual int uniformLocation(const RHIShaderProgram& prog, const char* name) = 0;

    // -------------------- 缓冲区 --------------------

    /// 创建顶点/索引缓冲区
    virtual RHIBuffer createBuffer() = 0;

    /// 销毁缓冲区
    virtual void destroyBuffer(RHIBuffer& buf) = 0;

    /// 上传缓冲区数据（data=nullptr 时仅分配/预清零）
    /// @param buf      目标缓冲区
    /// @param data     数据指针（可为 nullptr）
    /// @param size     字节数
    /// @param usage    用途提示
    virtual void uploadBufferData(RHIBuffer& buf, const void* data, size_t size,
                                  RHIBufferUsage usage = RHIBufferUsage::DynamicDraw) = 0;

    /// 绑定缓冲区为当前 ARRAY_BUFFER（顶点属性数据源）
    virtual void bindVertexBuffer(const RHIBuffer& buf) = 0;

    /// 绑定缓冲区为当前 ELEMENT_ARRAY_BUFFER（索引数据源）
    virtual void bindIndexBuffer(const RHIBuffer& buf) = 0;

    // -------------------- 纹理 --------------------

    /// 上传 RGBA8888 像素为纹理（线性过滤 + CLAMP_TO_EDGE）
    virtual RHITexture createTextureRGBA(const uint8_t* rgba, int width, int height) = 0;

    /// 上传 GPU 压缩纹理数据（ETC2/ASTC 等，直传压缩字节，硬件解压省显存）
    /// @param internalFormat  后端相关内部格式标识（GLES2 为 GL 枚举，Vulkan 后端自行映射）
    /// @param data            压缩字节流
    /// @param dataSize        字节长度
    virtual RHITexture createTextureCompressed(uint32_t internalFormat, int width, int height,
                                               const void* data, size_t dataSize) = 0;

    /// 销毁纹理
    virtual void destroyTexture(RHITexture& tex) = 0;

    /// 绑定纹理到指定纹理单元（unit 从 0 开始）
    virtual void bindTexture(const RHITexture& tex, int unit) = 0;

    // -------------------- Uniform 设置 --------------------

    virtual void setUniform(int loc, const float* mat4) = 0;                ///< mat4
    virtual void setUniform(int loc, float x, float y, float z, float w) = 0; ///< vec4
    virtual void setUniform(int loc, float x, float y, float z) = 0;       ///< vec3
    virtual void setUniform(int loc, float x, float y) = 0;               ///< vec2
    virtual void setUniform(int loc, float x) = 0;                        ///< float
    virtual void setUniform(int loc, int x) = 0;                          ///< int / sampler

    // -------------------- 顶点属性 --------------------

    /// 启用顶点属性数组
    virtual void enableAttrib(int loc) = 0;

    /// 禁用顶点属性数组
    virtual void disableAttrib(int loc) = 0;

    /// 设置顶点属性指针（须先 bindVertexBuffer）
    /// @param loc      属性位置
    /// @param size     每顶点分量数（1..4）
    /// @param stride   顶点间步幅（字节）
    /// @param offset   首元素偏移（字节）
    virtual void setAttribPointer(int loc, int size, int stride, size_t offset) = 0;

    // -------------------- 渲染状态 --------------------

    /// 设置视口
    virtual void setViewport(int x, int y, int width, int height) = 0;

    /// 深度测试开关
    virtual void setDepthTest(bool enable) = 0;

    /// 深度写入开关（glDepthMask）
    virtual void setDepthMask(bool enable) = 0;

    /// 深度比较函数
    virtual void setDepthFunc(RHIDepthFunc func) = 0;

    /// 混合开关
    virtual void setBlend(bool enable) = 0;

    /// 混合因子设置
    virtual void setBlendFunc(RHIBlendFactor src, RHIBlendFactor dst) = 0;

    /// 设置清屏颜色
    virtual void setClearColor(float r, float g, float b, float a) = 0;

    /// 清除缓冲区（mask 为 RHI_CLEAR_* 位或）
    virtual void clear(uint32_t mask) = 0;

    // -------------------- 帧缓冲区 --------------------

    /// 创建带颜色附件的帧缓冲区（用于离屏渲染/拾取）
    virtual RHIFramebuffer createFramebuffer(int width, int height) = 0;

    /// 销毁帧缓冲区
    virtual void destroyFramebuffer(RHIFramebuffer& fbo) = 0;

    /// 绑定帧缓冲区为渲染目标（fbo.id==0 表示绑定默认帧缓冲区）
    virtual void bindFramebuffer(const RHIFramebuffer& fbo) = 0;

    /// 读取帧缓冲区像素（用于 GPU 拾取）
    /// @param fbo     目标帧缓冲区（须已绑定）
    /// @param x,y     读取起点（左下原点）
    /// @param w,h     读取宽高
    /// @param outRgba 输出缓冲区（至少 w*h*4 字节）
    virtual void readPixels(int x, int y, int width, int height, uint8_t* outRgba) = 0;

    // -------------------- 绘制 --------------------

    /// 非索引绘制
    virtual void drawArrays(RHIPrimitive prim, int first, int count) = 0;

    /// 索引绘制（须先 bindIndexBuffer）
    virtual void drawElements(RHIPrimitive prim, int count, RHIIndexType type,
                              size_t offset) = 0;

    // -------------------- 查询 --------------------

    /// 最大纹理尺寸（上下文建立后查询，未建立时保守报 2048）
    virtual int maxTextureSize() const = 0;
};

// ============================================================================
// 后端工厂（编译时 / 运行时选择）
// ============================================================================

/// 创建 RHI 后端实例。默认创建 GLES2 后端；定义了 GLOBECORE_VULKAN_BACKEND
/// 且链接了 Vulkan SDK 时创建 Vulkan 后端。返回的实例须在渲染线程使用与销毁。
std::unique_ptr<RHIBackend> createRHIBackend();

} // namespace globecore

#endif // GLOBECORE_RHI_RHI_H
