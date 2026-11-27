#include "AV.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <xaudio2.h>
#include <wrl/client.h>
#include <winrt/windows.applicationmodel.h>
#include <winrt/windows.storage.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using winrt::check_hresult;

namespace rondo {
namespace {

constexpr UINT TextureWidth = 512;
constexpr UINT TextureHeight = 256;

std::vector<uint8_t> LoadShader(const wchar_t* name) {
    auto location = winrt::Windows::ApplicationModel::Package::Current()
                        .InstalledLocation().Path();
    std::wstring path(location.c_str());
    path += L"\\Shaders\\";
    path += name;
    HANDLE file = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              OPEN_EXISTING, nullptr);
    if (file == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    // A HANDLE has no WinRT wrapper. Close it on both success and exceptions.
    struct FileCloser {
        HANDLE value;
        ~FileCloser() { CloseHandle(value); }
    } closer{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) winrt::throw_last_error();
    if (size.QuadPart <= 0 || size.QuadPart > 1024 * 1024)
        throw std::runtime_error("Invalid packaged shader size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
        winrt::throw_last_error();
    if (read != bytes.size()) throw std::runtime_error("Incomplete packaged shader");
    return bytes;
}

bool DeviceLost(HRESULT result) {
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET;
}

} // namespace

struct Video::Impl {
    ComPtr<IUnknown> window;
    UINT width = 1;
    UINT height = 1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> textureView;
    std::array<ComPtr<ID3D11Texture2D>, Video::OverlayCount> overlayTextures;
    std::array<ComPtr<ID3D11ShaderResourceView>, Video::OverlayCount> overlayViews;
    ComPtr<ID3D11BlendState> overlayBlend;
    std::array<std::vector<uint8_t>, Video::OverlayCount> overlayPixels;
    UINT overlayWidth = 0;
    UINT overlayHeight = 0;
    uint32_t overlayIndex = 0;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2d;
    ComPtr<ID2D1Bitmap1> d2dTarget;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<ID2D1SolidColorBrush> panelBrush;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IDWriteTextFormat> textFormat;

    void ReleaseSizeResources() {
        if (context) context->OMSetRenderTargets(0, nullptr, nullptr);
        if (d2d) d2d->SetTarget(nullptr);
        d2dTarget.Reset();
        renderTarget.Reset();
    }

