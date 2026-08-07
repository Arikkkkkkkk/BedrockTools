#pragma once

#include "../Module.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <utility>

class KeystrokesModule : public Module {
public:
    KeystrokesModule();
    ~KeystrokesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    bool onMouseEvent(int button, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool bW = false;
    bool bA = false;
    bool bS = false;
    bool bD = false;
    bool bSpace = false;
    bool bSneak = false;

    int m_size = 50;
    bool m_showJump = true;
    bool m_showSneak = true;
    bool m_roundKeys = true;
    uint32_t m_pressedColor = 0xFF00FF00;
    bool m_rainbow = false;
    float m_rainbowSpeed = 1.0f;
    float m_rainbowHue = 0.0f;

    struct KeyAnimState {
        float pressProgress = 0.0f;
    };

    KeyAnimState m_wState;
    KeyAnimState m_aState;
    KeyAnimState m_sState;
    KeyAnimState m_dState;
    KeyAnimState m_jumpState;
    KeyAnimState m_sneakState;
    KeyAnimState m_lmbState;
    KeyAnimState m_rmbState;

    float hudPosX = 20.0f;
    float hudPosY = 100.0f;
    bool isHudModule = true;

private:
    std::pair<int, int> getMouseCps();
    void clearMouseState();

    std::atomic_bool m_mouseActive{false};
    std::atomic_bool m_showMouseCps{true};
    std::atomic_bool m_lmbDown{false};
    std::atomic_bool m_rmbDown{false};
    std::deque<std::chrono::steady_clock::time_point> m_leftClicks;
    std::deque<std::chrono::steady_clock::time_point> m_rightClicks;
    std::mutex m_mouseMutex;
};
