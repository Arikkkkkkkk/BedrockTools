#include "connectedglass.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

enum class GlassFace : uint8_t {
    Down = 0,
    Up = 1,
    North = 2,
    South = 3,
    West = 4,
    East = 5,
};

enum class TextureEdge : uint8_t {
    U0,
    U1,
    V0,
    V1,
};

enum class GlassShape : uint8_t {
    None,
    Block,
    Pane,
};

struct Vec3Raw {
    float x;
    float y;
    float z;
};

struct BlockPosRaw {
    int32_t x;
    int32_t y;
    int32_t z;
};

struct GlassInfo {
    GlassShape shape = GlassShape::None;
    std::string_view name;
    const void* identity = nullptr;

    bool valid() const {
        return shape != GlassShape::None;
    }
};

struct CropEdges {
    bool u0 = false;
    bool u1 = false;
    bool v0 = false;
    bool v1 = false;

    bool any() const {
        return u0 || u1 || v0 || v1;
    }
};

struct BlockOffset {
    int32_t x;
    int32_t y;
    int32_t z;
};

struct PaneContext {
    void* tessellator = nullptr;
    const void* block = nullptr;
    BlockPosRaw pos{};
    bool active = false;
};

using FaceFn = void (*)(void*, void*, const void*, const Vec3Raw*, const void*);
using PaneFn = bool (*)(void*, void*, const void*, const BlockPosRaw*, bool);
using BlockSourceGetBlockFn = const void* (*)(void*, const BlockPosRaw*, int32_t);
using BlockSourceIsSolidBlockingBlockFn = bool (*)(void*, const BlockPosRaw*);
using TextureUVCopyCtorFn = void* (*)(void*, const void*);
using TextureUVDtorFn = void (*)(void*);
using SetAllDirtyFn = void (*)(void*, bool, bool);

std::atomic_bool g_enabled{false};
std::atomic_bool g_rebuildPending{false};
std::atomic_bool g_connectGlassBlocks{true};
std::atomic_bool g_connectGlassPanes{true};
std::atomic_bool g_connectDifferentColors{true};
std::atomic_bool g_connectPanesToBlocks{true};
std::atomic_bool g_removeFlatBorders{true};
std::atomic_bool g_removeCornerBorders{true};
std::atomic_bool g_removeOuterVerticalBorders{false};
std::atomic_bool g_removeOuterHorizontalBorders{false};
std::atomic_bool g_removeOuterTopBottomFaceBorders{false};
std::atomic_bool g_affectSideFaces{true};
std::atomic_bool g_affectTopFace{true};
std::atomic_bool g_affectBottomFace{true};
std::atomic<float> g_borderWidth{2.0f};

FaceFn g_downOriginal = nullptr;
FaceFn g_upOriginal = nullptr;
FaceFn g_northOriginal = nullptr;
FaceFn g_southOriginal = nullptr;
FaceFn g_westOriginal = nullptr;
FaceFn g_eastOriginal = nullptr;
PaneFn g_paneOriginal = nullptr;

bedrocktools::hooks::Handle g_downHook = nullptr;
bedrocktools::hooks::Handle g_upHook = nullptr;
bedrocktools::hooks::Handle g_northHook = nullptr;
bedrocktools::hooks::Handle g_southHook = nullptr;
bedrocktools::hooks::Handle g_westHook = nullptr;
bedrocktools::hooks::Handle g_eastHook = nullptr;
bedrocktools::hooks::Handle g_paneHook = nullptr;

BlockSourceGetBlockFn g_getBlock = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;
TextureUVCopyCtorFn g_textureCopyCtor = nullptr;
TextureUVDtorFn g_textureDtor = nullptr;
SetAllDirtyFn g_setAllDirty = nullptr;

thread_local PaneContext g_paneContext;

template <typename T>
T& field(void* base, size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
}

template <typename T>
const T& field(const void* base, size_t offset) {
    return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(base) + offset);
}

std::string_view stripNamespace(std::string_view name) {
    constexpr std::string_view prefix = "minecraft:";
    if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
    return name;
}

