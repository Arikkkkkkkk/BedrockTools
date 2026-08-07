#include "keystrokes.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <entt/entt.hpp>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

using uint = uint32_t;
using ushort = uint16_t;
using uchar = unsigned char;

enum class EntityId : uint32_t {};

template <size_t N, typename T>
struct bitset {
    T value;
    void set(size_t index, bool v) {
        if (v) value |= (1ULL << index);
        else value &= ~(1ULL << index);
    }
    bool test(size_t index) const {
        return (value & (1ULL << index)) != 0;
    }
};

struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = uint32_t;
    using version_type = uint16_t;
    static constexpr uint32_t entity_mask = 0x3FFFF;
    static constexpr uint32_t version_mask = 0x3FFF;
};

template<>
struct entt::entt_traits<EntityId> : entt::basic_entt_traits<EntityIdTraits> {
    static constexpr std::size_t page_size = ENTT_SPARSE_PAGE;
};

struct MoveInputState {
    bitset<27, uint> mFlagValues;
    bedrocktools::sdk::Vec2 mAnalogMoveVector;
    uchar mLookSlightDirField;
    uchar mLookNormalDirField;
    uchar mLookSmoothDirField;
    uchar pad[1];
};

struct MoveInputComponent {
    MoveInputState mInputState;
    MoveInputState mRawInputState;
    uchar mHoldAutoJumpInWaterTicks;
    uchar pad[3];
    bedrocktools::sdk::Vec2 mMove;
    bedrocktools::sdk::Vec2 mLookDelta;
    bedrocktools::sdk::Vec2 mInteractDir;
    bedrocktools::sdk::Vec3 mDisplacement;
    bedrocktools::sdk::Vec3 mDisplacementDelta;
    bedrocktools::sdk::Vec3 mCameraOrientation;
    bitset<11, ushort> mFlagValues;
    std::array<bool, 2> mIsPaddling;
};

class EntityRegistry;

class EntityContext {
public:
    inline entt::basic_registry<EntityId>& getRegistry() { return mEnTTRegistry; }

    template <class T>
    inline T* tryGetComponent() {
        return getRegistry().try_get<T>(mEntity);
    }

    EntityRegistry& mRegistry;
    entt::basic_registry<EntityId>& mEnTTRegistry;
    EntityId const mEntity;
};

static void keystrokesHSVtoRGB(float h, float s, float v, float& out_r, float& out_g, float& out_b) {
    if (s == 0.0f) {
        out_r = out_g = out_b = v;
        return;
    }
    h = std::fmod(h, 1.0f) * 6.0f;
    int i = static_cast<int>(std::floor(h));
    float f = h - static_cast<float>(i);
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

static KeystrokesModule* g_keystrokesMod = nullptr;

static void s_normalTickCallback(void* _this) {
    if (!g_keystrokesMod || !g_keystrokesMod->enabled) return;

    EntityContext* ctx = reinterpret_cast<EntityContext*>(reinterpret_cast<char*>(_this) + bedrocktools::sdk::offsets::Actor::mEntityContext);
    if (!ctx) return;

    auto* moveInput = ctx->tryGetComponent<MoveInputComponent>();
    if (!moveInput) return;

    auto& flags = moveInput->mRawInputState.mFlagValues;
    g_keystrokesMod->bW = flags.test(13);
    g_keystrokesMod->bA = flags.test(15);
    g_keystrokesMod->bS = flags.test(14);
    g_keystrokesMod->bD = flags.test(16);
    g_keystrokesMod->bSpace = flags.test(7);
    g_keystrokesMod->bSneak = flags.test(0);
}

KeystrokesModule::KeystrokesModule()
    : Module("Keystrokes", "Shows key presses and mouse CPS on screen.") {
    g_keystrokesMod = this;
}

KeystrokesModule::~KeystrokesModule() {
    if (g_keystrokesMod == this) g_keystrokesMod = nullptr;
}

void KeystrokesModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_normalTickCallback(event.player); });
}

void KeystrokesModule::onEnable() {
    m_mouseActive.store(true, std::memory_order_release);
}

void KeystrokesModule::onDisable() {
    m_mouseActive.store(false, std::memory_order_release);
    clearMouseState();
}

