#include "rhi/RHIFactory.h"

#include "rhi/GLES2Backend.h"
#include "rhi/VulkanBackend.h"
#include "util/Log.h"

namespace globecore {

// ============================================================================
// RHI 后端工厂实现
//
// 编译时选择后端：
// - GLOBECORE_VULKAN_BACKEND 已定义 → 优先 Vulkan，失败回退 GLES2
// - 否则 → 直接 GLES2
// ============================================================================

std::unique_ptr<RHIBackend> createGLES2Backend() {
    return std::make_unique<GLES2Backend>();
}

std::unique_ptr<RHIBackend> createRHIBackend() {
#ifdef GLOBECORE_VULKAN_BACKEND
    auto vk = createVulkanBackend();
    if (vk) {
        LOGI("RHI: selected Vulkan backend");
        return vk;
    }
    LOGW("RHI: Vulkan backend unavailable, falling back to GLES2");
#endif
    LOGI("RHI: selected GLES2 backend");
    return createGLES2Backend();
}

} // namespace globecore
