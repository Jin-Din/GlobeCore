#ifndef GLOBECORE_TILESET3D_TILESET3DTRANSFORM_H
#define GLOBECORE_TILESET3D_TILESET3DTRANSFORM_H

#include <array>

#include "geom/Vec3.h"

namespace globecore {

/**
 * @Jin 3D Tiles 使用列主序 double 4x4；现有渲染 Matrix4 为 float，不能直接承担 ECEF 大数矩阵累乘。
 * @Jin 本工具只在 CPU 侧做双精度变换，最终进入渲染前再按 RTC 口径转 float。
 */
class Tileset3DTransform {
public:
    using Matrix = std::array<double, 16>;

    // @Jin 单位矩阵。
    static Matrix identity();

    // @Jin 列主序矩阵乘法 result=a*b，用于父节点 transform 与子节点局部 transform 累积。
    static Matrix multiply(const Matrix &a, const Matrix &b);

    // @Jin 点变换（w=1），处理平移；boundingVolume 中心点使用该函数。
    static Vec3 transformPoint(const Matrix &m, const Vec3 &p);

    // @Jin 方向变换（w=0），不消费平移；OBB 半轴使用该函数。
    static Vec3 transformDirection(const Matrix &m, const Vec3 &v);
};

} // namespace globecore

#endif // GLOBECORE_TILESET3D_TILESET3DTRANSFORM_H
