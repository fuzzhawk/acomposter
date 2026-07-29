#include "Renderer.h"

#include "FontAtlas.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

namespace acm::gfx {
namespace {

// Screen pixels in, clip space out. The projection is a constant buffer rather
// than baked into the vertices so a resize costs one 64-byte upload.
constexpr const char* kVertexShaderSource = R"hlsl(
cbuffer Constants : register(b0)
{
    float4x4 projection;
};

struct VertexIn
{
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
    float4 colour   : COLOR0;
};

struct VertexOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 colour   : COLOR0;
};

VertexOut main(VertexIn input)
{
    VertexOut output;
    output.position = mul(projection, float4(input.position, 0.0f, 1.0f));
    output.uv = input.uv;
    output.colour = input.colour;
    return output;
}
)hlsl";

// One sample, one multiply. Untextured shapes sample the atlas's white block, so
// there is no branch and no second pipeline state.
constexpr const char* kPixelShaderSource = R"hlsl(
Texture2D    atlas   : register(t0);
SamplerState pointer : register(s0);

struct PixelIn
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 colour   : COLOR0;
};

float4 main(PixelIn input) : SV_TARGET
{
    return input.colour * atlas.Sample(pointer, input.uv);
}
)hlsl";

template <typename T>
void release(T*& object) {
    if (object) { object->Release(); object = nullptr; }
}

} // namespace

// ---------------------------------------------------------------------------

struct Renderer::Impl {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;

    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11InputLayout* inputLayout = nullptr;
    ID3D11Buffer* constantBuffer = nullptr;
    ID3D11Buffer* vertexBuffer = nullptr;
    ID3D11Buffer* indexBuffer = nullptr;
    std::size_t vertexCapacity = 0;
    std::size_t indexCapacity = 0;

    ID3D11BlendState* blendState = nullptr;
    ID3D11RasterizerState* rasteriserState = nullptr;
    ID3D11DepthStencilState* depthStencilState = nullptr;
    ID3D11SamplerState* sampler = nullptr;

    struct Texture {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        int width = 0;
        int height = 0;
        bool inUse = false;
    };
    // Index 0 is reserved so kNoTexture stays a falsy sentinel.
    std::vector<Texture> textures{ Texture{} };

    bool createDeviceObjects(std::string& error);
    void releaseDeviceObjects();
    bool createRenderTarget();
    bool ensureBufferCapacity(std::size_t vertexCount, std::size_t indexCount);
    void setRenderState(int width, int height);
};

// ---------------------------------------------------------------------------

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}

Renderer::~Renderer() { shutdown(); }

bool Renderer::initialise(void* windowHandle, int width, int height) {
    error_.clear();
    width_ = std::max(1, width);
    height_ = std::max(1, height);

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Width = static_cast<UINT>(width_);
    swapChainDesc.BufferDesc.Height = static_cast<UINT>(height_);
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // Leaving the refresh rate at zero lets DXGI use the display's own rate
    // rather than forcing a mode change.
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = static_cast<HWND>(windowHandle);
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    UINT flags = 0;
#ifdef ACOMPOSTER_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        // A 2D UI needs nothing beyond feature level 9; accepting it means
        // acomposter still starts inside a VM or over remote desktop.
        D3D_FEATURE_LEVEL_9_3,
    };
    D3D_FEATURE_LEVEL obtained{};

    HRESULT result = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        requested, static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
        &swapChainDesc, &impl_->swapChain, &impl_->device, &obtained, &impl_->context);

    if (FAILED(result)) {
        // Fall back to WARP so a machine with no usable GPU driver still runs,
        // slowly, rather than refusing to start.
        result = ::D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            requested, static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
            &swapChainDesc, &impl_->swapChain, &impl_->device, &obtained, &impl_->context);
    }

    if (FAILED(result)) {
        error_ = "could not create a Direct3D 11 device (0x"
               + std::to_string(static_cast<unsigned long>(result)) + ")";
        return false;
    }

    // DXGI's default Alt+Enter handling fights with our own window management.
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(impl_->device->QueryInterface(__uuidof(IDXGIDevice),
                                                reinterpret_cast<void**>(&dxgiDevice)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory),
                                             reinterpret_cast<void**>(&factory)))) {
                factory->MakeWindowAssociation(static_cast<HWND>(windowHandle),
                                               DXGI_MWA_NO_ALT_ENTER);
                factory->Release();
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }

    if (!impl_->createDeviceObjects(error_)) {
        shutdown();
        return false;
    }
    if (!impl_->createRenderTarget()) {
        error_ = "could not create the swap chain's render target";
        shutdown();
        return false;
    }

    device_ = impl_->device;
    return true;
}