GlassInfo classifyGlassName(std::string_view rawName, const void* identity = nullptr) {
    const std::string_view name = stripNamespace(rawName);
    const bool pane = name == "glass_pane"
        || name == "hard_glass_pane"
        || name == "stained_glass_pane"
        || name == "hard_stained_glass_pane"
        || name.ends_with("_stained_glass_pane");
    const bool block = name == "glass"
        || name == "tinted_glass"
        || name == "hard_glass"
        || name == "stained_glass"
        || name == "hard_stained_glass"
        || name.ends_with("_stained_glass");

    if (pane) return {GlassShape::Pane, name, identity};
    if (block) return {GlassShape::Block, name, identity};
    return {};
}

std::string_view getBlockName(const void* block) {
    if (!block) return {};

    const uintptr_t blockType = field<uintptr_t>(block, bedrocktools::sdk::offsets::Block::mBlockType);
    if (!blockType) return {};

    const uintptr_t stringAddress = blockType
        + bedrocktools::sdk::offsets::BlockType::mNameInfo
        + bedrocktools::sdk::offsets::NameInfo::mFullName
        + bedrocktools::sdk::offsets::HashedString::mString;
    const auto* name = reinterpret_cast<const std::string*>(stringAddress);
    if (name->size() > 256 || (!name->empty() && name->data() == nullptr)) return {};
    return std::string_view(name->data(), name->size());
}

GlassInfo classifyGlass(const void* block) {
    return classifyGlassName(getBlockName(block), block);
}

std::string_view shapeIndependentName(const GlassInfo& info) {
    std::string_view result = info.name;
    constexpr std::string_view paneSuffix = "_pane";
    if (info.shape == GlassShape::Pane && result.ends_with(paneSuffix)) {
        result.remove_suffix(paneSuffix.size());
    }
    return result;
}

bool shapeEnabled(const GlassInfo& info) {
    if (info.shape == GlassShape::Block) {
        return g_connectGlassBlocks.load(std::memory_order_relaxed);
    }
    if (info.shape == GlassShape::Pane) {
        return g_connectGlassPanes.load(std::memory_order_relaxed);
    }
    return false;
}

bool compatibleGlass(const GlassInfo& current, const GlassInfo& neighbor) {
    if (!current.valid() || !neighbor.valid() || !shapeEnabled(current) || !shapeEnabled(neighbor)) {
        return false;
    }

    if (current.shape != neighbor.shape
        && !g_connectPanesToBlocks.load(std::memory_order_relaxed)) {
        return false;
    }

    if (g_connectDifferentColors.load(std::memory_order_relaxed)) return true;

    if (current.shape == neighbor.shape && current.identity && neighbor.identity) {
        return current.identity == neighbor.identity;
    }

    return shapeIndependentName(current) == shapeIndependentName(neighbor);
}

BlockPosRaw add(const BlockPosRaw& pos, const BlockOffset& delta) {
    return {pos.x + delta.x, pos.y + delta.y, pos.z + delta.z};
}

BlockOffset faceNormal(GlassFace face) {
    switch (face) {
    case GlassFace::Down: return {0, -1, 0};
    case GlassFace::Up: return {0, 1, 0};
    case GlassFace::North: return {0, 0, -1};
    case GlassFace::South: return {0, 0, 1};
    case GlassFace::West: return {-1, 0, 0};
    case GlassFace::East: return {1, 0, 0};
    }
    return {0, 0, 0};
}

const void* getBlock(void* region, const BlockPosRaw& pos) {
    return region && g_getBlock ? g_getBlock(region, &pos, 0) : nullptr;
}

bool faceEnabled(GlassFace face) {
    if (face == GlassFace::Up) return g_affectTopFace.load(std::memory_order_relaxed);
    if (face == GlassFace::Down) return g_affectBottomFace.load(std::memory_order_relaxed);
    return g_affectSideFaces.load(std::memory_order_relaxed);
}

