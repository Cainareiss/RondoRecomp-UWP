#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct IUnknown;

namespace rondo {

// Use these objects from the CoreWindow thread. Audio callbacks only touch
// atomic slot flags; all queued PCM storage belongs to Audio until completion.
class Video final {
public:
    static constexpr uint32_t OverlayCount = 2;
    Video();
    ~Video();
    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    void Initialize(IUnknown* coreWindow, uint32_t width, uint32_t height);
    void Resize(uint32_t width, uint32_t height);
    // Pixels are BGRA8 with premultiplied alpha. The transparent center is
    // drawn over the 4:3 game viewport, while the opaque side art fills 16:9.
    void SetOverlays(const uint8_t* firstBgra, const uint8_t* secondBgra,
                     uint32_t width, uint32_t height);
    void SelectOverlay(uint32_t index);
    // Pixels are packed 0xAARRGGBB (BGRA bytes on little endian x64).
    // A null frame draws a black background with the supplied overlay.
    void Present(const uint32_t* argb, int width, int height, int stridePixels,
                 const std::wstring& overlay);
    void Trim();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Audio final {
public:
    static constexpr uint32_t SampleRate = 44100;
    static constexpr uint32_t Channels = 2;
    static constexpr uint32_t FramesPerBuffer = 1024;
    static constexpr uint32_t BufferCount = 4;

    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // Endpoint/device errors disable audio (Ready() == false) without throwing.
    // Initialize or Reset may be called later to retry.
    void Initialize();
    // Copies stereo PCM16 before returning. False means unavailable, queue full,
    // or an oversized block; the caller must not grow an unbounded backlog.
    bool Submit(const int16_t* interleaved, uint32_t frames);
    void Reset();
    void Suspend();
    void Resume();
    bool Ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rondo
