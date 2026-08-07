#include "breakindicator.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <cmath>

static void indicatorHSVtoRGB(float h, float s, float v, float& out_r, float& out_g, float& out_b) {
    if (s == 0.0f) { out_r = out_g = out_b = v; return; }
    h = std::fmod(h, 1.0f) * 6.0f;
    int i = (int)std::floor(h);
    float f = h - (float)i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (i) {
        case 0: out_r = v; out_g = t; out_b = p; break;
        case 1: out_r = q; out_g = v; out_b = p; break;
        case 2: out_r = p; out_g = v; out_b = t; break;
        case 3: out_r = p; out_g = q; out_b = v; break;
        case 4: out_r = t; out_g = p; out_b = v; break;
        default: out_r = v; out_g = p; out_b = q; break;
    }
}

static float (*_getDestroyProgress_orig)(void* _this, void* block);
static BreakIndicatorModule* g_breakIndicatorMod = nullptr;

static float _getDestroyProgress_hook(void* _this, void* block) {
    float increment = 0.0f;
    if (_getDestroyProgress_orig) {
        increment = _getDestroyProgress_orig(_this, block);
    }
    
    if (g_breakIndicatorMod && g_breakIndicatorMod->enabled) {
        auto now = std::chrono::steady_clock::now();
        
        if (g_breakIndicatorMod->m_lastBlock == nullptr) {
            g_breakIndicatorMod->m_lastUpdate = now;
        }
        
        float elapsed_ms = std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(now - g_breakIndicatorMod->m_lastUpdate).count();
        
        
        if (elapsed_ms > 150.0f || g_breakIndicatorMod->m_lastBlock != block || g_breakIndicatorMod->m_progress >= 1.0f) {
            g_breakIndicatorMod->m_progress = 0.0f;
            elapsed_ms = 0.0f; 
        }
        
        
        
        
        if (increment > 0.0f) {
            g_breakIndicatorMod->m_progress += elapsed_ms * (increment / 50.0f);
        }
        
        if (g_breakIndicatorMod->m_progress > 1.0f) {
            g_breakIndicatorMod->m_progress = 1.0f;
        }
        
        g_breakIndicatorMod->m_lastBlock = block;
        g_breakIndicatorMod->m_lastUpdate = now;
    }
    
    return increment;
}

BreakIndicatorModule::BreakIndicatorModule() 
    : Module("Break Indicator", "Displays a progress bar when breaking blocks.") {
    m_patched = false;
    m_patchTarget = nullptr;
    g_breakIndicatorMod = this;
}

BreakIndicatorModule::~BreakIndicatorModule() {
    if (g_breakIndicatorMod == this) g_breakIndicatorMod = nullptr;
}

void BreakIndicatorModule::onInit() {
    if (m_patchTarget) return;
    uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GetDestroyProgress);
    if (addr != 0) {
        m_patchTarget = (void*)addr;
    }
}

void BreakIndicatorModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, (void*)_getDestroyProgress_hook, (void**)&_getDestroyProgress_orig);
    m_patched = true;
}

void BreakIndicatorModule::onEnable() {
    applyPatch();
}

void BreakIndicatorModule::onDisable() {
}

