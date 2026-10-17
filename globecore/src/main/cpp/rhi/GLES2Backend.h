#ifndef GLOBECORE_RHI_GLES2_BACKEND_H
#define GLOBECORE_RHI_GLES2_BACKEND_H

#include "rhi/RHI.h"

#include <GLES2/gl2.h>

namespace globecore {

// ============================================================================
// GLES2 后端：封装现有 OpenGL ES 2.0 调用为 RHI 接口实现
//
// 直接映射 RHI 抽象操作到 GLES2 原生调用，无额外抽象层开销。
// 资源句柄 id 直接使用 GL 对象名（GLuint），1:1 映射。
// 当前为现有 Renderer 中 GL 调用的薄封装，后续可逐步替代散落的直接 GL 调用。
// ============================================================================
class GLES2Backend : public RHIBackend {
public:
    GLES2Backend() = default;
    ~GLES2Backend() override = default;

    const char* name() const override { return "GLES2"; }

    void initialize() override;

    // 着色器程序
    RHIShaderProgram createShaderProgram(const char* vertexSrc,
                                          const char* fragmentSrc) override;
    void destroyShaderProgram(RHIShaderProgram& prog) override;
    void useProgram(const RHIShaderProgram& prog) override;
    int attribLocation(const RHIShaderProgram& prog, const char* name) override;
    int uniformLocation(const RHIShaderProgram& prog, const char* name) override;

    // 缓冲区
    RHIBuffer createBuffer() override;
    void destroyBuffer(RHIBuffer& buf) override;
    void uploadBufferData(RHIBuffer& buf, const void* data, size_t size,
                          RHIBufferUsage usage) override;
    void bindVertexBuffer(const RHIBuffer& buf) override;
    void bindIndexBuffer(const RHIBuffer& buf) override;

    // 纹理
    RHITexture createTextureRGBA(const uint8_t* rgba, int width, int height) override;
    RHITexture createTextureCompressed(uint32_t internalFormat, int width, int height,
                                        const void* data, size_t dataSize) override;
    void destroyTexture(RHITexture& tex) override;
    void bindTexture(const RHITexture& tex, int unit) override;

    // Uniform
    void setUniform(int loc, const float* mat4) override;
    void setUniform(int loc, float x, float y, float z, float w) override;
    void setUniform(int loc, float x, float y, float z) override;
    void setUniform(int loc, float x, float y) override;
    void setUniform(int loc, float x) override;
    void setUniform(int loc, int x) override;

    // 顶点属性
    void enableAttrib(int loc) override;
    void disableAttrib(int loc) override;
    void setAttribPointer(int loc, int size, int stride, size_t offset) override;

    // 渲染状态
    void setViewport(int x, int y, int width, int height) override;
    void setDepthTest(bool enable) override;
    void setDepthMask(bool enable) override;
    void setDepthFunc(RHIDepthFunc func) override;
    void setBlend(bool enable) override;
    void setBlendFunc(RHIBlendFactor src, RHIBlendFactor dst) override;
    void setClearColor(float r, float g, float b, float a) override;
    void clear(uint32_t mask) override;

    // 帧缓冲区
    RHIFramebuffer createFramebuffer(int width, int height) override;
    void destroyFramebuffer(RHIFramebuffer& fbo) override;
    void bindFramebuffer(const RHIFramebuffer& fbo) override;
    void readPixels(int x, int y, int width, int height, uint8_t* outRgba) override;

    // 绘制
    void drawArrays(RHIPrimitive prim, int first, int count) override;
    void drawElements(RHIPrimitive prim, int count, RHIIndexType type, size_t offset) override;

    // 查询
    int maxTextureSize() const override;

private:
    /// 缓存查询结果（initialize 时填充）
    int maxTextureSize_ = 2048;

    /// RHI 枚举 → GL 常量映射
    static GLenum toGLPrimitive(RHIPrimitive prim);
    static GLenum toGLIndexType(RHIIndexType type);
    static GLenum toGLDepthFunc(RHIDepthFunc func);
    static GLenum toGLBlendFactor(RHIBlendFactor f);
    static GLenum toGLBufferUsage(RHIBufferUsage usage);
    static GLenum toGLClearMask(uint32_t mask);
};

} // namespace globecore

#endif // GLOBECORE_RHI_GLES2_BACKEND_H