bool removeOuterBorder(GlassFace face, TextureEdge edge) {
    if (face == GlassFace::Up || face == GlassFace::Down) {
        return g_removeOuterTopBottomFaceBorders.load(std::memory_order_relaxed);
    }
    if (edge == TextureEdge::U0 || edge == TextureEdge::U1) {
        return g_removeOuterVerticalBorders.load(std::memory_order_relaxed);
    }
    return g_removeOuterHorizontalBorders.load(std::memory_order_relaxed);
}

bool isFaceExposed(void* region, const BlockPosRaw& pos, GlassFace face) {
    const BlockPosRaw outsidePos = add(pos, faceNormal(face));
    const void* outsideBlock = getBlock(region, outsidePos);
    if (!outsideBlock || classifyGlass(outsideBlock).valid()) return false;
    return g_isSolidBlockingBlock && !g_isSolidBlockingBlock(region, &outsidePos);
}

bool shouldCropEdge(
    void* region,
    const BlockPosRaw& pos,
    const GlassInfo& current,
    GlassFace face,
    TextureEdge edge,
    const BlockOffset& neighborOffset
) {
    const BlockPosRaw neighborPos = add(pos, neighborOffset);
    const void* neighborBlock = getBlock(region, neighborPos);
    if (neighborBlock) {
        const GlassInfo neighbor = classifyGlass(neighborBlock);
        if (compatibleGlass(current, neighbor)) {
            if (face == GlassFace::Down || face == GlassFace::Up || isFaceExposed(region, neighborPos, face)) {
                return g_removeFlatBorders.load(std::memory_order_relaxed);
            }
            return g_removeCornerBorders.load(std::memory_order_relaxed);
        }
    }
    return removeOuterBorder(face, edge);
}

CropEdges getCropEdges(void* region, const BlockPosRaw& pos, const GlassInfo& current, GlassFace face) {
    CropEdges edges;
    if (!faceEnabled(face)) return edges;

    switch (face) {
    case GlassFace::Down:
        edges.u0 = shouldCropEdge(region, pos, current, face, TextureEdge::U0, {-1, 0, 0});
        edges.u1 = shouldCropEdge(region, pos, current, face, TextureEdge::U1, {1, 0, 0});
        edges.v0 = shouldCropEdge(region, pos, current, face, TextureEdge::V0, {0, 0, 1});
        edges.v1 = shouldCropEdge(region, pos, current, face, TextureEdge::V1, {0, 0, -1});
        break;
    case GlassFace::Up:
        edges.u0 = shouldCropEdge(region, pos, current, face, TextureEdge::U0, {-1, 0, 0});
        edges.u1 = shouldCropEdge(region, pos, current, face, TextureEdge::U1, {1, 0, 0});
        edges.v0 = shouldCropEdge(region, pos, current, face, TextureEdge::V0, {0, 0, -1});
        edges.v1 = shouldCropEdge(region, pos, current, face, TextureEdge::V1, {0, 0, 1});
        break;
    case GlassFace::North:
        edges.u0 = shouldCropEdge(region, pos, current, face, TextureEdge::U0, {1, 0, 0});
        edges.u1 = shouldCropEdge(region, pos, current, face, TextureEdge::U1, {-1, 0, 0});
        edges.v0 = shouldCropEdge(region, pos, current, face, TextureEdge::V0, {0, 1, 0});
        edges.v1 = shouldCropEdge(region, pos, current, face, TextureEdge::V1, {0, -1, 0});
        break;
    case GlassFace::South:
        edges.u0 = shouldCropEdge(region, pos, current, face, TextureEdge::U0, {-1, 0, 0});
        edges.u1 = shouldCropEdge(region, pos, current, face, TextureEdge::U1, {1, 0, 0});
        edges.v0 = shouldCropEdge(region, pos, current, face, TextureEdge::V0, {0, 1, 0});
        edges.v1 = shouldCropEdge(region, pos, current, face, TextureEdge::V1, {0, -1, 0});
        break;
    case GlassFace::West:
        edges.u0 = shouldCropEdge(region, pos, current, face, TextureEdge::U0, {0, 0, -1});
        edges.u1 = shouldCropEdge(region, pos, current, face, TextureEdge::U1, {0, 0, 1});
        edges.v0 = shouldCropEdge(region, pos, current, face, TextureEdge::V0, {0, 1, 0});
        edges.v1 = shouldCropEdge(region, pos, current, face, TextureEdge::V1, {0, -1, 0});
        break;
    case GlassFace::East:
        edges.u0 = shouldCropEdge(region, pos, current, face, TextureEdge::U0, {0, 0, 1});
        edges.u1 = shouldCropEdge(region, pos, current, face, TextureEdge::U1, {0, 0, -1});
        edges.v0 = shouldCropEdge(region, pos, current, face, TextureEdge::V0, {0, 1, 0});
        edges.v1 = shouldCropEdge(region, pos, current, face, TextureEdge::V1, {0, -1, 0});
        break;
    }

    return edges;
}

