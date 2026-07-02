#ifndef VULKANSPLATTING_H
#define VULKANSPLATTING_H

#include <optional>
#include <string>
#include <memory>

class Window;
class Renderer;

class VulkanSplatting {
public:
    struct RendererConfiguration {
        bool enableVulkanValidationLayers = false;
        std::optional<uint8_t> physicalDeviceId = std::nullopt;
        bool immediateSwapchain = false;
        std::string scene;

        float fov = 45.0f;
        float near = 0.2f;
        float far = 1000.0f;
        bool enableGui = false;

        // MPM 物理仿真子步数（命令行 --substeps 覆盖，默认 256 = PhysDreamer 最小稳定值）
        uint32_t substeps = 256;

        // Benchmark模式配置
        bool benchmarkMode = false;           // 启用benchmark模式
        int benchmarkFrames = 300;            // benchmark运行帧数
        int benchmarkWarmup = 60;             // warmup帧数
        std::string benchmarkOutput = "benchmark_results.json";  // 输出文件

        std::shared_ptr<Window> window;
    };

    explicit VulkanSplatting(RendererConfiguration configuration) : configuration(configuration) {}

#ifdef VKGS_ENABLE_GLFW
    static std::shared_ptr<Window> createGlfwWindow(std::string name, int width, int height);
#endif

#ifdef VKGS_ENABLE_METAL
    static std::shared_ptr<Window> createMetalWindow(void *caMetalLayer, int width, int height);
#endif

    void start();

    void initialize();

    void draw();

    void logTranslation(float x, float y);

    void logMovement(float x, float y, float z);

    void loadCamera(const std::string& cameraPath);

    void stop();
private:
    RendererConfiguration configuration;
    std::shared_ptr<Renderer> renderer;
};

#endif //VULKANSPLATTING_H
