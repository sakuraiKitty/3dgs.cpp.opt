#ifndef MPM_BSPLINE_GLSL
#define MPM_BSPLINE_GLSL

/**
 * Quadratic B-spline weights for MPM (PhysDreamer/Taichi convention)
 *
 * Convention: base_node = floor(grid_pos - 0.5), fx = grid_pos - base ∈ [0.5, 1.5]
 * Stencil: 3×3×3 nodes at offsets {-1, 0, +1} from base
 *
 * Per-dimension weight formulas (quadratic B-spline, support [-1.5, 1.5]):
 *   w[0] = 0.5 * (1.5 - fx)^2    (node at base-1, offset=-1)
 *   w[1] = 0.75 - (fx - 1.0)^2   (node at base,   offset=0)
 *   w[2] = 0.5 * (fx - 0.5)^2    (node at base+1, offset=+1)
 *
 * Per-dimension derivative formulas:
 *   dw[0] = fx - 1.5              (d/d(fx) of 0.5*(1.5-fx)^2)
 *   dw[1] = -2.0 * (fx - 1.0)    (d/d(fx) of 0.75-(fx-1)^2)
 *   dw[2] = fx - 0.5              (d/d(fx) of 0.5*(fx-0.5)^2)
 *
 * Properties:
 *   - Partition of unity: w[0]+w[1]+w[2] = 1.0 for ALL fx ∈ [0.5, 1.5]
 *   - Support [-1.5, 1.5]: 3 nodes per dimension (NOT 4 like cubic B-spline)
 *   - Continuous at boundaries (NO discontinuity, unlike the old bspline_cubic)
 *   - Peak value: w[1] = 0.75 at fx=1.0 (center node dominates)
 *
 * Previous bug (ROOT CAUSE of MPM freeze):
 *   Old bspline_cubic used support [-2, 2] with:
 *     |x|<1:  0.5*(1-|x|)^2  (peak 0.5, not 0.75)
 *     1≤|x|<2: 0.5*(1.5-|x|)^2
 *   This kernel:
 *   1) Requires 4-node stencil but we used 3-node → missing node
 *   2) Has discontinuity at x=1 (0→0.125 jump)
 *   3) Gives wrong weights: for fx=1.0, w=[0,0.125,0.5] sum=0.625 ≠ 1.0
 *   4) Node at offset fx+1≥1.5 always gets zero weight
 *   → P2G transfers near-zero mass → Grid has no velocity → particles frozen
 *
 * Corresponds: mpm_utils.py p2g_apic_with_stress() line ~372
 */

struct BSplineWeights {
    // 一维权重 (wx[0], wx[1], wx[2])
    float wx[3];
    float wy[3];
    float wz[3];

    // 一维导数 (d/d(fx) of each weight)
    float dwx[3];
    float dwy[3];
    float dwz[3];

    // 3x3x3 权重张量 w[i][j][k] = wx[i] * wy[j] * wz[k]
    float w[3][3][3];

    // 3x3x3 梯度权重张量
    vec3 dw[3][3][3];
};

/**
 * Compute 3×3×3 quadratic B-spline weights (PhysDreamer convention)
 *
 * @param fx  Fractional position: grid_pos - base_node, range [0.5, 1.5]
 *             With base_node = floor(grid_pos - 0.5)
 * @param out_weights  Output: weights and derivatives
 */
void compute_bspine_weights_3x3x3(vec3 fx, out BSplineWeights out_weights) {
    // ── Per-dimension weights (PhysDreamer quadratic B-spline) ──
    // w[0] = 0.5 * (1.5 - fx)^2    (node at base-1)
    // w[1] = 0.75 - (fx - 1.0)^2   (node at base)
    // w[2] = 0.5 * (fx - 0.5)^2    (node at base+1)

    // x dimension
    out_weights.wx[0] = 0.5 * (1.5 - fx.x) * (1.5 - fx.x);
    out_weights.wx[1] = 0.75 - (fx.x - 1.0) * (fx.x - 1.0);
    out_weights.wx[2] = 0.5 * (fx.x - 0.5) * (fx.x - 0.5);

    // y dimension
    out_weights.wy[0] = 0.5 * (1.5 - fx.y) * (1.5 - fx.y);
    out_weights.wy[1] = 0.75 - (fx.y - 1.0) * (fx.y - 1.0);
    out_weights.wy[2] = 0.5 * (fx.y - 0.5) * (fx.y - 0.5);

    // z dimension
    out_weights.wz[0] = 0.5 * (1.5 - fx.z) * (1.5 - fx.z);
    out_weights.wz[1] = 0.75 - (fx.z - 1.0) * (fx.z - 1.0);
    out_weights.wz[2] = 0.5 * (fx.z - 0.5) * (fx.z - 0.5);

    // ── Per-dimension derivatives ──
    // dw[0] = fx - 1.5              (derivative of 0.5*(1.5-fx)^2)
    // dw[1] = -2.0 * (fx - 1.0)    (derivative of 0.75-(fx-1)^2)
    // dw[2] = fx - 0.5              (derivative of 0.5*(fx-0.5)^2)

    out_weights.dwx[0] = fx.x - 1.5;
    out_weights.dwx[1] = -2.0 * (fx.x - 1.0);
    out_weights.dwx[2] = fx.x - 0.5;

    out_weights.dwy[0] = fx.y - 1.5;
    out_weights.dwy[1] = -2.0 * (fx.y - 1.0);
    out_weights.dwy[2] = fx.y - 0.5;

    out_weights.dwz[0] = fx.z - 1.5;
    out_weights.dwz[1] = -2.0 * (fx.z - 1.0);
    out_weights.dwz[2] = fx.z - 0.5;

    // ── Build 3×3×3 weight tensor ──
    // w[i][j][k] = wx[i] * wy[j] * wz[k]
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 3; k++) {
                out_weights.w[i][j][k] = out_weights.wx[i] * out_weights.wy[j] * out_weights.wz[k];
            }
        }
    }

    // ── Build 3×3×3 gradient weight tensor ──
    // ∇w[i][j][k] / dx = (dwx[i]*wy[j]*wz[k], wx[i]*dwy[j]*wz[k], wx[i]*wy[j]*dwz[k]) / dx
    // The /dx (= *inv_dx) is applied in compute_dweight()
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
 * Compute interpolation weight gradient (for stress force computation)
 * Corresponds: mpm_utils.py compute_dweight()
 *
 * @param weights  Pre-computed BSplineWeights structure
 * @param i x dimension index [0-2]
 * @param j y dimension index [0-2]
 * @param k z dimension index [0-2]
 * @param inv_dx Grid spacing reciprocal (1/dx)
 * @return Gradient vector ∇w_ijk (in world-space units)
 */
vec3 compute_dweight(
    BSplineWeights weights,
    int i, int j, int k,
    float inv_dx
) {
    // ∇w[i][j][k] * inv_dx — converts grid-space gradient to world-space
    return weights.dw[i][j][k] * inv_dx;
}

/**
 * Verify quadratic B-spline weight normalization
 * For debugging: sum of weights should be exactly 1.0
 *
 * @param weights  Weight structure
 * @return Sum of all 27 weights (should be ≈1.0)
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