BlockPosRaw resolveBlockPosition(void* tessellator, const void* block, const Vec3Raw* position) {
    if (g_paneContext.active
        && g_paneContext.tessellator == tessellator
        && g_paneContext.block == block) {
        return g_paneContext.pos;
    }

    return {
        static_cast<int32_t>(std::floor(position->x)),
        static_cast<int32_t>(std::floor(position->y)),
        static_cast<int32_t>(std::floor(position->z)),
    };
}

class TextureCopy final {
public:
    explicit TextureCopy(const void* source) {
        if (g_textureCopyCtor && source) {
            g_textureCopyCtor(m_storage.data(), source);
            m_constructed = true;
        }
    }

    ~TextureCopy() {
        if (m_constructed && g_textureDtor) g_textureDtor(m_storage.data());
    }

    TextureCopy(const TextureCopy&) = delete;
    TextureCopy& operator=(const TextureCopy&) = delete;

    bool valid() const {
        return m_constructed;
    }

    void* data() {
        return m_storage.data();
    }

private:
    alignas(bedrocktools::sdk::offsets::TextureUVCoordinateSet::Alignment)
        std::array<std::byte, bedrocktools::sdk::offsets::TextureUVCoordinateSet::Size> m_storage{};
    bool m_constructed = false;
};

void cropTexture(void* texture, const CropEdges& edges) {
    float& u0 = field<float>(texture, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mU0);
    float& v0 = field<float>(texture, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mV0);
    float& u1 = field<float>(texture, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mU1);
    float& v1 = field<float>(texture, bedrocktools::sdk::offsets::TextureUVCoordinateSet::mV1);

    const float borderPixels = std::clamp(g_borderWidth.load(std::memory_order_relaxed), 0.0f, 7.5f);
    if (borderPixels <= 0.0f) return;

    constexpr float logicalTextureSize = 16.0f;
    const float uPixel = (u1 - u0) / logicalTextureSize;
    const float vPixel = (v1 - v0) / logicalTextureSize;

    if (edges.u0) u0 += uPixel * borderPixels;
    if (edges.u1) u1 -= uPixel * borderPixels;
    if (edges.v0) v0 += vPixel * borderPixels;
    if (edges.v1) v1 -= vPixel * borderPixels;

    field<uint8_t>(
        texture,
        bedrocktools::sdk::offsets::TextureUVCoordinateSet::mIsotropicFaceData
            + bedrocktools::sdk::offsets::IsotropicFaceData::mTextureIsotropic
    ) = 0;
}

class FaceStateGuard final {
public:
    FaceStateGuard(void* tessellator, GlassFace face)
        : m_flip(&field<uint8_t>(
              tessellator,
              bedrocktools::sdk::offsets::BlockTessellator::mFlipFace
                  + static_cast<size_t>(face) * bedrocktools::sdk::offsets::FlipFace::ElementSize
          )),
          m_xFlip(&field<uint8_t>(tessellator, bedrocktools::sdk::offsets::BlockTessellator::mXFlipTexture)),
          m_oldFlip(*m_flip),
          m_oldXFlip(*m_xFlip) {
        *m_flip = bedrocktools::sdk::offsets::FlipFace::DontRotate;
        *m_xFlip = 0;
    }

    ~FaceStateGuard() {
        *m_flip = m_oldFlip;
        *m_xFlip = m_oldXFlip;
    }

