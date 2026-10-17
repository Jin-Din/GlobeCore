#include "rhi/VulkanBackend.h"

#include "util/Log.h"

namespace globecore {

// ============================================================================
// Vulkan 后端实现
//
// 策略：当未定义 GLOBECORE_VULKAN_BACKEND 时，本编译单元为一个空桩——
// createVulkanBackend() 返回 nullptr，工厂自动回退 GLES2Backend。
// 当定义了 GLOBECORE_VULKAN_BACKEND 且链接了 Vulkan SDK 时，逐步填入
// 真实的 Vulkan 初始化与渲染管线代码。
// ============================================================================

#ifndef GLOBECORE_VULKAN_BACKEND

// ---------------------------------------------------------------------------
// 空桩：无 Vulkan SDK 时的回退实现
// ---------------------------------------------------------------------------
std::unique_ptr<RHIBackend> createVulkanBackend() {
    LOGW("RHI: Vulkan backend not compiled in (GLOBECORE_VULKAN_BACKEND undefined), "
         "falling back to GLES2");
    return nullptr;
}

#else

// ---------------------------------------------------------------------------
// 真实 Vulkan 后端（待接入 SDK 后逐步实现）
//
// 计划实现路线：
//   Phase 1: VkInstance + VkPhysicalDevice + VkDevice + VkQueue 初始化
//   Phase 2: VkSwapchainKHR + 渲染目标/深度缓冲创建
//   Phase 3: VkRenderPass + VkPipeline（着色器 SPIR-V 编译）
//   Phase 4: VkBuffer + VkDeviceMemory（顶点/索引缓冲）
//   Phase 5: VkImage + VkImageView（纹理）
//   Phase 6: VkFramebuffer（离屏渲染）
//   Phase 7: 命令缓冲录制 + draw call
//   Phase 8: 帧同步（fence/semaphore）与 present
//
// 当前：仅框架结构，真实逻辑待 SDK 接入后补全
// ---------------------------------------------------------------------------

struct VulkanBackend::Impl {
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDev = VK_NULL_HANDLE;
    VkDevice        device    = VK_NULL_HANDLE;
    VkQueue         graphicsQueue = VK_NULL_HANDLE;
    int             maxTexSize = 4096;  // Vulkan 多数实现 >= 4096