bool KeystrokesModule::onMouseEvent(int button, bool isDown) {
    if (button == 1) {
        if (!isDown) {
            m_lmbDown.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return false;
        m_lmbDown.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_leftClicks.push_back(std::chrono::steady_clock::now());
    } else if (button == 2) {
        if (!isDown) {
            m_rmbDown.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return false;
        m_rmbDown.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_rightClicks.push_back(std::chrono::steady_clock::now());
    }
    return false;
}

std::pair<int, int> KeystrokesModule::getMouseCps() {
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    std::lock_guard<std::mutex> lock(m_mouseMutex);
    while (!m_leftClicks.empty() && m_leftClicks.front() <= cutoff) m_leftClicks.pop_front();
    while (!m_rightClicks.empty() && m_rightClicks.front() <= cutoff) m_rightClicks.pop_front();
    return {static_cast<int>(m_leftClicks.size()), static_cast<int>(m_rightClicks.size())};
}

void KeystrokesModule::clearMouseState() {
    m_lmbDown.store(false, std::memory_order_relaxed);
    m_rmbDown.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mouseMutex);
    m_leftClicks.clear();
    m_rightClicks.clear();
}

void KeystrokesModule::onFrame() {
    if (!enabled) return;

    m_rainbowHue += 0.002f * m_rainbowSpeed;
    if (m_rainbowHue > 1.0f) m_rainbowHue -= 1.0f;

    auto updateAnim = [](KeyAnimState& state, bool pressed) {
        if (pressed) {
            state.pressProgress += 0.15f;
            if (state.pressProgress > 1.0f) state.pressProgress = 1.0f;
        } else {
            state.pressProgress -= 0.15f;
            if (state.pressProgress < 0.0f) state.pressProgress = 0.0f;
        }
    };

    const bool showMouseCps = m_showMouseCps.load(std::memory_order_relaxed);
    updateAnim(m_wState, bW);
    updateAnim(m_aState, bA);
    updateAnim(m_sState, bS);
    updateAnim(m_dState, bD);
    updateAnim(m_jumpState, bSpace);
    updateAnim(m_sneakState, bSneak);
    updateAnim(m_lmbState, showMouseCps && m_lmbDown.load(std::memory_order_relaxed));
    updateAnim(m_rmbState, showMouseCps && m_rmbDown.load(std::memory_order_relaxed));

    std::vector<PLModMenu_DrawCommand> cmds;

    float startX = hudPosX;
    float startY = hudPosY;
    float keySize = static_cast<float>(m_size);
    float spacing = 5.0f;

    auto addKey = [&](float x, float y, float w, std::string_view label, std::string_view detail, const KeyAnimState& state) {
        float progress = state.pressProgress;
        float currentH = keySize - (keySize * 0.1f * progress);
        float currentW = w - (keySize * 0.1f * progress);
        float offsetX = (w - currentW) / 2.0f;
        float offsetY = (keySize - currentH) / 2.0f;

        uint32_t baseBg = 0x44000000;
        uint32_t targetBg = m_pressedColor;
        if (m_rainbow) {
            float r, g, b;
            keystrokesHSVtoRGB(m_rainbowHue, 1.0f, 1.0f, r, g, b);
            targetBg = (0xAA << 24) | (static_cast<int>(r * 255) << 16) | (static_cast<int>(g * 255) << 8) | static_cast<int>(b * 255);
        } else {
            targetBg = (0xAA << 24) | (targetBg & 0x00FFFFFF);
        }

        auto lerpColor = [](uint32_t a, uint32_t b, float t) -> uint32_t {
            int aa = (a >> 24) & 0xFF;
            int ar = (a >> 16) & 0xFF;
            int ag = (a >> 8) & 0xFF;
            int ab = a & 0xFF;
            int ba = (b >> 24) & 0xFF;
            int br = (b >> 16) & 0xFF;
            int bg = (b >> 8) & 0xFF;
            int bb = b & 0xFF;
            int ra = static_cast<int>(aa + (ba - aa) * t);
            int rr = static_cast<int>(ar + (br - ar) * t);
            int rg = static_cast<int>(ag + (bg - ag) * t);
            int rb = static_cast<int>(ab + (bb - ab) * t);
            return (ra << 24) | (rr << 16) | (rg << 8) | rb;
        };

        PLModMenu_DrawCommand bgCmd = {};
        bgCmd.type = PL_DRAW_RECT_FILLED;
        bgCmd.x = x + offsetX;
        bgCmd.y = y + offsetY;
        bgCmd.w = currentW;
        bgCmd.h = currentH;
        bgCmd.color = lerpColor(baseBg, targetBg, progress);
        if (m_roundKeys) bgCmd.x3 = keySize * 0.1f;
        cmds.push_back(std::move(bgCmd));

        PLModMenu_DrawCommand textCmd = {};
        textCmd.type = PL_DRAW_TEXT;
        textCmd.x = x + offsetX;
        textCmd.y = y + offsetY;
        textCmd.w = currentW;
        textCmd.h = detail.empty() ? currentH : currentH * 0.58f;
        textCmd.color = 0xFFFFFFFF;
        textCmd.size = currentH * (detail.empty() ? 0.5f : 0.34f);
        textCmd.text = label;
        cmds.push_back(std::move(textCmd));

        if (!detail.empty()) {
            PLModMenu_DrawCommand detailCmd = {};
            detailCmd.type = PL_DRAW_TEXT;
            detailCmd.x = x + offsetX;
            detailCmd.y = y + offsetY + currentH * 0.48f;
            detailCmd.w = currentW;
            detailCmd.h = currentH * 0.45f;
            detailCmd.color = 0xFFFFFFFF;
            detailCmd.size = currentH * 0.25f;
            detailCmd.text = detail;
            cmds.push_back(std::move(detailCmd));
        }
    };

    addKey(startX + keySize + spacing, startY, keySize, "W", "", m_wState);
    addKey(startX, startY + keySize + spacing, keySize, "A", "", m_aState);
    addKey(startX + keySize + spacing, startY + keySize + spacing, keySize, "S", "", m_sState);
    addKey(startX + (keySize + spacing) * 2, startY + keySize + spacing, keySize, "D", "", m_dState);

    float currentY = startY + (keySize + spacing) * 2;
    float totalW = (keySize * 3) + (spacing * 2);

    if (showMouseCps) {
        auto [leftCps, rightCps] = getMouseCps();
        const float mouseWidth = (totalW - spacing) * 0.5f;
        const std::string leftText = std::to_string(leftCps) + " CPS";
        const std::string rightText = std::to_string(rightCps) + " CPS";
        addKey(startX, currentY, mouseWidth, "LMB", leftText, m_lmbState);
        addKey(startX + mouseWidth + spacing, currentY, mouseWidth, "RMB", rightText, m_rmbState);
        currentY += keySize + spacing;
    }

    if (m_showJump) {
        addKey(startX, currentY, totalW, "JUMP", "", m_jumpState);
        currentY += keySize + spacing;
    }

    if (m_showSneak) {
        addKey(startX, currentY, totalW, "SNEAK", "", m_sneakState);
    }

    submitDrawCommands(moduleId, cmds);
}

void KeystrokesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_size")) m_size = j["m_size"].get<int>();
    if (j.contains("m_showJump")) m_showJump = j["m_showJump"].get<bool>();
    if (j.contains("m_showSneak")) m_showSneak = j["m_showSneak"].get<bool>();
    if (j.contains("m_showMouseCps")) m_showMouseCps.store(j["m_showMouseCps"].get<bool>(), std::memory_order_relaxed);
    if (j.contains("roundKeys")) m_roundKeys = j["roundKeys"].get<bool>();
    if (j.contains("rainbow")) m_rainbow = j["rainbow"].get<bool>();
    if (j.contains("rainbowSpeed")) m_rainbowSpeed = j["rainbowSpeed"].get<float>();
    if (j.contains("color")) {
        std::string hexStr = j["color"].get<std::string>();
        if (!hexStr.empty() && hexStr[0] == '#') {
            try {
                m_pressedColor = std::stoul(hexStr.substr(1), nullptr, 16);
            } catch (...) {
            }
        }
    }
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (!m_showMouseCps.load(std::memory_order_relaxed)) clearMouseState();
}

void KeystrokesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_size"] = m_size;
    j["m_showJump"] = m_showJump;
    j["m_showSneak"] = m_showSneak;
    j["m_showMouseCps"] = m_showMouseCps.load(std::memory_order_relaxed);
    j["roundKeys"] = m_roundKeys;
    j["rainbow"] = m_rainbow;
    j["rainbowSpeed"] = m_rainbowSpeed;

    char hexStr[10];
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_pressedColor);
    j["color"] = std::string(hexStr);

    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
}