void BreakIndicatorModule::onFrame() {
    if (!enabled) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUpdate).count();
    
    float drawProgress = m_progress;

    
    if (elapsed > 150 || m_progress <= 0.0f) {
        if (!m_alwaysShow) {
            
            std::vector<PLModMenu_DrawCommand> cmds;
            submitDrawCommands(moduleId, cmds);
            return;
        } else {
            
            drawProgress = 0.0f;
        }
    }

    std::vector<PLModMenu_DrawCommand> cmds;

    
    if (m_outline) {
        PLModMenu_DrawCommand outlineCmd = {};
        outlineCmd.type = PL_DRAW_RECT_FILLED;
        outlineCmd.x = hudPosX - m_outlineThickness;
        outlineCmd.y = hudPosY - m_outlineThickness;
        outlineCmd.w = m_width + (m_outlineThickness * 2.0f);
        outlineCmd.h = m_height + (m_outlineThickness * 2.0f);
        outlineCmd.x3 = m_cornerRadius + m_outlineThickness;
        outlineCmd.color = (0xFF << 24) | (m_outlineColorHex & 0x00FFFFFF);
        cmds.push_back(outlineCmd);
    }

    
    if (m_background) {
        PLModMenu_DrawCommand bgCmd = {};
        bgCmd.type = PL_DRAW_RECT_FILLED;
        bgCmd.x = hudPosX; 
        bgCmd.y = hudPosY;
        bgCmd.w = m_width; 
        bgCmd.h = m_height;
        bgCmd.x3 = m_cornerRadius;
        bgCmd.color = ((int)(m_backgroundOpacity * 255.0f) << 24) | 0x000000;
        cmds.push_back(bgCmd);
    }

    
    uint32_t barColor = m_barColorHex;
    if (m_rainbow) {
        m_rainbowHue += 0.002f * m_rainbowSpeed;
        if (m_rainbowHue > 1.0f) m_rainbowHue -= 1.0f;
        float r, g, b;
        indicatorHSVtoRGB(m_rainbowHue, 1.0f, 1.0f, r, g, b);
        barColor = (0xFF << 24) | (((int)(r * 255)) << 16) | (((int)(g * 255)) << 8) | ((int)(b * 255));
    } else {
        barColor = (0xFF << 24) | (barColor & 0x00FFFFFF); 
    }

    
    if (drawProgress > 0.001f) {
        PLModMenu_DrawCommand barCmd = {};
        barCmd.type = PL_DRAW_RECT_FILLED;
        barCmd.x = hudPosX; 
        barCmd.y = hudPosY;
        barCmd.w = m_width * drawProgress; 
        barCmd.h = m_height;
        barCmd.x3 = m_cornerRadius;
        barCmd.color = barColor;
        cmds.push_back(barCmd);
    }

    
    std::string text = std::to_string((int)(drawProgress * 100)) + "%";
    PLModMenu_DrawCommand txtCmd = {};
    txtCmd.type = PL_DRAW_TEXT;
    txtCmd.x = hudPosX;
    txtCmd.y = hudPosY;
    txtCmd.w = m_width;  
    txtCmd.h = m_height;
    txtCmd.color = (0xFF << 24) | (m_textColorHex & 0x00FFFFFF);
    txtCmd.size = m_textSize;
    txtCmd.text = text.c_str();
    cmds.push_back(txtCmd);

    submitDrawCommands(moduleId, cmds);
}

void BreakIndicatorModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    
    if (j.contains("m_width")) m_width = j["m_width"].get<float>();
    if (j.contains("m_height")) m_height = j["m_height"].get<float>();
    if (j.contains("m_textSize")) m_textSize = j["m_textSize"].get<float>();
    if (j.contains("m_cornerRadius")) m_cornerRadius = j["m_cornerRadius"].get<float>();
    
    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    
    if (j.contains("barColorHex")) {
        std::string hexStr = j["barColorHex"].get<std::string>();
        if (hexStr.length() > 0 && hexStr[0] == '#') {
            try { m_barColorHex = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
        }
    }
    if (j.contains("m_rainbow")) m_rainbow = j["m_rainbow"].get<bool>();
    if (j.contains("m_rainbowSpeed")) m_rainbowSpeed = j["m_rainbowSpeed"].get<float>();
    
    if (j.contains("textColorHex")) {
        std::string hexStr = j["textColorHex"].get<std::string>();
        if (hexStr.length() > 0 && hexStr[0] == '#') {
            try { m_textColorHex = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
        }
    }
    
    if (j.contains("m_outline")) m_outline = j["m_outline"].get<bool>();
    if (j.contains("outlineColorHex")) {
        std::string hexStr = j["outlineColorHex"].get<std::string>();
        if (hexStr.length() > 0 && hexStr[0] == '#') {
            try { m_outlineColorHex = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
        }
    }
    if (j.contains("m_outlineThickness")) m_outlineThickness = j["m_outlineThickness"].get<float>();

    if (j.contains("m_alwaysShow")) m_alwaysShow = j["m_alwaysShow"].get<bool>();
}

void BreakIndicatorModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    
    j["m_width"] = m_width;
    j["m_height"] = m_height;
    j["m_textSize"] = m_textSize;
    j["m_cornerRadius"] = m_cornerRadius;
    
    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    
    char hexStr[10];
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_barColorHex);
    j["barColorHex"] = std::string(hexStr);
    
    j["m_rainbow"] = m_rainbow;
    j["m_rainbowSpeed"] = m_rainbowSpeed;
    
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_textColorHex);
    j["textColorHex"] = std::string(hexStr);
    
    j["m_outline"] = m_outline;
    
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_outlineColorHex);
    j["outlineColorHex"] = std::string(hexStr);
    
    j["m_outlineThickness"] = m_outlineThickness;

    j["m_alwaysShow"] = m_alwaysShow;
}
