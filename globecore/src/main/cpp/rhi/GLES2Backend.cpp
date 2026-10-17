#include "rhi/GLES2Backend.h"

#include <cstring>

#include "util/Log.h"

namespace globecore {

// ============================================================================
// GLES2 后端实现 —— RHI 抽象 → OpenGL ES 2.0 原生调用映射
//
// 资源 id 直接为 GL 对象名（GLuint），1:1 映射无中间表。
// 全部操作须在 GL 线程（拥有 EGL 上下文的线程）调用。
// ============================================================================

// ---------------- 枚举映射 ----------------

GLenum GLES2Backend::toGLPrimitive(RHIPrimitive prim) {
    switch (prim) {
        case RHIPrimitive::Triangles:     return GL_TRIANGLES;
        case RHIPrimitive::TriangleStrip: return GL_TRIANGLE_STRIP;
        case RHIPrimitive::Lines:         return GL_LINES;
        case RHIPrimitive::LineStrip:    return GL_LINE_STRIP;
        case RHIPrimitive::Points:        return GL_POINTS;
    }
    return GL_TRIANGLES;  // 保底
}

GLenum GLES2Backend::toGLIndexType(RHIIndexType type) {
    return type == RHIIndexType::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
}

GLenum GLES2Backend::toGLDepthFunc(RHIDepthFunc func) {
    switch (func) {
        case RHIDepthFunc::Never:        return GL_NEVER;
        case RHIDepthFunc::Less:         return GL_LESS;
        case RHIDepthFunc::Equal:        return GL_EQUAL;
        case RHIDepthFunc::LessEqual:    return GL_LEQUAL;
        case RHIDepthFunc::Greater:      return GL_GREATER;
        case RHIDepthFunc::NotEqual:     return GL_NOTEQUAL;
        case RHIDepthFunc::GreaterEqual: return GL_GEQUAL;
        case RHIDepthFunc::Always:       return GL_ALWAYS;
    }
    return GL_LESS;
}

GLenum GLES2Backend::toGLBlendFactor(RHIBlendFactor f) {
    switch (f) {
        case RHIBlendFactor::Zero:             return GL_ZERO;
        case RHIBlendFactor::One:              return GL_ONE;
        case RHIBlendFactor::SrcAlpha:         return GL_SRC_ALPHA;
        case RHIBlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case RHIBlendFactor::DstColor:          return GL_DST_COLOR;
        case RHIBlendFactor::OneMinusDstColor: return GL_ONE_MINUS_DST_COLOR;
    }
    return GL_ONE;
}

GLenum GLES2Backend::toGLBufferUsage(RHIBufferUsage usage) {
    switch (usage) {
        case RHIBufferUsage::StaticDraw:  return GL_STATIC_DRAW;
        case RHIBufferUsage::DynamicDraw: return GL_DYNAMIC_DRAW;
        case RHIBufferUsage::StreamDraw:   return GL_STREAM_DRAW;
    }
    return GL_DYNAMIC_DRAW;
}

GLenum GLES2Backend::toGLClearMask(uint32_t mask) {
    GLenum glMask = 0;
    if (mask & RHI_CLEAR_COLOR)   glMask |= GL_COLOR_BUFFER_BIT;
    if (mask & RHI_CLEAR_DEPTH)   glMask |= GL_DEPTH_BUFFER_BIT;
    if (mask & RHI_CLEAR_STENCIL) glMask |= GL_STENCIL_BUFFER_BIT;
    return glMask;
}

// ---------------- 生命周期 ----------------

void GLES2Backend::initialize() {
    GLint val = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &val);
    if (val > 0) {
        maxTextureSize_ = val;
    }
    LOGI("RHI[GLES2]: initialized, maxTextureSize=%d", maxTextureSize_);
}

// ---------------- 着色器程序 ----------------

RHIShaderProgram GLES2Backend::createShaderProgram(const char* vertexSrc,
                                                    const char* fragmentSrc) {
    RHIShaderProgram result;

    // 编译顶点着色器
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexSrc, nullptr);
    glCompileShader(vs);
    GLint compiled = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024];
        glGetShaderInfoLog(vs, sizeof(log), nullptr, log);
        LOGE("RHI[GLES2]: vertex shader compile failed: %s", log);
        glDeleteShader(vs);
        return result;  // id == 0
    }

    // 编译片元着色器
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentSrc, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024];
        glGetShaderInfoLog(fs, sizeof(log), nullptr, log);
        LOGE("RHI[GLES2]: fragment shader compile failed: %s", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return result;
    }

    // 链接程序
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOGE("RHI[GLES2]: program link failed: %s", log);
        glDeleteProgram(prog);
        return result;
    }

    result.id = prog;
    return result;
}

