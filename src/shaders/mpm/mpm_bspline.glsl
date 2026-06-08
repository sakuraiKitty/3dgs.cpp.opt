#ifndef MPM_BSPLINE_GLSL
#define MPM_BSPLINE_GLSL

/**
 * 三次B样条基函数及其导数
 * 对应 physDreamer: mpm_utils.py 第372-383行
 *
 * 公式:
 *   N(x) = 0.5 * |x - 0.5|² - |x - 0.5| + 0.5,  for |x| < 1
 *   N(x) = 0.5 * (1.5 - |x|)²,                    for 1 ≤ |x| < 2
 *   N(x) = 0,                                      otherwise
 *
 * 导数:
 *   N'(x) = x - 0.5 * sign(x - 0.5),              for |x| < 1
 *   N'(x) = -sign(x) * (1.5 - |x|),               for 1 ≤ |x| < 2
 *   N'(x) = 0,                                     otherwise
 *
 * @param x 相对位置偏移
 * @param value 输出: B样条权重值
 * @param derivative 输出: B样条导数值
 */
void bspline_cubic(float x, out float value, out float derivative) {
    float x_abs = abs(x);
    float x_abs_sq = x_abs * x_abs;

    if (x_abs < 1.0) {
        // 内部区域: |x| < 1
        value = 0.5 * x_abs_sq - x_abs + 0.5;
        derivative = x - sign(x - 0.5);
    } else if (x_abs < 2.0) {
        // 外部区域: 1 ≤ |x| < 2
        float temp = 1.5 - x_abs;
        value = 0.5 * temp * temp;
        derivative = -sign(x) * temp;
    } else {
        // 超出影响范围
        value = 0.0;
        derivative = 0.0;
    }
}

/**
 * B样条权重和梯度结构
 * 用于3x3x3插值
 */
struct BSplineWeights {
    // 一维权重 (wx[0], wx[1], wx[2])
    float wx[3];
    float wy[3];
    float wz[3];

    // 一维导数
    float dwx[3];
    float dwy[3];
    float dwz[3];

    // 3x3x3 权重张量 w[i][j][k] = wx[i] * wy[j] * wz[k]
    float w[3][3][3];

    // 3x3x3 梯度权重张量
    vec3 dw[3][3][3];
};

/**
 * 计算3x3x3 B样条权重张量
 * 对应 physDreamer: mpm_utils.py compute_dweight()
 *
 * @param fx 相对位置偏移 (grid_pos - base_node)，范围在 [0, 1]
 * @param out_weights 输出完整的权重和梯度结构
 */
void compute_bspine_weights_3x3x3(vec3 fx, out BSplineWeights out_weights) {
    // 对每个维度计算B样条基函数
    // 节点偏移: -1, 0, +1 (对应3x3x3邻居)
    for (int i = 0; i < 3; i++) {
        float offset_x = fx.x - float(i - 1);
        float offset_y = fx.y - float(i - 1);
        float offset_z = fx.z - float(i - 1);

        bspline_cubic(offset_x, out_weights.wx[i], out_weights.dwx[i]);
        bspline_cubic(offset_y, out_weights.wy[i], out_weights.dwy[i]);
        bspline_cubic(offset_z, out_weights.wz[i], out_weights.dwz[i]);
    }

    // 构建3x3x3权重张量 (外积: w[i][j][k] = wx[i] * wy[j] * wz[k])
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 3; k++) {
                out_weights.w[i][j][k] = out_weights.wx[i] * out_weights.wy[j] * out_weights.wz[k];
            }
        }
    }

    // 构建3x3x3梯度权重张量
    // ∇(wx[i] * wy[j] * wz[k]) = [dwx[i] * wy[j] * wz[k], wx[i] * dwy[j] * wz[k], wx[i] * wy[j] * dwz[k]]
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 3; k++) {
                float dwx = out_weights.dwx[i] * out_weights.wy[j] * out_weights.wz[k];
                float dwy = out_weights.wx[i] * out_weights.dwy[j] * out_weights.wz[k];
                float dwz = out_weights.wx[i] * out_weights.wy[j] * out_weights.dwz[k];
                out_weights.dw[i][j][k] = vec3(dwx, dwy, dwz);
            }
        }
    }
}

/**
 * 计算插值权重梯度（用于应力力计算）
 * 对应 physDreamer: mpm_utils.py compute_dweight() 函数
 *
 * @param w 3x3x3权重张量
 * @param dw 3x3x3梯度权重张量
 * @param i x维度索引 [0-2]
 * @param j y维度索引 [0-2]
 * @param k z维度索引 [0-2]
 * @param inv_dx 网格间距的倒数 (1/dx)
 * @return 梯度向量 ∇w_ijk
 */
vec3 compute_dweight(
    BSplineWeights weights,
    int i, int j, int k,
    float inv_dx
) {
    // ∇(wx[i] * wy[j] * wz[k]) / dx
    return weights.dw[i][j][k] * inv_dx;
}

/**
 * 验证B样条权重归一化
 * 用于调试：验证所有权重的和是否为1
 *
 * @param weights 权重结构
 * @return 权重和（应该接近1.0）
 */
float validate_weights_sum(BSplineWeights weights) {
    float sum = 0.0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 3; k++) {
                sum += weights.w[i][j][k];
            }
        }
    }
    return sum;
}

#endif // MPM_BSPLINE_GLSL
