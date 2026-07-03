#ifndef VULKANCONTEXT_H
#define VULKANCONTEXT_H

#define FRAMES_IN_FLIGHT 3

#include <optional>
#include <set>
#include <unordered_map>
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_TYPESAFE_CONVERSION 1
#include <vulkan/vulkan.hpp>
#include "vk_mem_alloc.h"

// 时间线信号量封装类
class TimelineSemaphore {
public:
    TimelineSemaphore(vk::Device device, uint64_t initialValue = 0)
        : device(device), currentValue(initialValue) {
        // 创建时间线信号量类型信息
        vk::SemaphoreTypeCreateInfo timelineCreateInfo{};
        timelineCreateInfo.semaphoreType = vk::SemaphoreType::eTimeline;
        timelineCreateInfo.initialValue = initialValue;

        // 创建信号量信息，链接时间线信息
        vk::SemaphoreCreateInfo semaphoreCreateInfo{};
        semaphoreCreateInfo.pNext = &timelineCreateInfo;

        semaphore = device.createSemaphoreUnique(semaphoreCreateInfo);
    }

    ~TimelineSemaphore() {
        // UniqueSemaphore会自动清理，无需手动destroy
    }

    vk::Semaphore getHandle() const { return semaphore.get(); }
    uint64_t getCurrentValue() const { return currentValue; }

    // 等待信号量达到指定值
    void wait(uint64_t value, uint64_t timeout = UINT64_MAX) {
        vk::SemaphoreWaitInfo waitInfo{};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &semaphore.get();
        waitInfo.pValues = &value;
        device.waitSemaphores(waitInfo, timeout);
    }

    // 信号量值增加
    void signal(uint64_t value) {
        currentValue = value;
    }

private:
    vk::UniqueSemaphore semaphore;
    vk::Device device;
    uint64_t currentValue;
};

struct Image {
    vk::Image image;
    vk::UniqueImageView imageView;
    vk::Format format;
    vk::Extent2D extent;
    std::optional<vk::UniqueFramebuffer> framebuffer;

    Image(const vk::Image &image, vk::UniqueImageView &&image_view, vk::Format format,
          const vk::Extent2D &extent, std::optional<vk::UniqueFramebuffer> &&framebuffer = std::nullopt)
            : image(image),
              imageView(std::move(image_view)),
              format(format),
              extent(extent),
              framebuffer(std::move(framebuffer)) {
    }
};

class VulkanContext {
private:
    struct QueueFamilyIndices {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> computeFamily;
        std::optional<uint32_t> presentFamily;

        bool isComplete() {
            return graphicsFamily.has_value() && computeFamily.has_value() && presentFamily.has_value();
        }
    };

public:
    struct Queue {
        enum Type {
            GRAPHICS,
            COMPUTE,
            PRESENT
        };

        std::set<Type> types;
        uint32_t queueFamily;
        uint32_t queueIndex;
        vk::Queue queue;

        // 辅助compute队列（用于多队列并行）
        std::optional<vk::Queue> secondaryQueue = std::nullopt;
        uint32_t secondaryQueueIndex = UINT32_MAX;
    };

    VulkanContext(const std::vector<std::string> &instance_extensions,
                  const std::vector<std::string> &device_extensions, bool validation_layers_enabled);

    VulkanContext(const VulkanContext &) = delete;

    VulkanContext(VulkanContext &&) = delete;

    VulkanContext &operator=(const VulkanContext &) = delete;

    VulkanContext &operator=(VulkanContext &&) = delete;

    void createInstance();

    bool isDeviceSuitable(vk::PhysicalDevice device, std::optional<vk::SurfaceKHR> surface = std::nullopt);

    void selectPhysicalDevice(std::optional<uint8_t> id = std::nullopt,
                              std::optional<vk::SurfaceKHR> surface = std::nullopt);

    VulkanContext::QueueFamilyIndices findQueueFamilies();

    void createQueryPool();

    void createLogicalDevice(vk::PhysicalDeviceFeatures deviceFeatures, vk::PhysicalDeviceVulkan11Features deviceFeatures11, vk::PhysicalDeviceVulkan12Features deviceFeatures12);

    void createDescriptorPool(uint8_t framesInFlight);

    bool hasIndependentComputeQueue() const;

    vk::UniqueCommandBuffer beginOneTimeCommandBuffer();

    void endOneTimeCommandBuffer(vk::UniqueCommandBuffer &&commandBuffer, Queue::Type queue);

    virtual ~VulkanContext();

    vk::UniqueInstance instance;
    vk::PhysicalDevice physicalDevice;
    std::optional<vk::UniqueSurfaceKHR> surface;
    vk::UniqueDevice device;
    std::unordered_map<Queue::Type, Queue> queues;
    VmaAllocator allocator;

    vk::UniqueDescriptorPool descriptorPool;
    vk::UniqueQueryPool queryPool;
    // physics 专用 timestamp query pool（mpm/coupling GPU 时延）。
    // 与 render queryPool 完全隔离：仅由 physics cmd（COMPUTE 队列）写入，
    // 由 retrievePhysicsTimestamps 单独读取，不复用既有 retrieveTimestamps 的 eWait 路径，
    // 避免跨队列/跨 cmd 池混用导致 frame loop 死等。
    vk::UniqueQueryPool physicsQueryPool;

    bool validationLayersEnabled;
private:
    std::vector<std::string> instanceExtensions;
    std::vector<std::string> deviceExtensions;

    vk::UniqueCommandPool commandPool;

    void setupVma();

    void createCommandPool();
};


#endif //VULKANCONTEXT_H