    FaceStateGuard(const FaceStateGuard&) = delete;
    FaceStateGuard& operator=(const FaceStateGuard&) = delete;

private:
    uint8_t* m_flip;
    uint8_t* m_xFlip;
    uint8_t m_oldFlip;
    uint8_t m_oldXFlip;
};

void renderFace(
    FaceFn original,
    GlassFace face,
    void* tessellator,
    void* meshTessellator,
    const void* block,
    const Vec3Raw* position,
    const void* inputTexture
) {
    if (!original) return;

    const GlassInfo current = classifyGlass(block);
    if (!g_enabled.load(std::memory_order_relaxed)
        || !tessellator
        || !block
        || !position
        || !inputTexture
        || !g_getBlock
        || !g_isSolidBlockingBlock
        || !g_textureCopyCtor
        || !g_textureDtor
        || !current.valid()
        || !shapeEnabled(current)) {
        original(tessellator, meshTessellator, block, position, inputTexture);
        return;
    }

    const void* internalTexture = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(tessellator) + bedrocktools::sdk::offsets::BlockTessellator::mInternalTexture
    );
    if (field<uint8_t>(tessellator, bedrocktools::sdk::offsets::BlockTessellator::mUseInternalTexture) != 0
        && inputTexture == internalTexture) {
        original(tessellator, meshTessellator, block, position, inputTexture);
        return;
    }

    void* region = field<void*>(tessellator, bedrocktools::sdk::offsets::BlockTessellator::mRegion);
    if (!region) {
        original(tessellator, meshTessellator, block, position, inputTexture);
        return;
    }

    const BlockPosRaw pos = resolveBlockPosition(tessellator, block, position);
    const GlassInfo worldBlock = classifyGlass(getBlock(region, pos));
    if (!worldBlock.valid()) {
        original(tessellator, meshTessellator, block, position, inputTexture);
        return;
    }

    const CropEdges edges = getCropEdges(region, pos, current, face);
    if (!edges.any()) {
        original(tessellator, meshTessellator, block, position, inputTexture);
        return;
    }

    TextureCopy texture(inputTexture);
    if (!texture.valid()) {
        original(tessellator, meshTessellator, block, position, inputTexture);
        return;
    }

    cropTexture(texture.data(), edges);
    FaceStateGuard state(tessellator, face);
    original(tessellator, meshTessellator, block, position, texture.data());
}

void downHook(void* a0, void* a1, const void* a2, const Vec3Raw* a3, const void* a4) {
    renderFace(g_downOriginal, GlassFace::Down, a0, a1, a2, a3, a4);
}

void upHook(void* a0, void* a1, const void* a2, const Vec3Raw* a3, const void* a4) {
    renderFace(g_upOriginal, GlassFace::Up, a0, a1, a2, a3, a4);
}

void northHook(void* a0, void* a1, const void* a2, const Vec3Raw* a3, const void* a4) {
    renderFace(g_northOriginal, GlassFace::North, a0, a1, a2, a3, a4);
}

void southHook(void* a0, void* a1, const void* a2, const Vec3Raw* a3, const void* a4) {
    renderFace(g_southOriginal, GlassFace::South, a0, a1, a2, a3, a4);
}

void westHook(void* a0, void* a1, const void* a2, const Vec3Raw* a3, const void* a4) {
    renderFace(g_westOriginal, GlassFace::West, a0, a1, a2, a3, a4);
}

void eastHook(void* a0, void* a1, const void* a2, const Vec3Raw* a3, const void* a4) {
    renderFace(g_eastOriginal, GlassFace::East, a0, a1, a2, a3, a4);
}

bool paneHook(void* a0, void* a1, const void* a2, const BlockPosRaw* a3, bool a4) {
    const PaneContext previous = g_paneContext;
    g_paneContext = {a0, a2, a3 ? *a3 : BlockPosRaw{}, true};
    const bool result = g_paneOriginal ? g_paneOriginal(a0, a1, a2, a3, a4) : false;
    g_paneContext = previous;
    return result;
}

bedrocktools::hooks::Handle installFaceHook(bedrocktools::memory::SignatureId id, void* detour, FaceFn* original) {
    const uintptr_t address = bedrocktools::memory::resolve(id);
    if (!address) return nullptr;
    return bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour, reinterpret_cast<void**>(original));
}

