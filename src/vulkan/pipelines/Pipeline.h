#ifndef VULKAN_SPLATTING_PIPELINE_H
#define VULKAN_SPLATTING_PIPELINE_H


#include <memory>
#include <map>
#include "../VulkanContext.h"
#include "../DescriptorSet.h"

class Pipeline {
public:
    struct DescriptorOption {
        bool multiple;
        uint32_t value;
        std::vector<uint32_t> values;

        DescriptorOption(uint32_t value) : multiple(false), value(value) {}

        DescriptorOption(std::vector<uint32_t> values) : multiple(true), values(std::move(values)) {}

        [[nodiscard]] uint32_t get(size_t index) const;
    };

    explicit Pipeline(const std::shared_ptr<VulkanContext>& context);

    Pipeline(const Pipeline &) = delete;

    Pipeline(Pipeline &&) = delete;

    Pipeline &operator=(const Pipeline &) = delete;

    Pipeline &operator=(Pipeline &&) = delete;

    void addDescriptorSet(uint32_t set, std::shared_ptr<DescriptorSet> descriptorSet);

    void addPushConstant(vk::ShaderStageFlags stageFlags, uint32_t offset, uint32_t size);

    void addDescriptorSetLayoutBinding(const vk::DescriptorSetLayoutBinding& binding);

    virtual void build() = 0;

    /**
     * Rebuild pipeline layout using actual descriptor set layouts.
     * Called after descriptor sets are built (which happens after buffers are created).
     * This replaces the TEMP layout used during initial build() with the real layouts.
     */
    virtual void rebuild() { rebuildPipelineLayout(); }

    // C++ style API (for vk::UniqueCommandBuffer)
    virtual void bind(const vk::UniqueCommandBuffer &commandBuffer, uint8_t currentFrame, DescriptorOption option);

    // C style API (for VkCommandBuffer - raw handle)
    virtual void bind(VkCommandBuffer commandBuffer, uint8_t currentFrame, DescriptorOption option);

    vk::UniquePipelineLayout pipelineLayout;
    vk::UniquePipeline pipeline;
protected:
    void buildPipelineLayout();
    void rebuildPipelineLayout();  // Rebuild layout using actual descriptor set layouts (not TEMP)

    std::shared_ptr<VulkanContext> context;
    std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;
    std::vector<vk::PushConstantRange> pushConstantRanges;

    std::map<uint32_t, std::shared_ptr<DescriptorSet>> descriptorSets;
    vk::UniqueDescriptorSetLayout tempDescriptorSetLayout;  // Keep temp layout alive
};


#endif //VULKAN_SPLATTING_PIPELINE_H
