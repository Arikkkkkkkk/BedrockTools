#pragma once

#include "../Module.hpp"

class BetterHandheldsModule final : public Module {
public:
    BetterHandheldsModule();
    ~BetterHandheldsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool isThirdPerson() const;

    float mainHandScale = 1.0f;
    float offHandScale = 1.0f;
    float mainHandPosX = 0.0f;
    float mainHandPosY = 0.0f;
    float mainHandPosZ = 0.0f;
    float offHandPosX = 0.0f;
    float offHandPosY = 0.0f;
    float offHandPosZ = 0.0f;
    bool  applyThirdPerson = false;
    bool  thirdPerson = false;

private:
    bool m_renderItemHooked = false;
    bool m_perspectiveHooked = false;
};
