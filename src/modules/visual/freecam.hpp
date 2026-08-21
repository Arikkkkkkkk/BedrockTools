#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>

#include <chrono>

class FreecamModule final : public Module {
public:
    FreecamModule();
    ~FreecamModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onKeybindEvent(const std::string& key, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float flightSpeed = 10.0f;
    bool  throughWalls = true;

    int forwardKeybind = 0;
    int backKeybind = 0;
    int leftKeybind = 0;
    int rightKeybind = 0;
    int upKeybind = 0;
    int downKeybind = 0;

    bool movingForward = false;
    bool movingBack = false;
    bool movingLeft = false;
    bool movingRight = false;
    bool movingUp = false;
    bool movingDown = false;

    bedrocktools::sdk::Vec3 freeCamPos{};
    bool positionCaptured = false;

private:
    bool m_renderLevelHooked = false;
    std::chrono::steady_clock::time_point m_lastFrameTime{};
};
