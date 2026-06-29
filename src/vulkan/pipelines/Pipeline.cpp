#include "Pipeline.h"

#include <utility>
#include <spdlog/spdlog.h>

uint32_t Pipeline::DescriptorOption::get(size_t index) const {
    if (multiple) {
        return values[index];
    } else {
        return value;
    }
}

Pipeline::Pipeline(const std::shared_ptr<VulkanContext>& _context) : context(_context) {

}

void Pipeline::addDescriptorSet(uint32_t set, std::shared_ptr<DescriptorSet> descriptorSet) {
    descriptorSets[set] = std::move(descriptorSet);
}

void Pipeline::addDescriptorSetLayoutBinding(const vk::DescriptorSetLayoutBinding& binding) {
    descriptorSetLayoutBindings.push_back(binding);
}

void Pipeline::buildPipelineLayout() {
    spdlog::debug("[Pipeline] Building pipeline layout with {} descriptor sets", descriptorSets.size());
    spdlog::debug("[Pipeline] Descriptor set layout bindings: {}", descriptorSetLayoutBindings.size());

    std::vector<vk::DescriptorSetLayout> layouts;

    // 首先使用现有的descriptor set layouts（如果有的话）
    layouts.reserve(descriptorSets.size());
    for (auto &descriptorSet: descriptorSets) {
        layouts.push_back(descriptorSet.second->descriptorSetLayout.get());
    }

    // 如果没有descriptor sets但有descriptor set layout bindings，创建并保存临时layout
    if (layouts.empty() && !descriptorSetLayoutBindings.empty()) {
        spdlog::debug("[Pipeline] Creating temporary descriptor set layout from bindings");
        vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo(
            {}, descriptorSetLayoutBindings.size(), descriptorSetLayoutBindings.data()
        );
        tempDescriptorSetLayout = context->device->createDescriptorSetLayoutUnique(descriptorSetLayoutCreateInfo);
        layouts.push_back(tempDescriptorSetLayout.get());
    }

    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo({}, layouts.size(), layouts.data());
    if (!pushConstantRanges.empty()) {
        pipelineLayoutCreateInfo.setPushConstantRangeCount(pushConstantRanges.size());
        pipelineLayoutCreateInfo.setPPushConstantRanges(pushConstantRanges.data());
    }

    spdlog::debug("[Pipeline] Calling createPipelineLayoutUnique");
    pipelineLayout = context->device->createPipelineLayoutUnique(pipelineLayoutCreateInfo);
    spdlog::debug("[Pipeline] Pipeline layout created successfully");
}

void Pipeline::bind(const vk::UniqueCommandBuffer &commandBuffer, uint8_t currentFrame, DescriptorOption option) {
    commandBuffer->bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.get());

    std::vector<vk::DescriptorSet> descriptorSetsToBind;
    descriptorSetsToBind.reserve(descriptorSets.size());
    auto ind = 0;
    for (auto &descriptorSet: descriptorSets) {
        descriptorSetsToBind.push_back(descriptorSet.second->getDescriptorSet(currentFrame, option.get(ind++)));
    }

    commandBuffer->bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipelineLayout.get(), 0, descriptorSetsToBind,
                                      nullptr);
}

// C style API overload for VkCommandBuffer (raw handle)
void Pipeline::bind(VkCommandBuffer commandBuffer, uint8_t currentFrame, DescriptorOption option) {
    // Use C-style Vulkan API calls
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    VkPipeline vkPipeline = pipeline.get();
    VkPipelineLayout vkLayout = pipelineLayout.get();

    // Bind pipeline
    vkCmdBindPipeline(commandBuffer, bindPoint, vkPipeline);

    // Collect descriptor sets
    std::vector<VkDescriptorSet> descriptorSetsToBind;
    descriptorSetsToBind.reserve(descriptorSets.size());
    auto ind = 0;
    for (auto &descriptorSet: descriptorSets) {
        descriptorSetsToBind.push_back(
            descriptorSet.second->getDescriptorSet(currentFrame, option.get(ind++))
        );
    }

    // Bind descriptor sets
    if (!descriptorSetsToBind.empty()) {
        vkCmdBindDescriptorSets(commandBuffer, bindPoint, vkLayout, 0,
                               static_cast<uint32_t>(descriptorSetsToBind.size()),
                               descriptorSetsToBind.data(), 0, nullptr);
    }
}

void Pipeline::rebuildPipelineLayout() {
    spdlog::info("[Pipeline] Rebuilding pipeline layout with {} actual descriptor sets (replacing TEMP layout)",
                descriptorSets.size());

    // Destroy TEMP layout (no longer needed — actual descriptor set layouts exist now)
    tempDescriptorSetLayout.reset();

    // Destroy old pipeline layout (was built with TEMP layout)
    pipelineLayout.reset();

    // Build new pipeline layout using ACTUAL descriptor set layouts
    std::vector<vk::DescriptorSetLayout> layouts;
    layouts.reserve(descriptorSets.size());
    for (auto &descriptorSet: descriptorSets) {
        layouts.push_back(descriptorSet.second->descriptorSetLayout.get());
    }

    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo({}, layouts.size(), layouts.data());
    if (!pushConstantRanges.empty()) {
        pipelineLayoutCreateInfo.setPushConstantRangeCount(pushConstantRanges.size());
        pipelineLayoutCreateInfo.setPPushConstantRanges(pushConstantRanges.data());
    }

    pipelineLayout = context->device->createPipelineLayoutUnique(pipelineLayoutCreateInfo);
    spdlog::info("[Pipeline] Pipeline layout rebuilt successfully with actual descriptor set layouts");
}

void Pipeline::addPushConstant(vk::ShaderStageFlags stageFlags, uint32_t offset, uint32_t size) {
    pushConstantRanges.emplace_back(stageFlags, offset, size);
}
