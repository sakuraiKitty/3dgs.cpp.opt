#ifndef MPM_STRESS_GLSL
#define MPM_STRESS_GLSL

/**
 * MPM 应力计算函数库
 * 从变形梯度 F 计算 First Piola-Kirchhoff 应力 P
 *
 * 使用 FCR (Fixed Corotated) 材料模型
 * 对应: mpm_utils.py compute_stress_from_F_trial() 第604行
 *
 * 原为独立 compute_stress.comp shader，现合并到 P2G 中 inline 计算，
 * 避免 compute_stress 覆盖 apic_matrix (APIC C 矩阵) 的 bug。
 */

// 材料类型常量由 mpm_structs.glsl 定义，此文件不重复定义
// (MATERIAL_JELLY, MATERIAL_METAL, etc.)

/**
 * 安全归一化：长度接近 0 时返回零向量，避免 normalize(0) 产生 NaN
 * 用于 F 退化（某列趋零）时提取旋转矩阵，防止应力 NaN 传播
 */
vec3 safe_normalize(vec3 v) {
    float len = length(v);
    if (len < 1e-8) {
        return vec3(0.0);
    }
    return v / len;
}

/**
 * Gram-Schmidt 正交化：从变形梯度 F 提取旋转矩阵 R
 * 简化的极分解实现
 * 使用 safe_normalize：F 退化时返回零列而非 NaN，保证应力有限
 */
mat3 extract_rotation_gram_schmidt(mat3 F) {
    vec3 c0 = F[0];
    vec3 c1 = F[1];
    vec3 c2 = F[2];

    vec3 r0 = safe_normalize(c0);

    float d1 = dot(c1, r0);
    vec3 r1 = c1 - d1 * r0;
    r1 = safe_normalize(r1);

    float d2 = dot(c2, r0);
    float d3 = dot(c2, r1);
    vec3 r2 = c2 - d2 * r0 - d3 * r1;
    r2 = safe_normalize(r2);

    return mat3(r0, r1, r2);
}

/**
 * 3x3 矩阵行列式
 */
float mat3_determinant(mat3 m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[2][1] * m[1][2]) -
           m[0][1] * (m[1][0] * m[2][2] - m[2][0] * m[1][2]) +
           m[0][2] * (m[1][0] * m[2][1] - m[2][0] * m[1][1]);
}

/**
 * 3x3 矩阵转置
 */
mat3 mat3_transpose(mat3 m) {
    return mat3(
        m[0][0], m[1][0], m[2][0],
        m[0][1], m[1][1], m[2][1],
        m[0][2], m[1][2], m[2][2]
    );
}

/**
 * 3x3 矩阵逆 (伴随矩阵方法)
 */
mat3 mat3_inverse(mat3 m) {
    float det = mat3_determinant(m);

    if (abs(det) < 1e-6) {
        return mat3(1.0);
    }

    float inv_det = 1.0 / det;

    mat3 adjugate = mat3(
        (m[1][1] * m[2][2] - m[2][1] * m[1][2]),
        (m[0][2] * m[2][1] - m[2][2] * m[0][1]),
        (m[0][1] * m[1][2] - m[1][1] * m[0][2]),

        (m[1][2] * m[2][0] - m[2][2] * m[1][0]),
        (m[0][0] * m[2][2] - m[2][0] * m[0][2]),
        (m[0][2] * m[1][0] - m[1][2] * m[0][0]),

        (m[1][0] * m[2][1] - m[2][0] * m[1][1]),
        (m[0][1] * m[2][0] - m[2][1] * m[0][0]),
        (m[0][0] * m[1][1] - m[1][0] * m[0][1])
    );

    return adjugate * inv_det;
}

/**
 * 极分解提取旋转 R（Newton 迭代，Higham）
 * R = F · (F^T·F)^(-1/2)，与 SVD 的 R=U·V^T **完全等价**。
 * 与 PhysDreamer wp.svd3 取 R 一致。
 *
 * 为什么不用 Gram-Schmidt：Gram-Schmidt 在大旋转下偏离极分解 R，
 * 使 (F-R) 含伪分量 → 应力方向偏离真实弹性势能梯度 → 非保守/自平衡伪应力。
 * 拖拽后花头 F 为 94% 旋转：Gram-Schmidt R 错 → 大但自平衡的伪应力 → 花头卡死不回弹。
 * 极分解 R 精确 → 应力正确对齐能量梯度 → 真实恢复力。
 *
 * 迭代 R <- 0.5*(R + R^{-T})，对接近正交的 F（旋转主导）收敛极快（~3 步）。
 * F 近奇异（det≈0）时回退 Gram-Schmidt 避免奇异迭代。
 */
