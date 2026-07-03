#include <filesystem>
#include <iostream>
#include <libenvpp/env.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
// windows.h 把 near/far 定义为空宏，会破坏 3dgs.h 里 `float near/far` 字段名
#undef near
#undef far
#endif

#include "3dgs.h"
#include "args.hxx"
#include "spdlog/spdlog.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    // 控制台切 UTF-8：源码字符串字面量为 UTF-8，默认控制台 CP(GBK/936) 会把中文日志打成乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    spdlog::set_pattern("[%H:%M:%S] [%^%L%$] %v");

    args::ArgumentParser parser("Vulkan Splatting");
    args::HelpFlag helpFlag{parser, "help", "Display this help menu", {'h', "help"}};
    args::Flag validationLayersFlag{
        parser, "validation-layers", "Enable Vulkan validation layers", {"validation"}
    };
    args::Flag verboseFlag{parser, "verbose", "Enable verbose logging", {'v', "verbose"}};
    args::ValueFlag<uint32_t> physicalDeviceIdFlag{
        parser, "physical-device", "Select physical device by index", {'d', "device"}
    };
    args::Flag immediateSwapchainFlag{
        parser, "immediate-swapchain", "Set swapchain mode to immediate (VK_PRESENT_MODE_IMMEDIATE_KHR)",
        {'i', "immediate-swapchain"}
    };
    args::ValueFlag<uint32_t> widthFlag{parser, "width", "Set window width", {'w', "width"}};
    args::ValueFlag<uint32_t> heightFlag{parser, "height", "Set window height", {'h', "height"}};
    args::Flag noGuiFlag{parser, "no-gui", "Disable GUI", { "no-gui"}};
    args::ValueFlag<uint32_t> substepsFlag{
        parser, "substeps", "MPM physics substeps per frame (default 256, PhysDreamer min stable)", {"substeps"}
    };
    args::ValueFlag<std::string> cameraFlag{parser, "camera", "Path to camera file for initial view", {"camera"}};
    args::Positional<std::string> scenePath{parser, "scene", "Path to scene file", "scene.ply"};

    try {
        parser.ParseCLI(argc, argv);
    } catch (const args::Completion& e) {
        std::cout << e.what();
        return 0;
    }
    catch (const args::Help&) {
        std::cout << parser;
        return 0;
    }
    catch (const args::ParseError& e) {
        std::cout << e.what() << std::endl;
        std::cout << parser;
        return 1;
    }

    auto pre = env::prefix("VKGS");
    auto validationLayers = pre.register_variable<bool>("VALIDATION_LAYERS");
    auto physicalDeviceId = pre.register_variable<uint8_t>("PHYSICAL_DEVICE");
    auto immediateSwapchain = pre.register_variable<bool>("IMMEDIATE_SWAPCHAIN");
    auto envVars = pre.parse_and_validate();

    // 默认 warn 级别：仅 warn/error/critical，关闭全部 info/debug 噪声（含实时渲染与回读诊断）。
    // --verbose 开启 debug 级别，并触发 Diagnose() 回读与指标计算（max_tau/ratio/stretch 等）。
    const bool verbose = args::get(verboseFlag);
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::warn);

    VulkanSplatting::RendererConfiguration config{
        envVars.get_or(validationLayers, false),
        envVars.get(physicalDeviceId).has_value()
            ? std::make_optional(envVars.get(physicalDeviceId).value())
            : std::nullopt,
        envVars.get_or(immediateSwapchain, false),
        args::get(scenePath)
    };

    // check that the scene file exists
    if (!std::filesystem::exists(config.scene)) {
        spdlog::critical("File does not exist: {}", config.scene);
        return 0;
    }

    if (validationLayersFlag) {
        config.enableVulkanValidationLayers = args::get(validationLayersFlag);
    }

    if (physicalDeviceIdFlag) {
        config.physicalDeviceId = std::make_optional<uint8_t>(static_cast<uint8_t>(args::get(physicalDeviceIdFlag)));
    }

    if (immediateSwapchainFlag) {
        config.immediateSwapchain = args::get(immediateSwapchainFlag);
    }

    if (noGuiFlag) {
        config.enableGui = false;
    } else {
        config.enableGui = true;
    }

    if (substepsFlag) {
        config.substeps = args::get(substepsFlag);
    }

    config.verbose = verbose;

    auto width = widthFlag ? args::get(widthFlag) : 1280;
    auto height = heightFlag ? args::get(heightFlag) : 720;

    config.window = VulkanSplatting::createGlfwWindow("Vulkan Splatting", width, height);

#ifndef DEBUG
    try {
#endif
    auto renderer = VulkanSplatting(config);
    renderer.initialize();

    // Load camera if specified
    if (cameraFlag) {
        auto cameraPath = args::get(cameraFlag);
        if (std::filesystem::exists(cameraPath)) {
            spdlog::info("Loading camera from: {}", cameraPath);
            renderer.loadCamera(cameraPath);
        } else {
            spdlog::warn("Camera file not found: {}", cameraPath);
        }
    }

    renderer.start();
#ifndef DEBUG
    } catch (const std::exception& e) {
        spdlog::critical(e.what());
        std::cout << e.what() << std::endl;
        return 0;
    }
#endif
    return 0;
}
