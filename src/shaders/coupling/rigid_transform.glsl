#ifndef RIGID_TRANSFORM_GLSL
#define RIGID_TRANSFORM_GLSL

/**
 * 刚性变换库（四元数格式统一为 vec4(w,x,y,z)）
 *
 * CRITICAL FIX: 所有四元数操作必须使用 vec4(w,x,y,z) 格式
 * 即 vec4.x = w(标量), vec4.y = x, vec4.z = y, vec4.w = z
 * 这与 common.glsl 中 rotationFromQuaternion() 的格式一致
 * 也与 PLY 文件中高斯旋转数据格式一致
 * 也与 GLM glm::quat 格式一致
 *
 * 之前的BUG:
 * 1. quaternion_multiply 假设 vec4(x,y,z,w) 格式，但数据实际是 vec4(w,x,y,z) → 乘法完全错误
 * 2. matrix_to_quaternion 分量符号全部反转 → 输出的是共轭四元数(逆旋转)
 */

// ============================================================================
// 四元数操作（vec4(w,x,y,z) 格式：.x=w, .y=x, .z=y, .w=z）
// ============================================================================

/**
 * 四元数归一化
 * q = vec4(w, x, y, z) where .x=w, .y=x, .z=y, .w=z
 */
vec4 quaternion_normalize(vec4 q) {
    // q.x=w, q.y=x, q.z=y, q.w=z
    float len = sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-6) {
        return vec4(1.0, 0.0, 0.0, 0.0); // identity: w=1, x=0, y=0, z=0
    }
    return q / len;
}

/**
 * 四元数乘法（vec4(w,x,y,z) 格式）
 * 对应 Python rigid_body_utils.py quaternion_multiply()
 *
 * a = vec4(aw, ax, ay, az) where a.x=aw, a.y=ax, a.z=ay, a.w=az
 * b = vec4(bw, bx, by, bz) where b.x=bw, b.y=bx, b.z=by, b.w=bz
 *
 * Standard quaternion product:
 * ow = aw*bw - ax*bx - ay*by - az*bz
 * ox = aw*bx + ax*bw + ay*bz - az*by
 * oy = aw*by - ax*bz + ay*bw + az*bx
 * oz = aw*bz + ax*by - ay*bx + az*bw
 *
 * In vec4(w,x,y,z) format output:
 * result.x = ow, result.y = ox, result.z = oy, result.w = oz
 */
vec4 quaternion_multiply(vec4 a, vec4 b) {
    // Extract components in (w,x,y,z) format
    // a.x=aw, a.y=ax, a.z=ay, a.w=az
    // b.x=bw, b.y=bx, b.z=by, b.w=bz
    float aw = a.x, ax = a.y, ay = a.z, az = a.w;
    float bw = b.x, bx = b.y, by = b.z, bz = b.w;

    // Standard quaternion product
    float ow = aw * bw - ax * bx - ay * by - az * bz;
    float ox = aw * bx + ax * bw + ay * bz - az * by;
    float oy = aw * by - ax * bz + ay * bw + az * bx;
    float oz = aw * bz + ax * by - ay * bx + az * bw;

    // Output in vec4(w,x,y,z) format: .x=w, .y=x, .z=y, .w=z
    return quaternion_normalize(vec4(ow, ox, oy, oz));
}

/**
 * 旋转矩阵转四元数（vec4(w,x,y,z) 格式）
 * 对应 Python rigid_body_utils.py matrix_to_quaternion()
 * 使用 Shepperd's 方法，数值稳定
 *
 * CRITICAL FIX: 之前所有分量符号都是反转的
 * 正确公式（GLSL column-major mat3 中 R[col][row] = m_{row}{col}）:
 *   qw = s/4
 *   qx = (m21 - m12) / s = (R[1][2] - R[2][1]) / s  ← 之前写的是 (R[2][1] - R[1][2]) 反了!
 *   qy = (m02 - m20) / s = (R[2][0] - R[0][2]) / s  ← 之前写的是 (R[0][2] - R[2][0]) 反了!
 *   qz = (m10 - m01) / s = (R[0][1] - R[1][0]) / s  ← 之前写的是 (R[1][0] - R[0][1]) 反了!
 */