mat3 extract_rotation_polar(mat3 F) {
    if (abs(mat3_determinant(F)) < 1e-6) {
        return extract_rotation_gram_schmidt(F);
    }
    mat3 R = F;
    for (int i = 0; i < 5; i++) {
        mat3 R_inv = mat3_inverse(R);
        mat3 R_inv_T = mat3_transpose(R_inv);
        R = (R + R_inv_T) * 0.5;
    }
    return R;
}

/**
 * 3x3 矩阵迹
 */
float mat3_trace(mat3 m) {
    return m[0][0] + m[1][1] + m[2][2];
}

/**
 * 应力偏量
 */
mat3 compute_deviatoric(mat3 stress) {
    float mean_stress = mat3_trace(stress) / 3.0;
    return stress - mat3(mean_stress);
}

/**
 * von Mises 等效应力
 */
float compute_von_mises_stress(mat3 stress) {
    mat3 deviatoric = compute_deviatoric(stress);
    float j2 = 0.5 * (
        deviatoric[0][0]*deviatoric[0][0] + deviatoric[0][1]*deviatoric[0][1] + deviatoric[0][2]*deviatoric[0][2] +
        deviatoric[1][0]*deviatoric[1][0] + deviatoric[1][1]*deviatoric[1][1] + deviatoric[1][2]*deviatoric[1][2] +
        deviatoric[2][0]*deviatoric[2][0] + deviatoric[2][1]*deviatoric[2][1] + deviatoric[2][2]*deviatoric[2][2]
    );
    return sqrt(3.0 * j2);
}

/**
 * FCR (Fixed Corotated) Kirchhoff 应力 - 弹性材料
 * τ = 2*mu*(F - R)*F^T + lam*J*(J - 1)*I
 *
 * 与 CUDA mpm_utils.py kirchoff_stress_FCR 完全一致。
 * 关键：返回 Kirchhoff 应力 τ（而非 PK1 P），与世界梯度 ∇ₓw 搭配使用——
 *   force = -V * τ * ∇ₓw  才能量守恒（等价于 -V * P * ∇ₓw，因 τ = P·Fᵀ）。
 * 体积项用 I 而非 F^(-T)：det(F)→0 时不再发散，消除拖拽压缩时的应力爆炸。
 */
mat3 compute_fcr_stress(mat3 F, float E, float nu) {
    float mu = E / (2.0 * (1.0 + nu));
    float lam = E * nu / ((1.0 + nu) * (1.0 - 2.0 * nu));

    mat3 R = extract_rotation_polar(F);
    float J = mat3_determinant(F);

    mat3 Ft = mat3_transpose(F);
    mat3 tau = 2.0 * mu * (F - R) * Ft + lam * J * (J - 1.0) * mat3(1.0);

    // ── 对称化（与 PhysDreamer mpm_utils.py:665 完全一致）──
    // 非对称 Kirchhoff 应力配 ∇w 会做非保守功 → 持续注入能量 →
    // 拖拽后 max_vel 不衰减（0.03~1.39 振荡 2 分钟）、速度被放大 10 倍。
    // 根因：本实现 R 用 Gram-Schmidt 近似（非真 SVD 极分解），大变形时
    // (F-R)*F^T 产生非对称分量。对称化强制应力为弹性势能的真实梯度，
    // 消除注入源。SVD R 下 (F-R)*F^T 本就对称，对称化是 no-op 安全网。
    tau = (tau + mat3_transpose(tau)) * 0.5;
    return tau;
}

/**
 * 简化塑性修正 (von Mises 屈服准则)
 */
mat3 compute_plastic_stress(mat3 trial_stress, float yield_stress) {
    float von_mises = compute_von_mises_stress(trial_stress);

    if (von_mises <= yield_stress) {
        return trial_stress;
    }

    float scale = yield_stress / von_mises;
    mat3 deviatoric = compute_deviatoric(trial_stress);
    float mean_stress = mat3_trace(trial_stress) / 3.0;

    return mat3(mean_stress) + deviatoric * scale;
}

/**
 * 主应力计算函数 (根据材料类型选择)
 */
mat3 compute_stress(mat3 F, float E, float nu, uint material_id, float yield_stress) {
    mat3 trial_stress = compute_fcr_stress(F, E, nu);

    if (material_id == MATERIAL_METAL) {
        return compute_plastic_stress(trial_stress, yield_stress);
    } else {
        return trial_stress;
    }
}

#endif // MPM_STRESS_GLSL
