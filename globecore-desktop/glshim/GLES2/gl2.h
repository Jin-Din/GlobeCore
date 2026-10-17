// 桌面端 GLES2 兼容头（仅 GLOBECORE_DESKTOP_GL=glew 后端使用）。
//
// 引擎 render/ 层一律 #include <GLES2/gl2.h>。CMake 把本目录放在 include 路径最前，
// 使该包含关系在桌面「桌面 OpenGL 2.1 + GLEW」后端下解析到本文件，render/ 源码零改动。
//
// 前提：驱动须提供 GL_ARB_ES2_compatibility（Mesa 全线支持），否则引擎内嵌的
// GLSL ES 1.00 着色器（precision/attribute/varying）在桌面 GLSL 下编译失败。
// Windows 原生 opengl32 不含该扩展 —— Windows 请用 gles2（ANGLE）后端，不要用本路径。
#ifndef GLOBECORE_DESKTOP_GLES2_GL2_H
#define GLOBECORE_DESKTOP_GLES2_GL2_H

#include <GL/glew.h>

// GLES2 独有、桌面头可能缺失的最小常量补齐（全部 #ifndef 守卫，与 glext 冲突时择一）
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_SAMPLER_2D
#define GL_SAMPLER_2D 0x8B5E
#endif
#ifndef GL_SAMPLER_CUBE
#define GL_SAMPLER_CUBE 0x8B60
#endif
#ifndef GL_FRAGMENT_PRECISION_HIGH
#define GL_FRAGMENT_PRECISION_HIGH 0x8123
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_ZERO
#define GL_ZERO 0
#endif
#ifndef GL_ONE
#define GL_ONE 1
#endif
#ifndef GL_SRC_ALPHA_SATURATE
#define GL_SRC_ALPHA_SATURATE 0x0308
#endif

#endif // GLOBECORE_DESKTOP_GLES2_GL2_H
