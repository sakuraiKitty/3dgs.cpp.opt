#ifndef MPM_STRUCTS_GLSL
#define MPM_STRUCTS_GLSL

// MPM粒子数据结构 (160 bytes, 与C++端一致)
struct ParticleData {
    vec3 position;              // 0-12   - 位置 [m]
    float mass;                  // 12-16  - 质量 [kg]
    vec3 velocity;              // 16-28  - 速度 [m/s]
    uint freeze_flag;            // 28-32  - 冻结标志 (0=可动, 1=冻结)
    mat3 deformation_gradient;  // 32-80  - 变形梯度 F (3x3矩阵)
    float volume;               // 80-84  - 体积 [m^3]
    uint material_id;           // 84-88  - 材料ID
    float youngs_modulus;       // 88-92  - 杨氏模量 E [Pa]
    float poisson_ratio;        // 92-96  - 泊松比 nu
    float density;              // 96-100 - 密度 [kg/m^3]

    // 新增字段（用于高级物理特性）
    mat3 apic_matrix;           // 100-148 - APIC动量矩阵C（或临时存储应力）
    uint is_filled_point;       // 148-152 - 是否为内部填充点 (0=原始高斯, 1=填充点)
    float padding;              // 152-156 - 对齐填充

    // 总计156字节 (需要对齐到160字节)
};

// MPM网格节点数据结构 (32 bytes, 与C++端一致)
struct GridNode {
    vec3 velocity;      // 0-12   - 速度 [m/s]
    float mass;          // 12-16  - 累积质量 [kg]
    vec3 force;         // 16-28  - 累积力 [N]
    uint active_count;  // 28-32  - 活跃粒子计数
};

// 材料类型常量
const uint MATERIAL_JELLY = 0;
const uint MATERIAL_METAL = 1;
const uint MATERIAL_SAND = 2;
const uint MATERIAL_FOAM = 3;
const uint MATERIAL_SNOW = 4;
const uint MATERIAL_PLASTICINE = 5;
const uint MATERIAL_NEO_HOOKEAN = 6;

#endif // MPM_STRUCTS_GLSL
