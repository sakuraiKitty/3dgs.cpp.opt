#ifndef MPM_SCENE_PHYSICS_PROFILE_H
#define MPM_SCENE_PHYSICS_PROFILE_H

#include <string>
#include <glm/glm.hpp>
#include <cstdint>
#include <cctype>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace MPM {

/**
 * 各场景物理参数 preset
 *
 * 数值取自 PhysDreamer: projects/inference/configs/<scene>.py 的 simulate_cfg
 *   - init_young (E)        : Young's modulus [Pa]
 *   - substep                : 离线子步数
 *   - downsample_scale      : KMeans 降采样比例
 *   - grid_size             : MPM 网格分辨率（四场景均为 64）
 *
 * 说明：
 *   - nu（Poisson 比）：PhysDreamer demo.py:190 用 np.random.uniform(0.1, 0.4) 每次随机，
 *     这里固定 0.3（与既有 Vulkan 默认一致，便于复现）。
 *   - gravity：PhysDreamer 这四个场景 simulate_cfg 均未设重力
 *     （花/帽/电话由冻结边界支撑，处于静止平衡，变形只来自交互力），统一 {0,0,0}。
 *   - substeps（实时）：PhysDreamer gui_demo.py:532 默认 256，注释明言 "<128 会指数爆炸→NaN"。
 *     carnation 离线 768，实时取 256（gui_demo 默认值，稳定余量，CFL: sub_dt=(1/30)/256≈1.3e-4）。
 *       carnation : 256  (gui_demo 默认；原 128 低于稳定阈致 F 发散→刚体旋转锁死)
 *       hat       :  64  (E 软 21×, 波速 c_p≈√(E/ρ) 低, 子步可更少)
 *       alocasia  : 128
 *       telephone :  64
 */
struct ScenePhysicsProfile {
    std::string  scene_name;          // 匹配到的场景名（fallback="default"）
    float        E;                   // Young's modulus [Pa]
    float        nu;                  // Poisson's ratio
    float        density;             // [kg/m³]
    float        downsample_scale;    // KMeans 降采样比例
    int          grid_size;           // MPM 网格分辨率
    uint32_t     substeps;            // 实时子步数（离线折中）
    glm::vec3    gravity;             // 重力 [m/s²]

    // 默认 = carnation 参数（最保守 fallback，未知场景沿用）
    ScenePhysicsProfile()
        : scene_name("default"),
          E(2140628.25f), nu(0.3f), density(2000.0f),
          downsample_scale(0.1f), grid_size(64),
          substeps(256u), gravity(0.0f) {}
};

/**
 * 从场景目录路径推断物理参数 preset
 *
 * @param scene_path PLY 所在目录（SceneDescriptor::scene_path，由 SceneLoader::CreateDescriptor 设置为
 *                   point_cloud.ply 的父目录）
 * @return 匹配到的 ScenePhysicsProfile；未命中返回默认(carnation) profile
 *
 * 匹配规则：取路径最后一段目录名，小写化后按子串匹配 carnation/hat/alocasia/telephone。
 * 例：D:\\...\\data\\physics_dreamer\\hat → "hat" → hat preset
 */
inline ScenePhysicsProfile GetScenePhysicsProfile(const std::string& scene_path) {
    // 提取最后一段目录名
    std::string leaf;
    const size_t pos = scene_path.find_last_of("/\\");
    leaf = (pos == std::string::npos) ? scene_path : scene_path.substr(pos + 1);

    // 小写化
    std::string key;
    key.reserve(leaf.size());
    std::transform(leaf.begin(), leaf.end(), std::back_inserter(key),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    ScenePhysicsProfile p;  // 默认 carnation
    if (key.find("carnation") != std::string::npos) {
        p.scene_name = "carnations";
        p.E = 2140628.25f; p.nu = 0.3f; p.density = 2000.0f;
        p.downsample_scale = 0.1f;  p.grid_size = 64;
        p.substeps = 256u;          p.gravity = {0.0f, 0.0f, 0.0f};  // carnation: PD gui_demo 默认 256（<128 爆炸）
    } else if (key.find("hat") != std::string::npos) {
        p.scene_name = "hat";
        p.E = 1.0e5f;      p.nu = 0.3f; p.density = 2000.0f;
        p.downsample_scale = 0.04f; p.grid_size = 64;
        p.substeps = 64u;           p.gravity = {0.0f, 0.0f, 0.0f};
    } else if (key.find("alocasia") != std::string::npos) {
        p.scene_name = "alocasia";
        p.E = 1.0e6f;      p.nu = 0.3f; p.density = 2000.0f;
        p.downsample_scale = 0.1f;  p.grid_size = 64;
        p.substeps = 128u;          p.gravity = {0.0f, 0.0f, 0.0f};
    } else if (key.find("telephone") != std::string::npos) {
        p.scene_name = "telephone";
        p.E = 1.0e5f;      p.nu = 0.3f; p.density = 2000.0f;
        p.downsample_scale = 0.1f;  p.grid_size = 64;
        p.substeps = 64u;           p.gravity = {0.0f, 0.0f, 0.0f};
    } else {
        p.scene_name = "default(carnation)";
        spdlog::warn("[ScenePhysicsProfile] scene '{}' not recognized, falling back to carnation preset "
                     "(E={}, substeps={}, downsample={})",
                     leaf, p.E, p.substeps, p.downsample_scale);
    }
    return p;
}

} // namespace MPM

#endif // MPM_SCENE_PHYSICS_PROFILE_H
