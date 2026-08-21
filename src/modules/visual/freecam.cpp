#include "freecam.hpp"
#include "core/memory/Hooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Functions.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/render/LevelRendererPlayer.hpp>

#include <algorithm>
#include <cmath>

using bedrocktools::sdk::ClientInstance;
using bedrocktools::sdk::Player;
using bedrocktools::sdk::Vec3;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::BlockPos;
using bedrocktools::sdk::LevelRenderer;
using bedrocktools::sdk::LevelRendererPlayer;
using bedrocktools::sdk::BlockSource;

namespace {

FreecamModule* g_freecamMod = nullptr;

using IsSolidBlockingFn = bool (*)(BlockSource*, const BlockPos*);
IsSolidBlockingFn g_isSolidBlocking = nullptr;

void (*g_renderLevelOriginal)(void* _this, void* screenContext, void* a3) = nullptr;

bool isSolid(BlockSource* region, const Vec3& pos) {
    if (!region || !g_isSolidBlocking) return false;
    const BlockPos bp{
        static_cast<int>(std::floor(pos.x)),
        static_cast<int>(std::floor(pos.y)),
        static_cast<int>(std::floor(pos.z)),
    };
    return g_isSolidBlocking(region, &bp);
}

void renderLevelHook(void* _this, void* screenContext, void* a3) {
    if (g_freecamMod && g_freecamMod->enabled && _this) {
        auto* levelRenderer = reinterpret_cast<LevelRenderer*>(_this);
        if (LevelRendererPlayer* lrp = levelRenderer->playerRenderer()) {
            // First frame after enabling: adopt whatever position the real
            // camera is already at, so entering freecam doesn't jump.
            if (!g_freecamMod->positionCaptured) {
                g_freecamMod->freeCamPos = lrp->cameraPosition();
                g_freecamMod->positionCaptured = true;
            }
            lrp->cameraPosition() = g_freecamMod->freeCamPos;
        }
    }

    if (g_renderLevelOriginal) {
        g_renderLevelOriginal(_this, screenContext, a3);
    }
}

}

FreecamModule::FreecamModule()
    : Module("Freecam", "Detach the camera and fly freely, independent of your character.") {
    g_freecamMod = this;
}

FreecamModule::~FreecamModule() {
    if (g_freecamMod == this) g_freecamMod = nullptr;
}

void FreecamModule::onInit() {
    if (!m_renderLevelHooked) {
        const uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
        if (addr != 0) {
            bedrocktools::hooks::install(
                reinterpret_cast<void*>(addr),
                reinterpret_cast<void*>(renderLevelHook),
                reinterpret_cast<void**>(&g_renderLevelOriginal)
            );
            m_renderLevelHooked = true;
        }
    }
    if (!g_isSolidBlocking) {
        g_isSolidBlocking = bedrocktools::sdk::function<IsSolidBlockingFn>(
            bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock
        );
    }
}

void FreecamModule::onEnable() {
    positionCaptured = false;
    m_lastFrameTime = std::chrono::steady_clock::now();
}

void FreecamModule::onDisable() {
    positionCaptured = false;
    movingForward = movingBack = movingLeft = movingRight = movingUp = movingDown = false;
}

void FreecamModule::onKeybindEvent(const std::string& key, bool isDown) {
    if (key == "keybind") {
        Module::onKeybindEvent(key, isDown);
        return;
    }
    if (key == "forwardKeybind")      movingForward = isDown;
    else if (key == "backKeybind")    movingBack = isDown;
    else if (key == "leftKeybind")    movingLeft = isDown;
    else if (key == "rightKeybind")   movingRight = isDown;
    else if (key == "upKeybind")      movingUp = isDown;
    else if (key == "downKeybind")    movingDown = isDown;
}

void FreecamModule::onFrame() {
    if (!enabled || !positionCaptured) return;

    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_lastFrameTime).count();
    m_lastFrameTime = now;
    dt = std::clamp(dt, 0.0f, 0.1f);

    if (!(movingForward || movingBack || movingLeft || movingRight || movingUp || movingDown)) return;

    ClientInstance* client = ClientInstance::current();
    Vec2 rot{0.f, 0.f};
    if (client) {
        if (Player* player = client->localPlayer()) rot = player->rotation();
    }

    // rotation().x is pitch, rotation().y is yaw, both in degrees.
    const float pitch = rot.x * (3.14159265f / 180.0f);
    const float yaw   = rot.y * (3.14159265f / 180.0f);

    const Vec3 forward{
        -std::sin(yaw) * std::cos(pitch),
        -std::sin(pitch),
        std::cos(yaw) * std::cos(pitch),
    };
    const Vec3 right{std::cos(yaw), 0.0f, std::sin(yaw)};

    Vec3 move{0.f, 0.f, 0.f};
    if (movingForward) { move.x += forward.x; move.y += forward.y; move.z += forward.z; }
    if (movingBack)    { move.x -= forward.x; move.y -= forward.y; move.z -= forward.z; }
    if (movingRight)   { move.x += right.x;   move.z += right.z; }
    if (movingLeft)    { move.x -= right.x;   move.z -= right.z; }
    if (movingUp)      { move.y += 1.0f; }
    if (movingDown)    { move.y -= 1.0f; }

    const float len = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z);
    if (len < 0.0001f) return;
    move.x = move.x / len * flightSpeed * dt;
    move.y = move.y / len * flightSpeed * dt;
    move.z = move.z / len * flightSpeed * dt;

    BlockSource* region = client ? client->region() : nullptr;

    if (throughWalls || !region || !g_isSolidBlocking) {
        freeCamPos.x += move.x;
        freeCamPos.y += move.y;
        freeCamPos.z += move.z;
        return;
    }

    // Per-axis check so bumping into one wall doesn't kill motion along
    // the other two — lets you slide along a surface instead of sticking.
    Vec3 candidate = freeCamPos;
    candidate.x += move.x;
    if (!isSolid(region, candidate)) freeCamPos.x = candidate.x;

    candidate = freeCamPos;
    candidate.y += move.y;
    if (!isSolid(region, candidate)) freeCamPos.y = candidate.y;

    candidate = freeCamPos;
    candidate.z += move.z;
    if (!isSolid(region, candidate)) freeCamPos.z = candidate.z;
}

void FreecamModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("flightSpeed"))    flightSpeed    = j["flightSpeed"].get<float>();
    if (j.contains("throughWalls"))   throughWalls   = j["throughWalls"].get<bool>();
    if (j.contains("forwardKeybind")) forwardKeybind = j["forwardKeybind"].get<int>();
    if (j.contains("backKeybind"))    backKeybind    = j["backKeybind"].get<int>();
    if (j.contains("leftKeybind"))    leftKeybind    = j["leftKeybind"].get<int>();
    if (j.contains("rightKeybind"))   rightKeybind   = j["rightKeybind"].get<int>();
    if (j.contains("upKeybind"))      upKeybind      = j["upKeybind"].get<int>();
    if (j.contains("downKeybind"))    downKeybind    = j["downKeybind"].get<int>();
}

void FreecamModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["flightSpeed"]    = flightSpeed;
    j["throughWalls"]   = throughWalls;
    j["forwardKeybind"] = forwardKeybind;
    j["backKeybind"]    = backKeybind;
    j["leftKeybind"]    = leftKeybind;
    j["rightKeybind"]   = rightKeybind;
    j["upKeybind"]      = upKeybind;
    j["downKeybind"]    = downKeybind;
}
