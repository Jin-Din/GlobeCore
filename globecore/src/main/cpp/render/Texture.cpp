#include "render/Texture.h"

#include "util/Log.h"

#include <cstdint>

namespace globecore {

Texture::Texture(Texture &&other) noexcept
    : id_(other.id_), width_(other.width_), height_(other.height_) {
    other.id_ = 0;
    other.width_ = 0;
    other.height_ = 0;
}

Texture &Texture::operator=(Texture &&other) noexcept {
    if (this != &other) {
        release();
        id_ = other.id_;
        width_ = other.width_;
        height_ = other.height_;
        other.id_ = 0;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

bool Texture::uploadRGBA(const uint8_t *rgba, int width, int height) {
    if (rgba == nullptr || width <= 0 || height <= 0) return false;

    if (id_ == 0) glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    // 瓦片按 1:1 或放大显示，用线性过滤；边缘 CLAMP 避免相邻瓦片采样溢出
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);

    width_ = width;
    height_ = height;
    return true;
}

// @Jin 3D Tiles 模型纹理上传：当前引擎 EGL 为 GLES2 上下文，NPOT（非 2 次幂尺寸，倾斜摄影常见，如 506x1024）
// 在 GLES2 下只允许 CLAMP_TO_EDGE + 非 mipmap 过滤；若强行 glGenerateMipmap 并选 MIPMAP 过滤，纹理会变为
// “不完整纹理”，采样恒返回黑色（真机黑块/椒盐根因之一）。故按尺寸分流：POT 走三线性 mipmap 抑制远景缩小混叠，
// NPOT 合法回退到双线性 CLAMP。后续若把上下文升级到 GLES3，可统一走 mipmap 路径。
namespace {
bool isPowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; } // @Jin 2 次幂判定
}

bool Texture::uploadRGBAWithMipmap(const uint8_t *rgba, int width, int height) {
    if (rgba == nullptr || width <= 0 || height <= 0) return false;

    if (id_ == 0) glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // @Jin GLES2 下 NPOT 必须 CLAMP
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    // @Jin 仅 POT 纹理在 GLES2 下可合法使用 mipmap；NPOT 保持 GL_LINEAR，否则纹理不完整、采样全黑。
    const bool pot = isPowerOfTwo(width) && isPowerOfTwo(height);
    if (pot) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        LOGI("Texture NPOT fallback to LINEAR (no mipmap, GLES2 constraint): %dx%d", width, height);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    width_ = width;
    height_ = height;
    return true;
}

bool Texture::uploadCompressed(GLenum internalFormat, int width, int height,
                                const void *data, GLsizei dataSize) {
    if (data == nullptr || width <= 0 || height <= 0 || dataSize <= 0) return false;

    if (id_ == 0) glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // GLES 2.0 压缩纹理：glCompressedTexImage2D（无 internalFormat 版本选择，
    // 直接用 KTX2 解析出的 vkFormat→GL 格式映射）
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, dataSize, data);
    glBindTexture(GL_TEXTURE_2D, 0);

    width_ = width;
    height_ = height;
    return true;
}

void Texture::bind() const {
    if (id_ != 0) glBindTexture(GL_TEXTURE_2D, id_);
}

void Texture::release() {
    if (id_ != 0) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

} // namespace globecore
