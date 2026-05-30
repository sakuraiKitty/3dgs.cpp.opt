#ifndef MPM_BSPLINE_GLSL
#define MPM_BSPLINE_GLSL

/**
 * B样条基函数及其导数
 * 用于MPM中的粒子到网格(P2G)和网格到粒子(G2P)的权重计算
 *
 * @param x 相对位置（以节点为原点）
 * @return 权重和导数
 */
void bspline(float x, out float value, out float derivative) {
    float x_abs = abs(x);
    float x_abs_sq = x_abs * x_abs;

    if (x_abs < 1.0) {
        value = 0.5 * x_abs_sq - x_abs + 0.5;
        derivative = x_abs - sign(x);
    } else if (x_abs < 2.0) {
        float temp = 2.0 - x_abs;
        value = 0.5 * temp * temp;
        derivative = -sign(x) * (2.0 - x_abs);
    } else {
        value = 0.0;
        derivative = 0.0;
    }
}

/**
 * 计算3x3x3样条的权重和梯度
 *
 * @param offset 粒子在网格中的相对位置
 * @param w 输出权重 [3][3][3]
 * @param dw 输出权重梯度 [3][3][3]
 */
void compute_weights_and_gradients(
    vec3 offset,
    out vec3 w[3],
    out vec3 dw[3]
) {
    // 对每个维度计算B样条
    float wx[3], wy[3], wz[3];
    float dwx[3], dwy[3], dwz[3];

    // x维度 (offset - 1, offset, offset + 1)
    bspline(offset.x - 1.0, wx[0], dwx[0]);
    bspline(offset.x, wx[1], dwx[1]);
    bspline(offset.x + 1.0, wx[2], dwx[2]);

    // y维度
    bspline(offset.y - 1.0, wy[0], dwy[0]);
    bspline(offset.y, wy[1], dwy[1]);
    bspline(offset.y + 1.0, wy[2], dwy[2]);

    // z维度
    bspline(offset.z - 1.0, wz[0], dwz[0]);
    bspline(offset.z, wz[1], dwz[1]);
    bspline(offset.z + 1.0, wz[2], dwz[2]);

    // 组装到数组中
    w[0][0] = wx[0];  w[0][1] = wx[1];  w[0][2] = wx[2];
    w[1][0] = wy[0];  w[1][1] = wy[1];  w[1][2] = wy[2];
    w[2][0] = wz[0];  w[2][1] = wz[1];  w[2][2] = wz[2];

    dw[0][0] = dwx[0]; dw[0][1] = dwx[1]; dw[0][2] = dwx[2];
    dw[1][0] = dwy[0]; dw[1][1] = dwy[1]; dw[1][2] = dwy[2];
    dw[2][0] = dwz[0]; dw[2][1] = dwz[1]; dw[2][2] = dwz[2];
}

#endif // MPM_BSPLINE_GLSL
