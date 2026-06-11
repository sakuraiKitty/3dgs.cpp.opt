#ifndef GLFWWINDOW_H
#define GLFWWINDOW_H

#include "../Window.h"
#include <array>

class GLFWWindow final : public Window {
public:
    GLFWWindow(std::string name, int width, int height);

    VkSurfaceKHR createSurface(std::shared_ptr<VulkanContext> context) override;

    std::array<bool, 3> getMouseButton() override;

    std::vector<std::string> getRequiredInstanceExtensions() override;

    [[nodiscard]] std::pair<uint32_t, uint32_t> getFramebufferSize() const override;

    std::array<double, 2> getCursorTranslation() override;

    std::array<double, 2> getCursorPosition() override;

    std::array<bool, 9> getKeys() override;

    void mouseCapture(bool capture) override;

    bool tick() override;

    /**
     * 设置光标样式
     * @param cursorType 0=默认, 1=绿色(可变形区域), 2=红色(非可变形区域)
     */
    void setCursor(int cursorType);

    void* window;

private:
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    double lastX = 0.0;
    double lastY = 0.0;

    // 自定义光标
    void* defaultCursor = nullptr;
    void* greenCursor = nullptr;
    void* redCursor = nullptr;

    /**
     * 创建彩色光标
     * @param r 红色分量 (0-255)
     * @param g 绿色分量 (0-255)
     * @param b 蓝色分量 (0-255)
     */
    void* createColorCursor(int r, int g, int b);
};


#endif //GLFWWINDOW_H