    void CreateSizeResources() {
        ComPtr<ID3D11Texture2D> backBuffer;
        check_hresult(swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())));
        check_hresult(device->CreateRenderTargetView(backBuffer.Get(), nullptr,
                                                     renderTarget.GetAddressOf()));
        ComPtr<IDXGISurface> surface;
        check_hresult(backBuffer.As(&surface));
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        check_hresult(d2d->CreateBitmapFromDxgiSurface(surface.Get(), &props,
                                                      d2dTarget.GetAddressOf()));
        d2d->SetTarget(d2dTarget.Get());
        textFormat.Reset();
        const float fontSize = std::clamp(static_cast<float>(height) / 36.0f,
                                         18.0f, 30.0f);
        check_hresult(writeFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, fontSize, L"pt-BR", textFormat.GetAddressOf()));
        check_hresult(textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP));
    }

    void CreateOverlayResources() {
        for (auto& view : overlayViews) view.Reset();
        for (auto& texture : overlayTextures) texture.Reset();
        overlayBlend.Reset();
        if (!device) return;

        for (size_t i = 0; i < overlayPixels.size(); ++i) {
            if (overlayPixels[i].empty()) continue;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = overlayWidth;
            desc.Height = overlayHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial{};
            initial.pSysMem = overlayPixels[i].data();
            initial.SysMemPitch = overlayWidth * 4;
            check_hresult(device->CreateTexture2D(&desc, &initial, overlayTextures[i].GetAddressOf()));
            check_hresult(device->CreateShaderResourceView(overlayTextures[i].Get(), nullptr,
                                                           overlayViews[i].GetAddressOf()));
        }

        D3D11_BLEND_DESC blend{};
        auto& target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_ONE; // The PNG is decoded as premultiplied alpha.
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check_hresult(device->CreateBlendState(&blend, overlayBlend.GetAddressOf()));
    }

    void CreateDeviceResources() {
        ReleaseSizeResources();
        if (context) {
            context->ClearState();
            context->Flush();
        }
        d2d.Reset();
        d2dDevice.Reset();
        d2dFactory.Reset();
        textBrush.Reset();
        panelBrush.Reset();
        textFormat.Reset();
        writeFactory.Reset();
        textureView.Reset();
        texture.Reset();
        for (auto& view : overlayViews) view.Reset();
        for (auto& texture : overlayTextures) texture.Reset();
        overlayBlend.Reset();
        vertexShader.Reset();
        pixelShader.Reset();
        constants.Reset();
        sampler.Reset();
        rasterizer.Reset();
        swapChain.Reset();
        context.Reset();
        device.Reset();

        static constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL selected{};
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION, device.GetAddressOf(), &selected, context.GetAddressOf()));

        ComPtr<IDXGIDevice> dxgiDevice;
        check_hresult(device.As(&dxgiDevice));
        ComPtr<IDXGIAdapter> adapter;
        check_hresult(dxgiDevice->GetAdapter(adapter.GetAddressOf()));
        ComPtr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        check_hresult(factory->CreateSwapChainForCoreWindow(device.Get(), window.Get(),
            &desc, nullptr, swapChain.GetAddressOf()));
        // One queued GPU frame is enough for this 2D game and limits input latency.
        ComPtr<IDXGIDevice1> dxgiDevice1;
        check_hresult(device.As(&dxgiDevice1));
        check_hresult(dxgiDevice1->SetMaximumFrameLatency(1));

        const auto vs = LoadShader(L"FrameVS.cso");
        const auto ps = LoadShader(L"FramePS.cso");
        check_hresult(device->CreateVertexShader(vs.data(), vs.size(), nullptr,
                                                 vertexShader.GetAddressOf()));
        check_hresult(device->CreatePixelShader(ps.data(), ps.size(), nullptr,
                                                pixelShader.GetAddressOf()));
        D3D11_TEXTURE2D_DESC tex{};
        tex.Width = TextureWidth;
        tex.Height = TextureHeight;
        tex.MipLevels = 1;
        tex.ArraySize = 1;
        tex.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        tex.SampleDesc.Count = 1;
        tex.Usage = D3D11_USAGE_DYNAMIC;
        tex.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        tex.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check_hresult(device->CreateTexture2D(&tex, nullptr, texture.GetAddressOf()));
        check_hresult(device->CreateShaderResourceView(texture.Get(), nullptr,
                                                       textureView.GetAddressOf()));
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 16;
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check_hresult(device->CreateBuffer(&buffer, nullptr, constants.GetAddressOf()));
        D3D11_SAMPLER_DESC sample{};
        sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD = D3D11_FLOAT32_MAX;
        check_hresult(device->CreateSamplerState(&sample, sampler.GetAddressOf()));
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        check_hresult(device->CreateRasterizerState(&raster, rasterizer.GetAddressOf()));

        D2D1_FACTORY_OPTIONS options{};
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &options,
            reinterpret_cast<void**>(d2dFactory.GetAddressOf())));
        check_hresult(d2dFactory->CreateDevice(dxgiDevice.Get(), d2dDevice.GetAddressOf()));
        check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                    d2d.GetAddressOf()));
        d2d->SetDpi(96.0f, 96.0f);
        d2d->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        check_hresult(d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                                 textBrush.GetAddressOf()));
        check_hresult(d2d->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.85f),
                                                 panelBrush.GetAddressOf()));
        check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())));
        CreateSizeResources();
        CreateOverlayResources();
    }

    void DrawOverlay(const std::wstring& overlay) {
        if (overlay.empty()) return;
        const float margin = std::max(12.0f, static_cast<float>(height) / 36.0f);
        const float maxWidth = std::max(1.0f, static_cast<float>(width) - margin * 4);
        const float maxHeight = std::max(1.0f, static_cast<float>(height) - margin * 4);
        ComPtr<IDWriteTextLayout> layout;
        // Error text must not allocate an unbounded layout on the render thread.
        const UINT length = static_cast<UINT>(std::min<size_t>(overlay.size(), 8192));
        check_hresult(writeFactory->CreateTextLayout(overlay.data(), length, textFormat.Get(),
            maxWidth, maxHeight, layout.GetAddressOf()));
        DWRITE_TEXT_METRICS metrics{};
        check_hresult(layout->GetMetrics(&metrics));
        const float bottom = std::min(static_cast<float>(height) - margin,
                                     margin * 3 + metrics.height);
        d2d->BeginDraw();
        d2d->FillRectangle(D2D1::RectF(margin, margin,
                                      static_cast<float>(width) - margin, bottom),
                            panelBrush.Get());
        d2d->DrawTextLayout(D2D1::Point2F(margin * 2, margin * 2), layout.Get(),
                           textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        check_hresult(d2d->EndDraw());
    }

    void DrawArtworkOverlay() {
        auto* selectedOverlay = overlayViews[overlayIndex].Get();
        if (!selectedOverlay || !overlayBlend) return;
        const float values[] = {1.0f, 1.0f, 0.0f, 0.0f};
        context->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.Get());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &selectedOverlay);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->OMSetDepthStencilState(nullptr, 0);
        const float blendFactor[4] = {};
        context->OMSetBlendState(overlayBlend.Get(), blendFactor, 0xffffffff);
        context->Draw(4, 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ID3D11ShaderResourceView* emptyView = nullptr;
        context->PSSetShaderResources(0, 1, &emptyView);
    }

    void Draw(const uint32_t* argb, int frameWidth, int frameHeight, int stride,
              const std::wstring& overlay) {
        static constexpr float black[] = {0, 0, 0, 1};
        context->OMSetRenderTargets(1, renderTarget.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(renderTarget.Get(), black);
        if (argb) {
            if (frameWidth < 1 || frameHeight < 1 || frameWidth > static_cast<int>(TextureWidth)
                || frameHeight > static_cast<int>(TextureHeight) || stride < frameWidth)
                throw std::invalid_argument("Frame exceeds UWP video texture");
            // Unbind before mapping the next frame into this dynamic texture.
            ID3D11ShaderResourceView* emptyView = nullptr;
            context->PSSetShaderResources(0, 1, &emptyView);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(context->Map(texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            auto* destination = static_cast<uint8_t*>(mapped.pData);
            for (int row = 0; row < frameHeight; ++row)
                std::memcpy(destination + static_cast<size_t>(row) * mapped.RowPitch,
                            argb + static_cast<size_t>(row) * stride,
                            static_cast<size_t>(frameWidth) * sizeof(uint32_t));
            context->Unmap(texture.Get(), 0);
            const float values[] = {static_cast<float>(frameWidth) / TextureWidth,
                                    static_cast<float>(frameHeight) / TextureHeight, 0, 0};
            context->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
            // The second bezel has a narrower transparent opening. Fit the
            // 4:3 game viewport inside each artwork's aperture, preserving the
            // game's aspect ratio for both themes.
            const float apertureLeft = overlayIndex == 0 ? 171.0f : 244.0f;
            const float apertureWidth = overlayIndex == 0 ? 1024.0f : 878.0f;
            const float apertureLeftOnScreen = apertureLeft * static_cast<float>(width) / 1366.0f;
            const float apertureWidthOnScreen = apertureWidth * static_cast<float>(width) / 1366.0f;
            const float apertureHeightOnScreen = static_cast<float>(height);
            const float viewWidth = std::min(apertureWidthOnScreen,
                                            apertureHeightOnScreen * 4.0f / 3.0f);
            const float viewHeight = viewWidth * 3.0f / 4.0f;
            D3D11_VIEWPORT viewport{};
            viewport.TopLeftX = apertureLeftOnScreen + (apertureWidthOnScreen - viewWidth) / 2;
            viewport.TopLeftY = (apertureHeightOnScreen - viewHeight) / 2;
            viewport.Width = viewWidth;
            viewport.Height = viewHeight;
            viewport.MaxDepth = 1;
            context->RSSetViewports(1, &viewport);
            context->RSSetState(rasterizer.Get());
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context->VSSetShader(vertexShader.Get(), nullptr, 0);
            context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
            // Direct2D shares the immediate context. Bind all graphics stages
            // used by this draw instead of inheriting its previous state.
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->PSSetShader(pixelShader.Get(), nullptr, 0);
            context->PSSetShaderResources(0, 1, textureView.GetAddressOf());
            context->PSSetSamplers(0, 1, sampler.GetAddressOf());
            context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            context->OMSetDepthStencilState(nullptr, 0);
            context->Draw(4, 0);
        }
        DrawArtworkOverlay();
        DrawOverlay(overlay);
        // DXGI performs the display pacing. The core separately maintains its
        // PC Engine clock so a 120 Hz dashboard does not double game speed.
        check_hresult(swapChain->Present(1, 0));
    }
};

Video::Video() : impl_(std::make_unique<Impl>()) {}
Video::~Video() = default;

void Video::Initialize(IUnknown* coreWindow, uint32_t width, uint32_t height) {
    if (!coreWindow) throw std::invalid_argument("CoreWindow is required");
    impl_->window = coreWindow;
    impl_->width = std::clamp(width, 1u, 1920u);
    impl_->height = std::clamp(height, 1u, 1080u);
    impl_->CreateDeviceResources();
}

void Video::Resize(uint32_t width, uint32_t height) {
    width = std::clamp(width, 1u, 1920u);
    height = std::clamp(height, 1u, 1080u);
    if (impl_->width == width && impl_->height == height) return;
    impl_->width = width;
    impl_->height = height;
    if (!impl_->swapChain) return;
    impl_->ReleaseSizeResources();
    impl_->context->Flush();
    const HRESULT resized = impl_->swapChain->ResizeBuffers(2, width, height,
                                                           DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (DeviceLost(resized)) {
        impl_->CreateDeviceResources();
        return;
    }
    check_hresult(resized);
    impl_->CreateSizeResources();
}

void Video::SetOverlays(const uint8_t* firstBgra, const uint8_t* secondBgra,
                        uint32_t width, uint32_t height) {
    if (!firstBgra || !secondBgra || width != 1366 || height != 768)
        throw std::invalid_argument("Gameplay overlay must be a 1366x768 BGRA image");
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    impl_->overlayPixels[0].assign(firstBgra, firstBgra + bytes);
    impl_->overlayPixels[1].assign(secondBgra, secondBgra + bytes);
    impl_->overlayWidth = width;
    impl_->overlayHeight = height;
    impl_->CreateOverlayResources();
}

void Video::SelectOverlay(uint32_t index) {
    if (index >= OverlayCount) throw std::out_of_range("Invalid gameplay overlay index");
    impl_->overlayIndex = index;
}

void Video::Present(const uint32_t* argb, int width, int height, int stridePixels,
                    const std::wstring& overlay) {
    if (!impl_->swapChain) throw std::logic_error("Video was not initialized");
    try {
        impl_->Draw(argb, width, height, stridePixels, overlay);
    } catch (const winrt::hresult_error& error) {
        if (!DeviceLost(error.code()) && error.code() != D2DERR_RECREATE_TARGET) throw;
        // Recreate once and let a persistent failure reach the app's status UI.
        impl_->CreateDeviceResources();
    }
}

void Video::Trim() {
    if (!impl_->device) return;
    impl_->context->ClearState();
    impl_->context->Flush();
    ComPtr<IDXGIDevice3> dxgiDevice;
    if (SUCCEEDED(impl_->device.As(&dxgiDevice))) dxgiDevice->Trim();
}

struct Audio::Impl final : IXAudio2VoiceCallback, IXAudio2EngineCallback {
    struct Slot {
        std::array<int16_t, Audio::FramesPerBuffer * Audio::Channels> pcm{};
        std::atomic<bool> inUse{false};
    };
    std::array<Slot, Audio::BufferCount> slots;
    ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice* mastering = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    std::atomic<bool> failed{false};
    bool suspended = false;
    bool engineCallbacksRegistered = false;

    ~Impl() { Shutdown(); }

    void Shutdown() {
        // DestroyVoice waits for outstanding callbacks; the slots stay alive.
        if (source) {
            source->DestroyVoice();
            source = nullptr;
        }
        if (mastering) {
            mastering->DestroyVoice();
            mastering = nullptr;
        }
        if (engine) {
            if (engineCallbacksRegistered) engine->UnregisterForCallbacks(this);
            engineCallbacksRegistered = false;
            engine->StopEngine();
            engine.Reset();
        }
        for (auto& slot : slots) slot.inUse.store(false, std::memory_order_release);
        suspended = false;
    }

    HRESULT CreateSource() {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = Audio::Channels;
        format.nSamplesPerSec = Audio::SampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        HRESULT result = engine->CreateSourceVoice(&source, &format, 0,
                                                   XAUDIO2_DEFAULT_FREQ_RATIO, this);
        if (FAILED(result)) return result;
        result = source->Start();
        if (FAILED(result)) {
            source->DestroyVoice();
            source = nullptr;
        }
        return result;
    }

    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override {
        if (context) static_cast<Slot*>(context)->inUse.store(false, std::memory_order_release);
    }
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {
        failed.store(true, std::memory_order_release);
    }
    void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT) noexcept override {
        failed.store(true, std::memory_order_release);
    }
};

Audio::Audio() : impl_(std::make_unique<Impl>()) {}
Audio::~Audio() = default;

void Audio::Initialize() {
    if (Ready()) return;
    // A missing/disconnected endpoint must not stop video or the game loop.
    // Clean every partial initialization so a later Initialize can retry.
    impl_->Shutdown();
    impl_->failed.store(false, std::memory_order_release);
    HRESULT result = XAudio2Create(impl_->engine.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(result)) {
        result = impl_->engine->RegisterForCallbacks(impl_.get());
        impl_->engineCallbacksRegistered = SUCCEEDED(result);
    }
    if (SUCCEEDED(result))
        result = impl_->engine->CreateMasteringVoice(&impl_->mastering, Channels, SampleRate,
            0, nullptr, nullptr, AudioCategory_GameEffects);
    if (SUCCEEDED(result)) result = impl_->CreateSource();
    if (FAILED(result)) {
        impl_->Shutdown();
        impl_->failed.store(true, std::memory_order_release);
        return;
    }
}

bool Audio::Submit(const int16_t* interleaved, uint32_t frames) {
    if (!Ready() || impl_->suspended || !interleaved || frames == 0 || frames > FramesPerBuffer)
        return false;
    for (auto& slot : impl_->slots) {
        bool available = false;
        if (!slot.inUse.compare_exchange_strong(available, true, std::memory_order_acq_rel))
            continue;
        const size_t samples = static_cast<size_t>(frames) * Channels;
        std::memcpy(slot.pcm.data(), interleaved, samples * sizeof(int16_t));
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(samples * sizeof(int16_t));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(slot.pcm.data());
        buffer.pContext = &slot;
        const HRESULT result = impl_->source->SubmitSourceBuffer(&buffer);
        if (FAILED(result)) {
            slot.inUse.store(false, std::memory_order_release);
            impl_->failed.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }
    return false;
}

void Audio::Reset() {
    if (impl_->failed.load(std::memory_order_acquire)) {
        Initialize();
        return;
    }
    if (!impl_->engine || !impl_->mastering) return;
    if (impl_->source) {
        impl_->source->DestroyVoice();
        impl_->source = nullptr;
    }
    // Flushing a voice alone can leave its current buffer in flight. Recreating
    // it guarantees old PCM is no longer read before the slot flags are reset.
    for (auto& slot : impl_->slots) slot.inUse.store(false, std::memory_order_release);
    const HRESULT result = impl_->CreateSource();
    if (FAILED(result)) impl_->failed.store(true, std::memory_order_release);
}

void Audio::Suspend() {
    if (!impl_->engine || impl_->suspended) return;
    impl_->engine->StopEngine();
    impl_->suspended = true;
}

void Audio::Resume() {
    if (!impl_->engine || !impl_->suspended) return;
    const HRESULT result = impl_->engine->StartEngine();
    if (FAILED(result)) impl_->failed.store(true, std::memory_order_release);
    else impl_->suspended = false;
}

bool Audio::Ready() const {
    return impl_->source && !impl_->failed.load(std::memory_order_acquire);
}

} // namespace rondo
