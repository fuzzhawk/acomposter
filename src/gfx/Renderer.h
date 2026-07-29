// Direct3D 11 backend.
//
// Deliberately small: one vertex shader, one pixel shader, one blend state, and
// a pair of dynamic buffers that the whole frame is uploaded into. All the
// expressiveness lives in the draw list; this only replays it.
//
// D3D11 rather than D3D12 or Vulkan because it is present on every Windows 10
// installation without a runtime to install, and because a 2D UI has nothing to
// gain from an explicit API.
#pragma once

#include "DrawList.h"
#include "Geometry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acm::gfx {

class FontAtlas;

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // `windowHandle` is an HWND. Returns false and fills errorText() on failure.
    bool initialise(void* windowHandle, int width, int height);
    void shutdown();
    bool ready() const noexcept { return device_ != nullptr; }
    const std::string& errorText() const noexcept { return error_; }

    void resize(int width, int height);
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    // -- textures ----------------------------------------------------------
    // RGBA8, tightly packed. Returns kNoTexture on failure.
    TextureId createTexture(const std::uint32_t* pixels, int width, int height);
    // Replaces the contents of a texture created with the same dimensions. Used
    // by the metasurface's influence field, which is regenerated as snapshots
    // move.
    bool updateTexture(TextureId texture, const std::uint32_t* pixels, int width, int height);
    void destroyTexture(TextureId texture);

    // Uploads the atlas and wires it up as the draw list's default texture.
    TextureId uploadFontAtlas(const FontAtlas& atlas);

    // -- frame -------------------------------------------------------------
    void beginFrame(const Colour& clearColour);
    void render(const DrawList& drawList);
    // `vsync` off is worth having when a machine cannot hold the frame rate:
    // tearing is preferable to stutter when you are performing.
    void endFrame(bool vsync);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Mirrored here so the header stays free of D3D types.
    void* device_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    std::string error_;
};

} // namespace acm::gfx