void Renderer::shutdown() {
    if (!impl_) return;

    for (auto& texture : impl_->textures) {
        release(texture.view);
        release(texture.texture);
    }
    impl_->textures.assign(1, Impl::Texture{});

    impl_->releaseDeviceObjects();
    release(impl_->renderTarget);
    release(impl_->swapChain);
    release(impl_->context);
    release(impl_->device);
    device_ = nullptr;
}

// ---------------------------------------------------------------------------
// Device objects
// ---------------------------------------------------------------------------

bool Renderer::Impl::createDeviceObjects(std::string& error) {
    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    const UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;

    HRESULT result = ::D3DCompile(kVertexShaderSource, std::strlen(kVertexShaderSource),
                                  "acomposter.vs", nullptr, nullptr, "main", "vs_4_0_level_9_1",
                                  compileFlags, 0, &vertexBlob, &errorBlob);
    if (FAILED(result)) {
        error = "vertex shader would not compile";
        if (errorBlob) {
            error += ": ";
            error.append(static_cast<const char*>(errorBlob->GetBufferPointer()),
                         errorBlob->GetBufferSize());
            errorBlob->Release();
        }
        return false;
    }

    result = ::D3DCompile(kPixelShaderSource, std::strlen(kPixelShaderSource),
                          "acomposter.ps", nullptr, nullptr, "main", "ps_4_0_level_9_1",
                          compileFlags, 0, &pixelBlob, &errorBlob);
    if (FAILED(result)) {
        error = "pixel shader would not compile";
        if (errorBlob) {
            error += ": ";
            error.append(static_cast<const char*>(errorBlob->GetBufferPointer()),
                         errorBlob->GetBufferSize());
            errorBlob->Release();
        }
        vertexBlob->Release();
        return false;
    }

    device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                               nullptr, &vertexShader);
    device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(),
                              nullptr, &pixelShader);

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)),
                              vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                              &inputLayout);

    vertexBlob->Release();
    pixelBlob->Release();

    if (!vertexShader || !pixelShader || !inputLayout) {
        error = "could not create the shader pipeline";
        return false;
    }

    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth = sizeof(float) * 16;
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&constantDesc, nullptr, &constantBuffer);

    // Straight alpha in, premultiplied out for the alpha channel so layered
    // translucency (the glow passes) composites correctly.
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blendDesc, &blendState);

    D3D11_RASTERIZER_DESC rasteriserDesc{};
    rasteriserDesc.FillMode = D3D11_FILL_SOLID;
    rasteriserDesc.CullMode = D3D11_CULL_NONE;   // 2D geometry has no winding
    rasteriserDesc.ScissorEnable = TRUE;         // the draw list's clip stack
    rasteriserDesc.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rasteriserDesc, &rasteriserState);

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    device->CreateDepthStencilState(&depthDesc, &depthStencilState);

    D3D11_SAMPLER_DESC samplerDesc{};
    // Linear, because the influence field is a small texture stretched across
    // the metasurface; glyphs are drawn at integer positions so they stay sharp.
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    device->CreateSamplerState(&samplerDesc, &sampler);

    return blendState && rasteriserState && depthStencilState && sampler && constantBuffer;
}