vec4 matrix_to_quaternion(mat3 R) {
    // GLSL column-major: R[col][row] = m_{row}{col}
    float m00 = R[0][0], m10 = R[0][1], m20 = R[0][2];
    float m01 = R[1][0], m11 = R[1][1], m21 = R[1][2];
    float m02 = R[2][0], m12 = R[2][1], m22 = R[2][2];

    float tr = m00 + m11 + m22;

    if (tr > 0.0) {
        float s = sqrt(tr + 1.0) * 2.0; // s = 4 * qw
        return vec4(
            0.25 * s,                    // qw → .x
            (m21 - m12) / s,             // qx → .y  (CORRECTED sign)
            (m02 - m20) / s,             // qy → .z  (CORRECTED sign)
            (m10 - m01) / s              // qz → .w  (CORRECTED sign)
        );
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrt(1.0 + m00 - m11 - m22) * 2.0; // s = 4 * qx
        return vec4(
            (m21 - m12) / s,             // qw → .x  (CORRECTED sign)
            0.25 * s,                    // qx → .y
            (m10 + m01) / s,             // qy → .z
            (m20 + m02) / s              // qz → .w
        );
    } else if (m11 > m22) {
        float s = sqrt(1.0 + m11 - m00 - m22) * 2.0; // s = 4 * qy
        return vec4(
            (m02 - m20) / s,             // qw → .x  (CORRECTED sign)
            (m10 + m01) / s,             // qx → .y
            0.25 * s,                    // qy → .z
            (m21 + m12) / s              // qz → .w
        );
    } else {
        float s = sqrt(1.0 + m22 - m00 - m11) * 2.0; // s = 4 * qz
        return vec4(
            (m10 - m01) / s,             // qw → .x  (CORRECTED sign)
            (m20 + m02) / s,             // qx → .y
            (m21 + m12) / s,             // qy → .z
            0.25 * s                     // qz → .w
        );
    }
}

// ============================================================================
// SVD 分解 (Jacobi 迭代方法)
// ============================================================================

struct SVDResult {
    mat3 U;  // 左奇异向量
    mat3 V;  // 右奇异向量
};

/**
 * SVD 分解的简化实现 (Jacobi 迭代)
 * A = U * S * V^T
 * 最多迭代 10 次，适用于 3x3 矩阵
 */
SVDResult svd_jacobi(mat3 A) {
    SVDResult result;
    mat3 V = mat3(1.0);  // 初始为单位矩阵
    mat3 U = A;

    // 最多 10 次迭代
    for (int iter = 0; iter < 10; iter++) {
        // 找最大非对角元素
        int p = 0, q = 1;
        float max_val = abs(U[0][1]);

        if (abs(U[0][2]) > max_val) {
            max_val = abs(U[0][2]);
            p = 0; q = 2;
        }
        if (abs(U[1][2]) > max_val) {
            max_val = abs(U[1][2]);
            p = 1; q = 2;
        }

        // 收敛判断
        if (max_val < 1e-6) {
            break;
        }

        // Jacobi 旋转参数
        float theta = 0.0;
        if (abs(U[p][p] - U[q][q]) > 1e-6) {
            theta = atan(2.0 * U[p][q], U[q][q] - U[p][p]) * 0.5;
        } else {
            if (U[p][q] > 0.0) {
                theta = 3.14159265359 * 0.25;
            } else {
                theta = -3.14159265359 * 0.25;
            }
        }

        float c = cos(theta);
        float s = sin(theta);

        // 更新 U: U = J^T * U
        for (int i = 0; i < 3; i++) {
            float temp = c * U[i][p] - s * U[i][q];
            U[i][q] = s * U[i][p] + c * U[i][q];
            U[i][p] = temp;
        }

        // 更新 V: V = V * J
        for (int i = 0; i < 3; i++) {
            float temp = c * V[i][p] - s * V[i][q];
            V[i][q] = s * V[i][p] + c * V[i][q];
            V[i][p] = temp;
        }
    }

    result.U = U;
    result.V = V;
    return result;
}

// ============================================================================
// 刚性变换
// ============================================================================