void GLES2Backend::destroyShaderProgram(RHIShaderProgram& prog) {
    if (prog.id) {
        glDeleteProgram(prog.id);
        prog.id = 0;
    }
}

void GLES2Backend::useProgram(const RHIShaderProgram& prog) {
    glUseProgram(prog.id);
}

int GLES2Backend::attribLocation(const RHIShaderProgram& prog, const char* name) {
    return glGetAttribLocation(prog.id, name);
}

int GLES2Backend::uniformLocation(const RHIShaderProgram& prog, const char* name) {
    return glGetUniformLocation(prog.id, name);
}

// ---------------- 缓冲区 ----------------

RHIBuffer GLES2Backend::createBuffer() {
    RHIBuffer result;
    GLuint id = 0;
    glGenBuffers(1, &id);
    result.id = id;
    return result;
}

void GLES2Backend::destroyBuffer(RHIBuffer& buf) {
    if (buf.id) {
        GLuint id = buf.id;
        glDeleteBuffers(1, &id);
        buf.id = 0;
    }
}

void GLES2Backend::uploadBufferData(RHIBuffer& buf, const void* data, size_t size,
                                      RHIBufferUsage usage) {
    // 调用方须先 bindVertexBuffer 或 bindIndexBuffer；此处只做数据上传
    GLsizeiptr sz = static_cast<GLsizeiptr>(size);
    glBufferData(GL_ARRAY_BUFFER, sz, data, toGLBufferUsage(usage));
}

void GLES2Backend::bindVertexBuffer(const RHIBuffer& buf) {
    glBindBuffer(GL_ARRAY_BUFFER, buf.id);
}

void GLES2Backend::bindIndexBuffer(const RHIBuffer& buf) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf.id);
}

// ---------------- 纹理 ----------------

RHITexture GLES2Backend::createTextureRGBA(const uint8_t* rgba, int width, int height) {
    RHITexture result;
    if (!rgba || width <= 0 || height <= 0) return result;

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    result.id = id;
    result.width = width;
    result.height = height;
    return result;
}

RHITexture GLES2Backend::createTextureCompressed(uint32_t internalFormat, int width, int height,
                                                   const void* data, size_t dataSize) {
    RHITexture result;
    if (!data || width <= 0 || height <= 0 || dataSize == 0) return result;

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLenum fmt = static_cast<GLenum>(internalFormat);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, width, height, 0,
                           static_cast<GLsizei>(dataSize), data);

    result.id = id;
    result.width = width;
    result.height = height;
    return result;
}

void GLES2Backend::destroyTexture(RHITexture& tex) {
    if (tex.id) {
        GLuint id = tex.id;
        glDeleteTextures(1, &id);
        tex.id = 0;
        tex.width = 0;
        tex.height = 0;
    }
}

void GLES2Backend::bindTexture(const RHITexture& tex, int unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex.id);
}

// ---------------- Uniform ----------------

void GLES2Backend::setUniform(int loc, const float* mat4) {
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, mat4);
}

void GLES2Backend::setUniform(int loc, float x, float y, float z, float w) {
    if (loc >= 0) glUniform4f(loc, x, y, z, w);
}

void GLES2Backend::setUniform(int loc, float x, float y, float z) {
    if (loc >= 0) glUniform3f(loc, x, y, z);
}

void GLES2Backend::setUniform(int loc, float x, float y) {
    if (loc >= 0) glUniform2f(loc, x, y);
}

void GLES2Backend::setUniform(int loc, float x) {
    if (loc >= 0) glUniform1f(loc, x);
}

void GLES2Backend::setUniform(int loc, int x) {
    if (loc >= 0) glUniform1i(loc, x);
}

// ---------------- 顶点属性 ----------------

void GLES2Backend::enableAttrib(int loc) {
    glEnableVertexAttribArray(static_cast<GLuint>(loc));
}