void requestChunkRebuild() {
    g_rebuildPending.store(true, std::memory_order_release);
}

bool rebuildRenderChunks(void* clientInstance) {
    if (!clientInstance || !g_setAllDirty) return false;

    void* levelRenderer = field<void*>(clientInstance, bedrocktools::sdk::offsets::ClientInstance::mLevelRenderer);
    if (!levelRenderer) return false;

    void* node = field<void*>(
        levelRenderer,
        bedrocktools::sdk::offsets::LevelRenderer::mRenderChunkCoordinators
            + bedrocktools::sdk::offsets::HashTable::mFirstNode
    );

    bool rebuilt = false;
    size_t visited = 0;
    while (node && visited++ < bedrocktools::sdk::offsets::RenderChunkCoordinator::MaxNodes) {
        void* next = field<void*>(node, bedrocktools::sdk::offsets::HashNode::mNext);
        void* coordinator = field<void*>(node, bedrocktools::sdk::offsets::HashNode::mValuePointer);
        if (coordinator) {
            g_setAllDirty(coordinator, true, false);
            rebuilt = true;
        }
        node = next;
    }

    return rebuilt;
}

}

void ConnectedGlassHandleClientInstanceUpdate(void* clientInstance) {
    if (!g_rebuildPending.load(std::memory_order_acquire)) return;
    if (rebuildRenderChunks(clientInstance)) {
        g_rebuildPending.store(false, std::memory_order_release);
    }
}

ConnectedGlassModule::ConnectedGlassModule()
    : Module(
          "Connected Glass",
          "Connects glass blocks and panes with configurable borders and colors."
      ) {
}

ConnectedGlassModule::~ConnectedGlassModule() {
    g_enabled.store(false, std::memory_order_relaxed);
}

void ConnectedGlassModule::applySettings() {
    g_connectGlassBlocks.store(connectGlassBlocks, std::memory_order_relaxed);
    g_connectGlassPanes.store(connectGlassPanes, std::memory_order_relaxed);
    g_connectDifferentColors.store(connectDifferentColors, std::memory_order_relaxed);
    g_connectPanesToBlocks.store(connectPanesToBlocks, std::memory_order_relaxed);
    g_removeFlatBorders.store(removeFlatBorders, std::memory_order_relaxed);
    g_removeCornerBorders.store(removeCornerBorders, std::memory_order_relaxed);
    g_removeOuterVerticalBorders.store(removeOuterVerticalBorders, std::memory_order_relaxed);
    g_removeOuterHorizontalBorders.store(removeOuterHorizontalBorders, std::memory_order_relaxed);
    g_removeOuterTopBottomFaceBorders.store(removeOuterTopBottomFaceBorders, std::memory_order_relaxed);
    g_affectSideFaces.store(affectSideFaces, std::memory_order_relaxed);
    g_affectTopFace.store(affectTopFace, std::memory_order_relaxed);
    g_affectBottomFace.store(affectBottomFace, std::memory_order_relaxed);
    borderWidth = std::clamp(borderWidth, 0.0f, 7.5f);
    g_borderWidth.store(borderWidth, std::memory_order_relaxed);
}

