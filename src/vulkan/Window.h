#ifndef WINDOW_H
#define WINDOW_H
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>

#include "VulkanContext.h"

class Window {
public:
    virtual VkSurfaceKHR createSurface(std::shared_ptr<VulkanContext> context) = 0;

    virtual std::array<bool, 3> getMouseButton() { return {false, false, false}; }

    virtual std::vector<std::string> getRequiredInstanceExtensions() = 0;

    [[nodiscard]] virtual std::pair<uint32_t, uint32_t> getFramebufferSize() const = 0;

    virtual std::array<double, 2> getCursorTranslation() { return {0, 0}; }

    virtual std::array<double, 2> getCursorPosition() { return {0, 0}; }

    // 返回自上次调用以来累积的滚轮偏移量并清零（x: 水平, y: 垂直，通常只用到 y）
    virtual std::array<double, 2> getScrollOffset() { return {0, 0}; }

    // [0]=W [1]=A [2]=S [3]=D [4]=SPACE [5]=LSHIFT [6]=ESC [7]=F12 [8]=P [9]=Q [10]=E
    virtual std::array<bool, 11> getKeys() { return {false, false, false, false, false, false, false, false, false, false, false}; }

    virtual void mouseCapture(bool capture) { }

    /**
     * 设置光标样式
     * @param cursorType 0=默认, 1=绿色(可变形区域), 2=红色(非可变形区域)
     */
    virtual void setCursor(int cursorType) { }

    virtual bool tick() { return false; };

    virtual void logTranslation(float x, float y) { };

    virtual void logMovement(float x, float y) { };

    virtual ~Window() = default;

};



#endif //WINDOW_H
