#include "betterhandhelds.hpp"
#include "core/memory/Hooks.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

static BetterHandheldsModule* g_betterHandheldsMod = nullptr;


static void (*_renderItem_orig)(void*, void*, void*, void*, int, int, int, int) = nullptr;

static void _renderItem_hook(void* _this, void* renderContext, void* entity,
                             void* item, int posAndRotSet, int itemFlags,
                             int useMatrixAsIs, int renderingMainHand)
{
    if (g_betterHandheldsMod && g_betterHandheldsMod->enabled) {
        bool skip = g_betterHandheldsMod->isThirdPerson() && !g_betterHandheldsMod->applyThirdPerson;

        if (!skip) {
            const float scale = g_betterHandheldsMod->mainHandScale;
            const float px = renderingMainHand
                ? g_betterHandheldsMod->mainHandPosX
                : g_betterHandheldsMod->offHandPosX;
            const float py = renderingMainHand
                ? g_betterHandheldsMod->mainHandPosY
                : g_betterHandheldsMod->offHandPosY;
            const float pz = renderingMainHand
                ? g_betterHandheldsMod->mainHandPosZ
                : g_betterHandheldsMod->offHandPosZ;

            const bool hasScale = scale != 1.0f;
            const bool hasPos = px != 0.0f || py != 0.0f || pz != 0.0f;

            if (hasScale || hasPos) {
                uintptr_t rcBase = (uintptr_t)renderContext;
                uintptr_t ptr1 = *(uintptr_t*)(rcBase + bedrocktools::sdk::offsets::RenderContext::mMatrixStackWrapper);
                if (ptr1 != 0) {
                    uintptr_t matStack = *(uintptr_t*)(ptr1 + bedrocktools::sdk::offsets::MatrixStackWrapper::mMatrixStack);
                    if (matStack != 0) {
                        uintptr_t* blocks = *(uintptr_t**)(matStack + bedrocktools::sdk::offsets::MatrixStack::mBlocks);
                        size_t start = *(size_t*)(matStack + bedrocktools::sdk::offsets::MatrixStack::mStart);
                        size_t size  = *(size_t*)(matStack + bedrocktools::sdk::offsets::MatrixStack::mSize);

                        if (blocks != nullptr && size > 0) {
                            size_t last = start + size - 1;
                            size_t blockOff = (last >> 3) & ~(size_t)7;
                            size_t elemIdx  = last & 0x3F;
                            uintptr_t blockPtr = *(uintptr_t*)((uintptr_t)blocks + blockOff);

                            if (blockPtr != 0) {
                                glm::mat4& matrix = *(glm::mat4*)(blockPtr + elemIdx * 64);

                                // Position first, then scale — same order View Model
                                // uses, so the offset reads as a plain nudge in the
                                // current local frame rather than being stretched
                                // by whatever scale factor is also active.
                                if (hasPos) {
                                    matrix = glm::translate(matrix, glm::vec3(px, py, pz));
                                }
                                if (hasScale) {
                                    matrix = glm::scale(matrix, glm::vec3(scale, scale, scale));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (_renderItem_orig)
        _renderItem_orig(_this, renderContext, entity, item, posAndRotSet,
                         itemFlags, useMatrixAsIs, renderingMainHand);
}


static int (*_getPerspective_orig)(void*) = nullptr;

static int _getPerspective_hook(void* _this) {
    int result = 0;
    if (_getPerspective_orig)
        result = _getPerspective_orig(_this);

    if (g_betterHandheldsMod && g_betterHandheldsMod->enabled) {
        g_betterHandheldsMod->thirdPerson = (result != 0);
    }
    return result;
}


BetterHandheldsModule::BetterHandheldsModule()
    : Module("Better Handhelds", "Independently resize and reposition your main-hand and off-hand items.") {
    g_betterHandheldsMod = this;
}

BetterHandheldsModule::~BetterHandheldsModule() {
    if (g_betterHandheldsMod == this) g_betterHandheldsMod = nullptr;
}

bool BetterHandheldsModule::isThirdPerson() const {
    return thirdPerson;
}

void BetterHandheldsModule::onInit() {
    if (!m_renderItemHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderItem);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_renderItem_hook, (void**)&_renderItem_orig);
            m_renderItemHooked = true;
        }
    }

    if (!m_perspectiveHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GetPerspective);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_getPerspective_hook, (void**)&_getPerspective_orig);
            m_perspectiveHooked = true;
        }
    }
}

void BetterHandheldsModule::onEnable() {
}

void BetterHandheldsModule::onDisable() {
}

void BetterHandheldsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("mainHandScale")) mainHandScale = j["mainHandScale"].get<float>();
    if (j.contains("offHandScale"))  offHandScale  = j["offHandScale"].get<float>();
    if (j.contains("mainHandPosX"))  mainHandPosX  = j["mainHandPosX"].get<float>();
    if (j.contains("mainHandPosY"))  mainHandPosY  = j["mainHandPosY"].get<float>();
    if (j.contains("mainHandPosZ"))  mainHandPosZ  = j["mainHandPosZ"].get<float>();
    if (j.contains("offHandPosX"))   offHandPosX   = j["offHandPosX"].get<float>();
    if (j.contains("offHandPosY"))   offHandPosY   = j["offHandPosY"].get<float>();
    if (j.contains("offHandPosZ"))   offHandPosZ   = j["offHandPosZ"].get<float>();
    if (j.contains("applyThirdPerson")) applyThirdPerson = j["applyThirdPerson"].get<bool>();
}

void BetterHandheldsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["mainHandScale"] = mainHandScale;
    j["offHandScale"]  = offHandScale;
    j["mainHandPosX"]  = mainHandPosX;
    j["mainHandPosY"]  = mainHandPosY;
    j["mainHandPosZ"]  = mainHandPosZ;
    j["offHandPosX"]   = offHandPosX;
    j["offHandPosY"]   = offHandPosY;
    j["offHandPosZ"]   = offHandPosZ;
    j["applyThirdPerson"] = applyThirdPerson;
}