struct RigidTransform {
    mat3 R;  // 旋转矩阵 (3x3)
    vec3 t;  // 平移向量
};

/**
 * 拟合刚性变换 (对应 Python get_rigid_transform)
 * 使用加权最小二乘法找到最优的 R 和 t
 *
 * 输入：
 * - source_points: 源点集 (K 个点, 归一化空间)
 * - target_points: 目标点集 (K 个点, 归一化空间)
 * - weights: 权重 (K 个值)
 *
 * 输出：
 * - RigidTransform {R, t} 使得 target ≈ R * source + t
 */
RigidTransform fit_rigid_transform(
    vec3 source_points[8],
    vec3 target_points[8],
    float weights[8]
) {
    RigidTransform transform;
    transform.R = mat3(1.0);
    transform.t = vec3(0.0);

    // 1. 计算加权质心
    vec3 centroid_A = vec3(0.0);
    vec3 centroid_B = vec3(0.0);
    float weight_sum = 0.0;

    for (int i = 0; i < 8; i++) {
        centroid_A += source_points[i] * weights[i];
        centroid_B += target_points[i] * weights[i];
        weight_sum += weights[i];
    }

    if (weight_sum < 1e-6) {
        return transform; // 无效权重，返回单位变换
    }

    centroid_A /= weight_sum;
    centroid_B /= weight_sum;

    // 2. 计算交叉协方差矩阵 H = A_centered^T * B_centered
    // 对应 Python: H = A_centered.transpose(-2, -1) @ B_centered
    // 注意: Python H[i,j] = sum_k (A_centered[k][i] * B_centered[k][j])
    // 在 GLSL column-major 中: H[col][row] = H_{row}{col}
    // 所以 H_{ij} = H[col=j][row=i]
    mat3 H = mat3(0.0);

    for (int i = 0; i < 8; i++) {
        vec3 a = source_points[i] - centroid_A;
        vec3 b = target_points[i] - centroid_B;

        // 外积累加: a * b^T
        // a*b^T 的 (i,j) 元素 = a[i] * b[j]
        // 在 GLSL column-major: H[col=j][row=i] = a[i] * b[j]
        // H[0][0] = a.x * b.x (row=0, col=0)
        // H[0][1] = a.y * b.x (row=1, col=0)
        // H[0][2] = a.z * b.x (row=2, col=0)
        // H[1][0] = a.x * b.y (row=0, col=1)
        // H[1][1] = a.y * b.y (row=1, col=1)
        // H[1][2] = a.z * b.y (row=2, col=1)
        // H[2][0] = a.x * b.z (row=0, col=2)
        // H[2][1] = a.y * b.z (row=1, col=2)
        // H[2][2] = a.z * b.z (row=2, col=2)
        H[0][0] += weights[i] * a.x * b.x;
        H[0][1] += weights[i] * a.y * b.x;
        H[0][2] += weights[i] * a.z * b.x;

        H[1][0] += weights[i] * a.x * b.y;
        H[1][1] += weights[i] * a.y * b.y;
        H[1][2] += weights[i] * a.z * b.y;

        H[2][0] += weights[i] * a.x * b.z;
        H[2][1] += weights[i] * a.y * b.z;
        H[2][2] += weights[i] * a.z * b.z;
    }

    // 3. SVD 分解: H = U * S * V^T
    SVDResult svd = svd_jacobi(H);

    // 4. 计算 R = V * U^T
    // 对应 Python: R = Vt.transpose(-2, -1) @ U.transpose(-2, -1)
    // 即 R = V * U^T
    // In GLSL column-major: transpose(X) swaps rows and columns
    mat3 R = transpose(svd.V) * transpose(svd.U);

    // 5. 确保右手坐标系 (det(R) = +1)
    float det_R = determinant(R);
    if (det_R < 0.0) {
        // 反转 V 的第三列（column 2）
        svd.V[2] *= -1.0;
        R = transpose(svd.V) * transpose(svd.U);
    }

    // 6. 计算平移: t = centroid_B - R * centroid_A
    vec3 t = centroid_B - R * centroid_A;

    transform.R = R;
    transform.t = t;

    return transform;
}

#endif // RIGID_TRANSFORM_GLSL