void Renderer::Impl::releaseDeviceObjects() {
    release(sampler);
    release(depthStencilState);
    release(rasteriserState);
    release(blendState);
    release(indexBuffer);
    release(vertexBuffer);
    release(constantBuffer);
    release(inputLayout);
    release(pixelShader);
    release(vertexShader);
    vertexCapacity = indexCapacity = 0;
}

bool Renderer::Impl::createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backBuffer))))
        return false;

    const HRESULT result = device->CreateRenderTargetView(backBuffer, nullptr, &renderTarget);
    backBuffer->Release();
    return SUCCEEDED(result);
}

bool Renderer::Impl::ensureBufferCapacity(std::size_t vertexCount, std::size_t indexCount) {
    // Grow with headroom so a frame that creeps up in complexity does not
    // reallocate every time.
    if (vertexCount > vertexCapacity) {
        release(vertexBuffer);
        vertexCapacity = vertexCount + vertexCount / 2 + 4096;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(vertexCapacity * sizeof(Vertex));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &vertexBuffer))) {
            vertexCapacity = 0;
            return false;
        }
    }

    if (indexCount > indexCapacity) {
        release(indexBuffer);
        indexCapacity = indexCount + indexCount / 2 + 8192;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(indexCapacity * sizeof(std::uint32_t));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &indexBuffer))) {
            indexCapacity = 0;
            return false;
        }
    }

    return true;
}

void Renderer::Impl::setRenderState(int width, int height) {
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    // Orthographic, y down, origin top-left: the coordinate system the UI works
    // in, so nothing above has to think about clip space.
    const float left = 0.0f;
    const float right = static_cast<float>(width);
    const float top = 0.0f;
    const float bottom = static_cast<float>(height);

    const float projection[16] = {
        2.0f / (right - left),            0.0f,                            0.0f, 0.0f,
        0.0f,                             2.0f / (top - bottom),           0.0f, 0.0f,
        0.0f,                             0.0f,                            0.5f, 0.0f,
        (right + left) / (left - right),  (top + bottom) / (bottom - top), 0.5f, 1.0f,
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, projection, sizeof(projection));
        context->Unmap(constantBuffer, 0);
    }

    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;

    context->IASetInputLayout(inputLayout);
    context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context->VSSetShader(vertexShader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &constantBuffer);
    context->PSSetShader(pixelShader, nullptr, 0);
    context->PSSetSamplers(0, 1, &sampler);

    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState(blendState, blendFactor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depthStencilState, 0);
    context->RSSetState(rasteriserState);
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

