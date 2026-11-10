#include "tileset3d/Tileset3DTransform.h"

namespace globecore {

Tileset3DTransform::Matrix Tileset3DTransform::identity() {
    // @Jin 列主序单位矩阵：对角元素位于 0/5/10/15。
    return {1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0};
}

Tileset3DTransform::Matrix Tileset3DTransform::multiply(const Matrix &a, const Matrix &b) {
    Matrix out{};
    // @Jin out[col*4+row] = Σ_k a[k*4+row] * b[col*4+k]。
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[static_cast<size_t>(k * 4 + row)] *
                       b[static_cast<size_t>(col * 4 + k)];
            }
            out[static_cast<size_t>(col * 4 + row)] = sum;
        }
    }
    return out;
}

Vec3 Tileset3DTransform::transformPoint(const Matrix &m, const Vec3 &p) {
    // @Jin 3D Tiles 为齐次坐标 [x,y,z,1]，显式加回平移列 m12/m13/m14。
    return {
        m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]
    };
}

Vec3 Tileset3DTransform::transformDirection(const Matrix &m, const Vec3 &v) {
    // @Jin 方向向量使用 w=0，避免 ECEF 平移污染 OBB 半轴方向和长度。
    return {
        m[0] * v.x + m[4] * v.y + m[8]  * v.z,
        m[1] * v.x + m[5] * v.y + m[9]  * v.z,
        m[2] * v.x + m[6] * v.y + m[10] * v.z
    };
}

} // namespace globecore
