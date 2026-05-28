#ifndef GUIMANAGER_H
#define GUIMANAGER_H
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

class GUIManager {
public:
    struct CameraInfo {
        glm::vec3 position;
        glm::quat rotation;
        float fov;
        float nearPlane;
        float farPlane;
    };

    GUIManager();

    static void init();

    void buildGui();

    static void pushTextMetric(const std::string& name, float value);

    static void pushMetric(const std::string& name, float value);

    static void pushMetric(const std::unordered_map<std::string, float>& name);

    static bool wantCaptureMouse();

    static bool wantCaptureKeyboard();

    bool mouseCapture = false;

    CameraInfo cameraInfo {};

    bool saveCameraRequested = false;

};

#endif //GUIMANAGER_H