void Renderer::resize(int width, int height) {
    if (!impl_ || !impl_->swapChain) return;

    width = std::max(1, width);
    height = std::max(1, height);
    if (width == width_ && height == height_) return;

    width_ = width;
    height_ = height;

    // The render target view must be released before the buffers can be resized.
    release(impl_->renderTarget);
    impl_->swapChain->ResizeBuffers(0, static_cast<UINT>(width_), static_cast<UINT>(height_),
                                    DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    impl_->createRenderTarget();
}

TextureId Renderer::createTexture(const std::uint32_t* pixels, int width, int height) {
    if (!impl_ || !impl_->device || width <= 0 || height <= 0) return kNoTexture;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels;
    initial.SysMemPitch = static_cast<UINT>(width) * 4;

    Impl::Texture texture;
    texture.width = width;
    texture.height = height;

    if (FAILED(impl_->device->CreateTexture2D(&desc, pixels ? &initial : nullptr, &texture.texture)))
        return kNoTexture;

    if (FAILED(impl_->device->CreateShaderResourceView(texture.texture, nullptr, &texture.view))) {
        release(texture.texture);
        return kNoTexture;
    }

    texture.inUse = true;

    // Reuse a slot freed by destroyTexture rather than growing forever; the
    // metasurface recreates its field texture whenever the panel is resized.
    for (std::size_t i = 1; i < impl_->textures.size(); ++i) {
        if (!impl_->textures[i].inUse) {
            impl_->textures[i] = texture;
            return static_cast<TextureId>(i);
        }
    }

    impl_->textures.push_back(texture);
    return static_cast<TextureId>(impl_->textures.size() - 1);
}

bool Renderer::updateTexture(TextureId id, const std::uint32_t* pixels, int width, int height) {
    if (!impl_ || id == kNoTexture || id >= impl_->textures.size()) return false;

    Impl::Texture& texture = impl_->textures[id];
    if (!texture.inUse || texture.width != width || texture.height != height) return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(impl_->context->Map(texture.texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;

    // The mapped pitch is not necessarily the row length, so copy row by row.
    auto* destination = static_cast<std::uint8_t*>(mapped.pData);
    for (int row = 0; row < height; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * mapped.RowPitch,
                    pixels + static_cast<std::size_t>(row) * static_cast<std::size_t>(width),
                    static_cast<std::size_t>(width) * 4);
    }

    impl_->context->Unmap(texture.texture, 0);
    return true;
}

void Renderer::destroyTexture(TextureId id) {
    if (!impl_ || id == kNoTexture || id >= impl_->textures.size()) return;

    Impl::Texture& texture = impl_->textures[id];
    release(texture.view);
    release(texture.texture);
    texture = Impl::Texture{};
}

TextureId Renderer::uploadFontAtlas(const FontAtlas& atlas) {
    if (!atlas.built()) return kNoTexture;
    return createTexture(atlas.pixels(), atlas.width(), atlas.height());
}

void Renderer::beginFrame(const Colour& clearColour) {
    if (!impl_ || !impl_->context || !impl_->renderTarget) return;

    impl_->context->OMSetRenderTargets(1, &impl_->renderTarget, nullptr);
    const float clear[4] = { clearColour.r, clearColour.g, clearColour.b, clearColour.a };
    impl_->context->ClearRenderTargetView(impl_->renderTarget, clear);
}

void Renderer::render(const DrawList& drawList) {
    if (!impl_ || !impl_->context) return;

    const auto& vertices = drawList.vertices();
    const auto& indices = drawList.indices();
    if (vertices.empty() || indices.empty()) return;

    if (!impl_->ensureBufferCapacity(vertices.size(), indices.size())) return;

    D3D11_MAPPED_SUBRESOURCE mappedVertices{};
    if (SUCCEEDED(impl_->context->Map(impl_->vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVertices))) {
        std::memcpy(mappedVertices.pData, vertices.data(), vertices.size() * sizeof(Vertex));
        impl_->context->Unmap(impl_->vertexBuffer, 0);
    }

    D3D11_MAPPED_SUBRESOURCE mappedIndices{};
    if (SUCCEEDED(impl_->context->Map(impl_->indexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedIndices))) {
        std::memcpy(mappedIndices.pData, indices.data(), indices.size() * sizeof(std::uint32_t));
        impl_->context->Unmap(impl_->indexBuffer, 0);
    }

    impl_->setRenderState(width_, height_);

    for (const DrawCommand& command : drawList.commands()) {
        if (command.indexCount == 0) continue;

        const D3D11_RECT scissor{
            static_cast<LONG>(command.clip.left()),
            static_cast<LONG>(command.clip.top()),
            static_cast<LONG>(command.clip.right()),
            static_cast<LONG>(command.clip.bottom()),
        };
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        impl_->context->RSSetScissorRects(1, &scissor);

        ID3D11ShaderResourceView* view = nullptr;
        if (command.texture != kNoTexture && command.texture < impl_->textures.size())
            view = impl_->textures[command.texture].view;
        impl_->context->PSSetShaderResources(0, 1, &view);

        impl_->context->DrawIndexed(command.indexCount, command.indexOffset, 0);
    }
}

void Renderer::endFrame(bool vsync) {
    if (!impl_ || !impl_->swapChain) return;

    const HRESULT result = impl_->swapChain->Present(vsync ? 1 : 0, 0);

    // A removed or reset device means the GPU went away underneath us - a driver
    // update, or a laptop switching graphics adapters.
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
        error_ = "the graphics device was reset; restart acomposter to recover";
}

} // namespace acm::gfx