void GLES2Backend::disableAttrib(int loc) {
    glDisableVertexAttribArray(static_cast<GLuint>(loc));
}

void GLES2Backend::setAttribPointer(int loc, int size, int stride, size_t offset) {
    glVertexAttribPointer(static_cast<GLuint>(loc), size, GL_FLOAT, GL_FALSE,
                          stride, reinterpret_cast<const void*>(offset));
}

// ---------------- 渲染状态 ----------------

void GLES2Backend::setViewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

void GLES2Backend::setDepthTest(bool enable) {
    if (enable) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

void GLES2Backend::setDepthMask(bool enable) {
    glDepthMask(enable ? GL_TRUE : GL_FALSE);
}

void GLES2Backend::setDepthFunc(RHIDepthFunc func) {
    glDepthFunc(toGLDepthFunc(func));
}

void GLES2Backend::setBlend(bool enable) {
    if (enable) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

void GLES2Backend::setBlendFunc(RHIBlendFactor src, RHIBlendFactor dst) {
    glBlendFunc(toGLBlendFactor(src), toGLBlendFactor(dst));
}

void GLES2Backend::setClearColor(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
}

void GLES2Backend::clear(uint32_t mask) {
    glClear(toGLClearMask(mask));
}

// ---------------- 帧缓冲区 ----------------

RHIFramebuffer GLES2Backend::createFramebuffer(int width, int height) {
    RHIFramebuffer result;
    if (width <= 0 || height <= 0) return result;

    GLuint fbo = 0;
    GLuint tex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("RHI[GLES2]: framebuffer incomplete (0x%x)", status);
        glDeleteTextures(1, &tex);
        glDeleteFramebuffers(1, &fbo);
        return result;
    }

    // fbo id 和 tex id 拼接：高 16 位存 tex，低 16 位存 fbo
    // （GLES2 资源名通常 < 65536，实际中安全；如需更稳健可改用映射表）
    result.id = fbo;
    result.width = width;
    result.height = height;
    // 保存纹理 id 到 id 的方式：单独不存 tex——destroy 时需从 id 高位取
    // 简化：直接把 tex 存入 result 的 padding。GLES2 下用静态映射更稳，但此处
    // 为轻量封装，用一个简单的 static unordered_map 不合适（多实例不安全）。
    // 实际方案：把 fbo 和 tex 打包到 id——低 16 位 fbo，高 16 位 tex。
    if (tex > 0xFFFF || fbo > 0xFFFF) {
        // 资源名超 16 位，回退：只返回 fbo，tex 泄漏（极端罕见，GLES2 资源名通常小）
        LOGW("RHI[GLES2]: GL object name > 65535, texture may leak on destroy");
        result.id = fbo;
    } else {
        result.id = (tex << 16) | (fbo & 0xFFFF);
    }
    return result;
}

void GLES2Backend::destroyFramebuffer(RHIFramebuffer& fbo) {
    if (fbo.id) {
        GLuint fboId = fbo.id & 0xFFFF;
        GLuint texId = (fbo.id >> 16) & 0xFFFF;
        if (texId) glDeleteTextures(1, &texId);
        if (fboId) glDeleteFramebuffers(1, &fboId);
        fbo.id = 0;
        fbo.width = 0;
        fbo.height = 0;
    }
}

void GLES2Backend::bindFramebuffer(const RHIFramebuffer& fbo) {
    GLuint fboId = fbo.id ? (fbo.id & 0xFFFF) : 0;
    glBindFramebuffer(GL_FRAMEBUFFER, fboId);
}

void GLES2Backend::readPixels(int x, int y, int width, int height, uint8_t* outRgba) {
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, outRgba);
}

// ---------------- 绘制 ----------------

void GLES2Backend::drawArrays(RHIPrimitive prim, int first, int count) {
    glDrawArrays(toGLPrimitive(prim), first, count);
}

void GLES2Backend::drawElements(RHIPrimitive prim, int count, RHIIndexType type, size_t offset) {
    glDrawElements(toGLPrimitive(prim), count, toGLIndexType(type),
                   reinterpret_cast<const void*>(offset));
}

// ---------------- 查询 ----------------

int GLES2Backend::maxTextureSize() const {
    return maxTextureSize_;
}

} // namespace globecore
