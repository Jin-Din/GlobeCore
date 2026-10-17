#ifndef GLOBECORE_RHI_RHI_FACTORY_H
#define GLOBECORE_RHI_RHI_FACTORY_H

#include <memory>

#include "rhi/RHI.h"

namespace globecore {

/// 创建 RHI 后端实例的工厂。
///
/// 选择逻辑（编译时决定）：
/// - 未定义 GLOBECORE_VULKAN_BACKEND：直接返回 GLES2Backend
/// - 定义了 GLOBECORE_VULKAN_BACKEND：优先尝试 VulkanBackend，失败时回退 GLES2Backend
///
/// 调用方须在渲染线程（拥有 GL / Vk 上下文的线程）创建与销毁后端实例。

/// 创建 GLES2 后端
std::unique_ptr<RHIBackend> createGLES2Backend();

/// 创建 Vulkan 后端（未编译 Vulkan 支持时返回 nullptr）
std::unique_ptr<RHIBackend> createVulkanBackend();

/// 工厂入口：按编译配置创建最合适的后端
std::unique_ptr<RHIBackend> createRHIBackend();

} // namespace globecore

#endif // GLOBECORE_RHI_RHI_FACTORY_H