    ~Impl() {
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

VulkanBackend::VulkanBackend() : impl_(std::make_unique<Impl>()) {}
VulkanBackend::~VulkanBackend() = default;

void VulkanBackend::initialize() {
    // TODO Phase 1: vkCreateInstance + vkEnumeratePhysicalDevices + vkCreateDevice
    // 当前：仅日志占位
    LOGI("RHI[Vulkan]: initialize() — skeleton, real impl pending SDK integration");
}

RHIShaderProgram VulkanBackend::createShaderProgram(const char* vertexSrc,
                                                    const char* fragmentSrc) {
    // TODO Phase 3: 编译 GLSL → SPIR-V (shaderc) → VkShaderModule
    LOGW("RHI[Vulkan]: createShaderProgram — not implemented yet");
    return RHIShaderProgram{};
}

void VulkanBackend::destroyShaderProgram(RHIShaderProgram& /*prog*/) {
    // TODO Phase 3: vkDestroyShaderModule
}

void VulkanBackend::useProgram(const RHIShaderProgram& /*prog*/) {
    // Vulkan 无「当前程序」概念——着色器在 VkPipeline 创建时绑定
    // 状态记录在此，drawElements/drawArrays 时绑定到命令缓冲
}

int VulkanBackend::attribLocation(const RHIShaderProgram& /*prog*/, const char* /*name*/) {
    // Vulkan 顶点属性位置在管线创建时显式指定（VkVertexInputBindingDescription）
    return -1;
}

int VulkanBackend::uniformLocation(const RHIShaderProgram& /*prog*/, const char* /*name*/) {
    // Vulkan uniform 通过 descriptor set layout + push constants 管理
    return -1;
}

RHIBuffer VulkanBackend::createBuffer() {
    // TODO Phase 4: vkCreateBuffer + vkAllocateMemory + vkBindBufferMemory
    LOGW("RHI[Vulkan]: createBuffer — not implemented yet");
    return RHIBuffer{};
}

void VulkanBackend::destroyBuffer(RHIBuffer& /*buf*/) {
    // TODO Phase 4: vkDestroyBuffer + vkFreeMemory
}

void VulkanBackend::uploadBufferData(RHIBuffer& /*buf*/, const void* /*data*/,
                                      size_t /*size*/, RHIBufferUsage /*usage*/) {
    // TODO Phase 4: vkMapMemory + memcpy + vkUnmapMemory（或 staging buffer）
}

void VulkanBackend::bindVertexBuffer(const RHIBuffer& /*buf*/) {
    // 记录到命令缓冲：vkCmdBindVertexBuffers
}

void VulkanBackend::bindIndexBuffer(const RHIBuffer& /*buf*/) {
    // 记录到命令缓冲：vkCmdBindIndexBuffer
}

RHITexture VulkanBackend::createTextureRGBA(const uint8_t* /*rgba*/, int /*width*/, int /*height*/) {
    // TODO Phase 5: vkCreateImage + vkAllocateMemory + staging upload
    LOGW("RHI[Vulkan]: createTextureRGBA — not implemented yet");
    return RHITexture{};
}

RHITexture VulkanBackend::createTextureCompressed(uint32_t /*internalFormat*/, int /*width*/,
                                                   int /*height*/, const void* /*data*/,
                                                   size_t /*dataSize*/) {
    // TODO Phase 5: VK_FORMAT_BC* / VK_FORMAT_ASTC* / VK_FORMAT_ETC2*
    LOGW("RHI[Vulkan]: createTextureCompressed — not implemented yet");
    return RHITexture{};
}

void VulkanBackend::destroyTexture(RHITexture& /*tex*/) {
    // TODO Phase 5: vkDestroyImageView + vkDestroyImage + vkFreeMemory
}

void VulkanBackend::bindTexture(const RHITexture& /*tex*/, int /*unit*/) {
    // 通过 descriptor set 更新纹理绑定
}

void VulkanBackend::setUniform(int /*loc*/, const float* /*mat4*/) {}
void VulkanBackend::setUniform(int /*loc*/, float, float, float, float) {}
void VulkanBackend::setUniform(int /*loc*/, float, float, float) {}
void VulkanBackend::setUniform(int /*loc*/, float, float) {}
void VulkanBackend::setUniform(int /*loc*/, float) {}
void VulkanBackend::setUniform(int /*loc*/, int) {}

void VulkanBackend::enableAttrib(int /*loc*/) {}
void VulkanBackend::disableAttrib(int /*loc*/) {}
void VulkanBackend::setAttribPointer(int /*loc*/, int /*size*/, int /*stride*/, size_t /*offset*/) {}

void VulkanBackend::setViewport(int /*x*/, int /*y*/, int /*width*/, int /*height*/) {
    // 记录到命令缓冲：VkViewport + vkCmdSetViewport
}
void VulkanBackend::setDepthTest(bool /*enable*/) {
    // 管线创建时配置深度状态
}
void VulkanBackend::setDepthMask(bool /*enable*/) {
    // vkCmdSetDepthWriteMask（需 dynamic state）
}
void VulkanBackend::setDepthFunc(RHIDepthFunc /*func*/) {
    // 管线创建时配置
}
void VulkanBackend::setBlend(bool /*enable*/) {
    // vkCmdSetBlendConstants / 管线创建时配置
}
void VulkanBackend::setBlendFunc(RHIBlendFactor /*src*/, RHIBlendFactor /*dst*/) {
    // 管线创建时配置 VkPipelineColorBlendAttachmentState
}
void VulkanBackend::setClearColor(float /*r*/, float /*g*/, float /*b*/, float /*a*/) {
    // 记录清除色，clear() 时 vkCmdClearAttachments / vkCmdClearColorImage
}
void VulkanBackend::clear(uint32_t /*mask*/) {
    // vkCmdClearAttachments（当前渲染目标内）
}

RHIFramebuffer VulkanBackend::createFramebuffer(int /*width*/, int /*height*/) {
    // TODO Phase 6: VkImage + VkImageView + VkFramebuffer
    LOGW("RHI[Vulkan]: createFramebuffer — not implemented yet");
    return RHIFramebuffer{};
}

void VulkanBackend::destroyFramebuffer(RHIFramebuffer& /*fbo*/) {
    // TODO Phase 6: vkDestroyFramebuffer + vkDestroyImageView + vkDestroyImage
}

void VulkanBackend::bindFramebuffer(const RHIFramebuffer& /*fbo*/) {
    // vkCmdBeginRenderPass / vkCmdEndRenderPass
}

void VulkanBackend::readPixels(int /*x*/, int /*y*/, int /*width*/, int /*height*/,
                                uint8_t* /*outRgba*/) {
    // Vulkan 读取帧缓冲需 vkCmdCopyImageToBuffer + staging + map
    LOGW("RHI[Vulkan]: readPixels — not implemented yet");
}

void VulkanBackend::drawArrays(RHIPrimitive /*prim*/, int /*first*/, int /*count*/) {
    // vkCmdDraw
}

void VulkanBackend::drawElements(RHIPrimitive /*prim*/, int /*count*/, RHIIndexType /*type*/,
                                  size_t /*offset*/) {
    // vkCmdDrawIndexed
}

int VulkanBackend::maxTextureSize() const {
    return impl_ ? impl_->maxTexSize : 4096;
}

std::unique_ptr<RHIBackend> createVulkanBackend() {
    return std::make_unique<VulkanBackend>();
}

#endif // GLOBECORE_VULKAN_BACKEND

} // namespace globecore
