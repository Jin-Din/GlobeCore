#ifndef GLOBECORE_RHI_VULKAN_BACKEND_H
#define GLOBECORE_RHI_VULKAN_BACKEND_H

#include "rhi/RHI.h"

// ============================================================================
// Vulkan 后端骨架（#9 RHI 抽象：Vulkan/Metal 路线）
//
// 本文件仅在定义了 GLOBECORE_VULKAN_BACKEND 编译宏时才包含 Vulkan 头并
// 实现真实后端逻辑；否则仅暴露一个 createVulkanBackend() 空桩供工厂编译通过。
//
// 当前 Windows 环境无 Vulkan SDK，本后端为编译守卫下的空实现：
//   - 所有 create* 返回 id==0（无效）
//   - 所有 set/draw/clear 为 no-op
//   - initialize() / maxTextureSize() 返回保守值
//
// 待接入 Vulkan SDK 后，取消下方 #ifdef 守卫内的真实实现，逐步填入
// VkInstance / VkDevice / VkQueue / VkSwapchain / VkPipeline 全套流程。
// ============================================================================

namespace globecore {

#ifdef GLOBECORE_VULKAN_BACKEND

// ---------------------------------------------------------------------------
// 真实 Vulkan 后端（需 Vulkan SDK）
// ---------------------------------------------------------------------------
#include <vulkan/vulkan.h>

class VulkanBackend : public RHIBackend {
public:
    VulkanBackend() = default;
    ~VulkanBackend() override;

    const char* name() const override { return "Vulkan"; }

    void initialize() override;

    RHIShaderProgram createShaderProgram(const char* vertexSrc,
                                          const char* fragmentSrc) override;
    void destroyShaderProgram(RHIShaderProgram& prog) override;
    void useProgram(const RHIShaderProgram& prog) override;
    int attribLocation(const RHIShaderProgram& prog, const char* name) override;
    int uniformLocation(const RHIShaderProgram& prog, const char* name) override;

    RHIBuffer createBuffer() override;
    void destroyBuffer(RHIBuffer& buf) override;
    void uploadBufferData(RHIBuffer& buf, const void* data, size_t size,
                          RHIBufferUsage usage) override;
    void bindVertexBuffer(const RHIBuffer& buf) override;
    void bindIndexBuffer(const RHIBuffer& buf) override;

    RHITexture createTextureRGBA(const uint8_t* rgba, int width, int height) override;
    RHITexture createTextureCompressed(uint32_t internalFormat, int width, int height,
                                        const void* data, size_t dataSize) override;
    void destroyTexture(RHITexture& tex) override;
    void bindTexture(const RHITexture& tex, int unit) override;

    void setUniform(int loc, const float* mat4) override;
    void setUniform(int loc, float x, float y, float z, float w) override;
    void setUniform(int loc, float x, float y, float z) override;
    void setUniform(int loc, float x, float y) override;
    void setUniform(int loc, float x) override;
    void setUniform(int loc, int x) override;

    void enableAttrib(int loc) override;
    void disableAttrib(int loc) override;
    void setAttribPointer(int loc, int size, int stride, size_t offset) override;

    void setViewport(int x, int y, int width, int height) override;
    void setDepthTest(bool enable) override;
    void setDepthMask(bool enable) override;
    void setDepthFunc(RHIDepthFunc func) override;
    void setBlend(bool enable) override;
    void setBlendFunc(RHIBlendFactor src, RHIBlendFactor dst) override;
    void setClearColor(float r, float g, float b, float a) override;
    void clear(uint32_t mask) override;

    RHIFramebuffer createFramebuffer(int width, int height) override;
    void destroyFramebuffer(RHIFramebuffer& fbo) override;
    void bindFramebuffer(const RHIFramebuffer& fbo) override;
    void readPixels(int x, int y, int width, int height, uint8_t* outRgba) override;

    void drawArrays(RHIPrimitive prim, int first, int count) override;
    void drawElements(RHIPrimitive prim, int count, RHIIndexType type, size_t offset) override;

    int maxTextureSize() const override;

private:
    struct Impl;  // Pimpl：持有 VkInstance/Device/Queue 等重量级成员
    std::unique_ptr<Impl> impl_;
};

#endif // GLOBECORE_VULKAN_BACKEND

/// 创建 Vulkan 后端实例。仅在 GLOBECORE_VULKAN_BACKEND 定义时返回真实后端，
/// 否则返回 nullptr（调用方应回退到 GLES2Backend）。
std::unique_ptr<RHIBackend> createVulkanBackend();

} // namespace globecore

#endif // GLOBECORE_RHI_VULKAN_BACKEND_H