void ConnectedGlassModule::installHooks() {
    if (m_hooked) return;

    g_getBlock = reinterpret_cast<BlockSourceGetBlockFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlockForTessellation));
    g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock)
    );
    g_textureCopyCtor = reinterpret_cast<TextureUVCopyCtorFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TextureUVCoordinateSetCopyCtor)
    );
    g_textureDtor = reinterpret_cast<TextureUVDtorFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TextureUVCoordinateSetDtor));
    g_setAllDirty = reinterpret_cast<SetAllDirtyFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderChunkCoordinatorSetAllDirty));

    if (!g_downHook) {
        g_downHook = installFaceHook(bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceDown, reinterpret_cast<void*>(downHook), &g_downOriginal);
    }
    if (!g_upHook) {
        g_upHook = installFaceHook(bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceUp, reinterpret_cast<void*>(upHook), &g_upOriginal);
    }
    if (!g_northHook) {
        g_northHook = installFaceHook(bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceNorth, reinterpret_cast<void*>(northHook), &g_northOriginal);
    }
    if (!g_southHook) {
        g_southHook = installFaceHook(bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceSouth, reinterpret_cast<void*>(southHook), &g_southOriginal);
    }
    if (!g_westHook) {
        g_westHook = installFaceHook(bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceWest, reinterpret_cast<void*>(westHook), &g_westOriginal);
    }
    if (!g_eastHook) {
        g_eastHook = installFaceHook(bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceEast, reinterpret_cast<void*>(eastHook), &g_eastOriginal);
    }
    if (!g_paneHook) {
        const uintptr_t paneAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockTessellatorTessellatePane);
        if (paneAddress) {
            g_paneHook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(paneAddress),
                reinterpret_cast<void*>(paneHook),
                reinterpret_cast<void**>(&g_paneOriginal)
            );
        }
    }

    m_hooked = g_getBlock
        && g_isSolidBlockingBlock
        && g_textureCopyCtor
        && g_textureDtor
        && g_setAllDirty
        && g_downHook
        && g_upHook
        && g_northHook
        && g_southHook
        && g_westHook
        && g_eastHook
        && g_paneHook;
}

void ConnectedGlassModule::onInit() {
    applySettings();
    installHooks();
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>([](auto& event) {
        ConnectedGlassHandleClientInstanceUpdate(event.clientInstance);
    });
}

void ConnectedGlassModule::onEnable() {
    applySettings();
    if (!m_hooked) installHooks();
    g_enabled.store(m_hooked, std::memory_order_release);
    if (m_hooked) requestChunkRebuild();
}

void ConnectedGlassModule::onDisable() {
    g_enabled.store(false, std::memory_order_release);
    if (m_hooked) requestChunkRebuild();
}

void ConnectedGlassModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    connectGlassBlocks = j.value("connectGlassBlocks", connectGlassBlocks);
    connectGlassPanes = j.value("connectGlassPanes", connectGlassPanes);
    connectDifferentColors = j.value("connectDifferentColors", connectDifferentColors);
    connectPanesToBlocks = j.value("connectPanesToBlocks", connectPanesToBlocks);
    removeFlatBorders = j.value("removeFlatBorders", removeFlatBorders);
    removeCornerBorders = j.value("removeCornerBorders", removeCornerBorders);
    removeOuterVerticalBorders = j.value("removeOuterVerticalBorders", removeOuterVerticalBorders);
    removeOuterHorizontalBorders = j.value("removeOuterHorizontalBorders", removeOuterHorizontalBorders);
    removeOuterTopBottomFaceBorders = j.value(
        "removeOuterTopBottomFaceBorders",
        removeOuterTopBottomFaceBorders
    );
    affectSideFaces = j.value("affectSideFaces", affectSideFaces);
    affectTopFace = j.value("affectTopFace", affectTopFace);
    affectBottomFace = j.value("affectBottomFace", affectBottomFace);
    borderWidth = j.value("borderWidth", borderWidth);
    applySettings();
    if (enabled && m_hooked) requestChunkRebuild();
}

void ConnectedGlassModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["connectGlassBlocks"] = connectGlassBlocks;
    j["connectGlassPanes"] = connectGlassPanes;
    j["connectDifferentColors"] = connectDifferentColors;
    j["connectPanesToBlocks"] = connectPanesToBlocks;
    j["removeFlatBorders"] = removeFlatBorders;
    j["removeCornerBorders"] = removeCornerBorders;
    j["removeOuterVerticalBorders"] = removeOuterVerticalBorders;
    j["removeOuterHorizontalBorders"] = removeOuterHorizontalBorders;
    j["removeOuterTopBottomFaceBorders"] = removeOuterTopBottomFaceBorders;
    j["affectSideFaces"] = affectSideFaces;
    j["affectTopFace"] = affectTopFace;
    j["affectBottomFace"] = affectBottomFace;
    j["borderWidth"] = borderWidth;
}
