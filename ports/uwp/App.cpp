// Native UWP entry point. Keep the machine and all save I/O on the view thread;
// XAudio2 only consumes copies of already mixed PCM held by Audio.
#include "AV.h"
#include "CoreSession.h"

#include <windows.h>
#include <winrt/windows.applicationmodel.h>
#include <winrt/windows.applicationmodel.activation.h>
#include <winrt/windows.applicationmodel.core.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.collections.h>
#include <winrt/windows.gaming.input.h>
#include <winrt/windows.graphics.imaging.h>
#include <winrt/windows.storage.h>
#include <winrt/windows.storage.pickers.h>
#include <winrt/windows.system.h>
#include <winrt/windows.ui.core.h>
#include <winrt/windows.ui.viewmanagement.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Storage;
using namespace Windows::System;
using namespace Windows::UI::Core;
using namespace Windows::UI::ViewManagement;

namespace {
using Clock = std::chrono::steady_clock;
constexpr double FrameSeconds = 455.0 * 263.0 / 7159090.0;
// Pad bits are part of the portable core's public input contract.
constexpr uint8_t PadI = 1, PadII = 2, PadSelect = 4, PadRun = 8;
constexpr uint8_t PadUp = 16, PadRight = 32, PadDown = 64, PadLeft = 128;

struct SessionDeleter {
    void operator()(RondoSession* p) const { rondo_session_destroy(p); }
};
using Session = std::unique_ptr<RondoSession, SessionDeleter>;

struct Input {
    uint8_t pad = 0, gamepadPad = 0;
    bool pause = false, accept = false, save = false, load = false, import = false, overlayNext = false;
    bool connected = false;
    bool gamepadConnected = false;
};

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() { return *this; }

    void Initialize(CoreApplicationView const& view) {
        m_activated = view.Activated([this](auto const&, auto const&) {
            m_window.Activate();
        });
        m_view = view;
        m_suspending = CoreApplication::Suspending([this](auto const&, SuspendingEventArgs const& args) {
            auto deferral = args.SuspendingOperation().GetDeferral();
            m_paused = true;
            StopVibration();
            m_audio.Suspend();
            FlushBram();
            m_video.Trim();
            deferral.Complete();
        });
        m_resuming = CoreApplication::Resuming([this](auto const&, auto const&) {
            // Resuming can arrive off the view thread. Resource/core access is
            // deferred to Run, which owns those objects.
            m_resumeRequested.store(true, std::memory_order_release);
        });
    }

    void SetWindow(CoreWindow const& window) {
        m_window = window;
        m_navigation = SystemNavigationManager::GetForCurrentView();
        // UWP maps an unhandled gamepad B press to system BackRequested. This
        // game has no page stack; consume Back so B never closes the app.
        m_backToken = m_navigation.BackRequested([this](auto const&, BackRequestedEventArgs const& args) {
            args.Handled(true);
        });
        m_closedToken = window.Closed([this](auto const&, auto const&) { m_closed = true; });
        m_visibilityToken = window.VisibilityChanged([this](auto const&, VisibilityChangedEventArgs const& args) {
            m_visible = args.Visible();
            if (!m_visible) Pause();
        });
        m_activationToken = window.Activated([this](auto const&, WindowActivatedEventArgs const& args) {
            if (args.WindowActivationState() == CoreWindowActivationState::Deactivated) Pause();
        });
        m_sizeToken = window.SizeChanged([this](auto const&, auto const&) { Resize(); });
        m_keyDownToken = window.KeyDown([this](auto const&, KeyEventArgs const& args) {
            auto k = static_cast<unsigned>(args.VirtualKey());
            const auto key = args.VirtualKey();
            if (key == VirtualKey::Z || key == VirtualKey::X || key == VirtualKey::I ||
                key == VirtualKey::F5 || key == VirtualKey::F8 || key == VirtualKey::O || key == VirtualKey::Enter ||
                key == VirtualKey::Escape || key == VirtualKey::Shift ||
                key == VirtualKey::Up || key == VirtualKey::Down ||
                key == VirtualKey::Left || key == VirtualKey::Right) m_keyboardUsed = true;
            if (k < m_keys.size()) m_keys[k] = true;
        });
        m_keyUpToken = window.KeyUp([this](auto const&, KeyEventArgs const& args) {
            auto k = static_cast<unsigned>(args.VirtualKey());
            if (k < m_keys.size()) m_keys[k] = false;
        });
        ApplicationView::PreferredLaunchWindowingMode(ApplicationViewWindowingMode::FullScreen);
        ApplicationView::GetForCurrentView().TryEnterFullScreenMode();
        const auto bounds = window.Bounds();
        m_video.Initialize(reinterpret_cast<::IUnknown*>(get_abi(window)),
                           Dimension(bounds.Width, 1920), Dimension(bounds.Height, 1080));
        m_audio.Initialize(); // An absent audio device is supported; video still runs.
    }

    void Load(hstring const&) {
        LoadOverlaySelection();
        PrepareStorage();
        LoadOverlayImage();
    }

    void Run() {
        auto previous = Clock::now();
        double accumulator = 0;
        while (!m_closed) {
            m_window.Dispatcher().ProcessEvents(m_visible ? CoreProcessEventsOption::ProcessAllIfPresent
                                                         : CoreProcessEventsOption::ProcessOneAndAllPending);
            if (m_closed) break;
            if (m_resumeRequested.exchange(false, std::memory_order_acq_rel)) {
                m_audio.Resume();
                Pause();
            }
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - previous).count();
            previous = now;
            const auto input = ReadInput();
            const auto pressed = Rising(input, m_previousInput);
            const auto gamepadPressed = static_cast<uint8_t>(input.gamepadPad & ~m_previousInput.gamepadPad);
            const bool lostGamepad = m_previousInput.gamepadConnected && !input.gamepadConnected;
            m_previousInput = input;
            UpdateVibration(input, gamepadPressed, now);

            if (!m_busy) {
                if (pressed.pause && Active()) {
                    if (m_paused) m_paused = false;
                    else Pause();
                }
                else if (m_paused || !Active()) {
                    if (pressed.overlayNext && m_overlayReady) SelectNextOverlay();
                    else if (pressed.import) ImportChd();
                    else if (pressed.accept && Active() && input.connected) m_paused = false;
                    else if (pressed.accept && !Active()) ReloadDisc();
                    else if (pressed.save && Active()) SaveState();
                    else if (pressed.load && Active()) LoadState();
                }
            }
            if (Active() && (lostGamepad || !input.connected) && !m_paused) {
                Pause();
                Status(L"Controle desconectado. Reconecte e pressione A para continuar.");
            }
            CheckMemory(now);

            if (m_visible && !m_busy && Active() && !m_paused) {
                // Preserve the PCE's 59.826 Hz on 60/120 Hz displays; never tie
                // game speed to Present count or accumulate an unbounded backlog.
                accumulator = std::min(accumulator + elapsed, FrameSeconds * 3);
                while (accumulator >= FrameSeconds) {
                    if (!rondo_session_step(m_session.get(), input.pad)) {
                        Pause();
                        Status(L"O núcleo interrompeu a execução.");
                        break;
                    }
                    std::array<int16_t, 2048> samples{};
                    const int frames = rondo_session_audio(m_session.get(), samples.data(), 1024);
                    if (frames > 0) m_audio.Submit(samples.data(), static_cast<uint32_t>(frames));
                    accumulator -= FrameSeconds;
                }
                if (now - m_lastBramFlush >= std::chrono::seconds(5)) FlushBram();
            } else accumulator = 0;

            if (m_visible) {
                int width = 0, height = 0, stride = 512;
                auto pixels = Active() ? rondo_session_framebuffer(m_session.get(), &width, &height, &stride) : nullptr;
                if (width <= 0 || height <= 0) pixels = nullptr;
                m_video.Present(pixels, width, height, stride, Overlay());
            } else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        FlushBram();
        StopVibration();
        m_audio.Reset();
    }

    void Uninitialize() {
        StopVibration();
        if (m_navigation) m_navigation.BackRequested(m_backToken);
        CoreApplication::Suspending(m_suspending);
        CoreApplication::Resuming(m_resuming);
        m_view.Activated(m_activated);
        m_window.Closed(m_closedToken);
        m_window.VisibilityChanged(m_visibilityToken);
        m_window.Activated(m_activationToken);
        m_window.SizeChanged(m_sizeToken);
        m_window.KeyDown(m_keyDownToken);
        m_window.KeyUp(m_keyUpToken);
    }

private:
    static uint32_t Dimension(float v, uint32_t maximum) {
        return static_cast<uint32_t>(std::clamp(v, 1.0f, static_cast<float>(maximum)));
    }
    void Resize() {
        auto bounds = m_window.Bounds();
        m_video.Resize(Dimension(bounds.Width, 1920), Dimension(bounds.Height, 1080));
    }
    bool Active() const { return m_session && rondo_session_active(m_session.get()); }
    void Status(std::wstring text) {
        m_status = std::move(text);
        m_statusUntil = Clock::now() + std::chrono::seconds(6);
    }
    void Pause() {
        m_paused = true;
        StopVibration();
        m_keys.fill(false);
        m_audio.Reset();
        if (m_session) rondo_session_discard_audio(m_session.get());
    }
    bool FlushBram() {
        m_lastBramFlush = Clock::now();
        if (!Active()) return true;
        char error[512]{};
        if (!rondo_session_flush_bram(m_session.get(), error, sizeof error)) {
            Status(to_hstring(error).c_str());
            return false;
        }
        return true;
    }
    void SaveState() {
        char error[512]{};
        Status(rondo_session_save(m_session.get(), error, sizeof error)
               ? L"Estado salvo no slot 1." : std::wstring(to_hstring(error)));
    }
    void LoadState() {
        char error[512]{};
        if (rondo_session_load(m_session.get(), error, sizeof error)) {
            m_audio.Reset();
            Status(L"Estado carregado. A para continuar.");
        } else Status(to_hstring(error).c_str());
    }

    fire_and_forget PrepareStorage() {
        auto lifetime = get_strong();
        m_busy = true;
        StorageFolder game{nullptr}, saves{nullptr};
        std::wstring failure;
        try {
            auto local = ApplicationData::Current().LocalFolder();
            game = co_await local.CreateFolderAsync(L"game", CreationCollisionOption::OpenIfExists);
            saves = co_await local.CreateFolderAsync(L"saves", CreationCollisionOption::OpenIfExists);
        } catch (hresult_error const& e) {
            failure = L"Não foi possível preparar o armazenamento: " + std::wstring(e.message());
        }
        co_await resume_foreground(m_window.Dispatcher());
        if (m_closed) co_return;
        if (failure.empty()) {
            m_gameFolder = game;
            m_saveFolder = saves;
            m_busy = false;
            ReloadDisc();
        } else {
            Status(failure);
            m_busy = false;
        }
    }

    void LoadOverlaySelection() {
        try {
            const auto values = ApplicationData::Current().LocalSettings().Values();
            if (values.HasKey(L"gameplayOverlay")) {
                const auto selected = unbox_value<int32_t>(values.Lookup(L"gameplayOverlay"));
                if (selected >= 0 && selected < static_cast<int32_t>(rondo::Video::OverlayCount))
                    m_overlayIndex = static_cast<uint32_t>(selected);
            }
        } catch (...) {
            m_overlayIndex = 0;
        }
    }

    void SelectNextOverlay() {
        m_overlayIndex = (m_overlayIndex + 1) % rondo::Video::OverlayCount;
        m_video.SelectOverlay(m_overlayIndex);
        try {
            ApplicationData::Current().LocalSettings().Values().Insert(
                L"gameplayOverlay", box_value(static_cast<int32_t>(m_overlayIndex)));
        } catch (...) {
            // Switching the bezel remains available if settings storage is full.
        }
        Status(m_overlayIndex == 0 ? L"Moldura 1/2 selecionada." : L"Moldura 2/2 selecionada.");
    }

    fire_and_forget LoadOverlayImage() {
        auto lifetime = get_strong();
        std::wstring failure;
        std::array<std::vector<uint8_t>, rondo::Video::OverlayCount> pixels;
        uint32_t width = 0, height = 0;
        try {
            const std::array<const wchar_t*, rondo::Video::OverlayCount> assets = {
                L"ms-appx:///Assets/GameplayOverlay.png",
                L"ms-appx:///Assets/GameplayOverlay2.png"
            };
            for (size_t i = 0; i < assets.size(); ++i) {
                const auto file = co_await StorageFile::GetFileFromApplicationUriAsync(Uri{assets[i]});
                const auto stream = co_await file.OpenAsync(FileAccessMode::Read);
                const auto decoder = co_await BitmapDecoder::CreateAsync(stream);
                if (i == 0) {
                    width = decoder.PixelWidth();
                    height = decoder.PixelHeight();
                }
                if (decoder.PixelWidth() != width || decoder.PixelHeight() != height ||
                    width != 1366 || height != 768)
                    throw hresult_error(E_INVALIDARG, L"As molduras precisam ter 1366 x 768 pixels.");
                const auto provider = co_await decoder.GetPixelDataAsync(
                    BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, BitmapTransform{},
                    ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage);
                auto decoded = provider.DetachPixelData();
                const size_t expected = static_cast<size_t>(width) * height * 4;
                if (decoded.size() != expected)
                    throw hresult_error(E_INVALIDARG, L"Uma moldura PNG tem dados incompletos.");
                pixels[i].assign(decoded.begin(), decoded.end());
            }
        } catch (hresult_error const& error) {
            failure = L"A moldura visual não pôde ser carregada: " + std::wstring(error.message());
        } catch (...) {
            failure = L"A moldura visual não pôde ser carregada.";
        }
        co_await resume_foreground(m_window.Dispatcher());
        if (m_closed) co_return;
        if (!failure.empty()) {
            Status(std::move(failure));
            co_return;
        }
        try {
            m_video.SetOverlays(pixels[0].data(), pixels[1].data(), width, height);
            m_video.SelectOverlay(m_overlayIndex);
            m_overlayReady = true;
        } catch (std::exception const&) {
            Status(L"As molduras visuais não puderam ser enviadas à GPU.");
        }
    }

    fire_and_forget ReloadDisc() {
        auto lifetime = get_strong();
        if (!m_gameFolder || !m_saveFolder || m_busy) co_return;
        m_busy = true;
        Pause();
        std::wstring failure;
        try {
            const auto files = co_await m_gameFolder.GetFilesAsync();
            co_await resume_foreground(m_window.Dispatcher());
            if (m_closed) co_return;
            std::vector<StorageFile> discs;
            for (auto const& file : files) {
                auto extension = std::wstring(file.FileType());
                std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                if (extension == L".chd" || extension == L".cue") discs.push_back(file);
            }
            std::sort(discs.begin(), discs.end(), [](auto const& a, auto const& b) { return a.Name() < b.Name(); });
            auto values = ApplicationData::Current().LocalSettings().Values();
            if (values.HasKey(L"disc")) {
                const auto selected = unbox_value<hstring>(values.Lookup(L"disc"));
                const auto it = std::find_if(discs.begin(), discs.end(), [&](auto const& d) { return d.Name() == selected; });
                if (it != discs.end()) std::rotate(discs.begin(), it, it + 1);
            }
            if (discs.empty()) {
                Status(L"Copie seu CHD ou CUE e as faixas para LocalState/game. X importa um CHD pelo seletor do sistema.");
            } else {
                if (!FlushBram()) {
                    m_busy = false;
                    co_return;
                }
                m_session.reset();
                m_session.reset(rondo_session_create());
                char error[512]{};
                if (!m_session) Status(L"Memória insuficiente para iniciar o núcleo.");
                else if (!rondo_session_open(m_session.get(), to_string(discs.front().Path()).c_str(),
                                             to_string(m_saveFolder.Path()).c_str(), error, sizeof error)) {
                    Status(L"Falha ao abrir o disco: " + std::wstring(to_hstring(error)));
                } else Status(L"Disco carregado: " + std::wstring(discs.front().Name()) + L". A para jogar.");
            }
        } catch (hresult_error const& e) {
            failure = L"Erro no armazenamento: " + std::wstring(e.message());
        }
        co_await resume_foreground(m_window.Dispatcher());
        if (m_closed) co_return;
        if (!failure.empty()) Status(failure);
        m_busy = false;
    }

    fire_and_forget ImportChd() {
        auto lifetime = get_strong();
        if (m_busy || !m_gameFolder) co_return;
        m_busy = true;
        Pause();
        std::wstring failure;
        try {
            Pickers::FileOpenPicker picker;
            picker.FileTypeFilter().Append(L".chd");
            picker.SuggestedStartLocation(Pickers::PickerLocationId::ComputerFolder);
            const auto source = co_await picker.PickSingleFileAsync();
            co_await resume_foreground(m_window.Dispatcher());
            if (m_closed) co_return;
            if (source) {
                Status(L"Importando o CHD para o armazenamento do aplicativo…");
                // GenerateUniqueName keeps the mounted disc intact until copying
                // finishes. A brokered file outside the sandbox is never passed to fopen.
                const auto copy = co_await source.CopyAsync(m_gameFolder, L"disc.chd", NameCollisionOption::GenerateUniqueName);
                co_await resume_foreground(m_window.Dispatcher());
                if (m_closed) co_return;
                ApplicationData::Current().LocalSettings().Values().Insert(L"disc", box_value(copy.Name()));
                m_busy = false;
                ReloadDisc();
                co_return;
            }
        } catch (hresult_error const& e) {
            failure = L"Seletor indisponível ou importação falhou: " + std::wstring(e.message()) +
                      L". Use o Device Portal para copiar os arquivos para LocalState/game.";
        }
        co_await resume_foreground(m_window.Dispatcher());
        if (m_closed) co_return;
        if (!failure.empty()) Status(failure);
        m_busy = false;
    }

    bool Key(VirtualKey key) const { return m_keys[static_cast<unsigned>(key)]; }
    Input ReadInput() {
        Input input;
        try {
          auto pads = Gamepad::Gamepads();
          if (pads.Size()) {
            m_gamepad = pads.GetAt(0);
            auto reading = m_gamepad.GetCurrentReading();
            const auto has = [&](GamepadButtons b) { return (reading.Buttons & b) != GamepadButtons::None; };
            input.connected = true;
            input.gamepadConnected = true;
            if (has(GamepadButtons::A)) input.pad |= PadI;
            if (has(GamepadButtons::X)) input.pad |= PadII;
            if (has(GamepadButtons::Menu)) input.pad |= PadRun;
            if (has(GamepadButtons::LeftShoulder)) input.pad |= PadSelect;
            if (has(GamepadButtons::DPadUp) || reading.LeftThumbstickY > 0.25) input.pad |= PadUp;
            if (has(GamepadButtons::DPadDown) || reading.LeftThumbstickY < -0.25) input.pad |= PadDown;
            if (has(GamepadButtons::DPadLeft) || reading.LeftThumbstickX < -0.25) input.pad |= PadLeft;
            if (has(GamepadButtons::DPadRight) || reading.LeftThumbstickX > 0.25) input.pad |= PadRight;
            input.pause = has(GamepadButtons::View);
            input.accept = has(GamepadButtons::A);
            input.save = has(GamepadButtons::Y);
            input.load = has(GamepadButtons::B);
            input.import = has(GamepadButtons::X);
            input.overlayNext = has(GamepadButtons::RightShoulder);
            input.gamepadPad = input.pad;
          } else m_gamepad = nullptr;
        } catch (hresult_error const&) {
            // A controller can disappear between enumeration and reading.
            input = {};
            m_gamepad = nullptr;
        }
        input.connected |= m_keyboardUsed;
        if (Key(VirtualKey::Z)) input.pad |= PadI;
        if (Key(VirtualKey::X)) input.pad |= PadII;
        if (Key(VirtualKey::Enter)) input.pad |= PadRun;
        if (Key(VirtualKey::Shift)) input.pad |= PadSelect;
        if (Key(VirtualKey::Up)) input.pad |= PadUp;
        if (Key(VirtualKey::Down)) input.pad |= PadDown;
        if (Key(VirtualKey::Left)) input.pad |= PadLeft;
        if (Key(VirtualKey::Right)) input.pad |= PadRight;
        input.pause |= Key(VirtualKey::Escape);
        input.accept |= Key(VirtualKey::Enter);
        input.save |= Key(VirtualKey::F5);
        input.load |= Key(VirtualKey::F8);
        input.import |= Key(VirtualKey::I);
        input.overlayNext |= Key(VirtualKey::O);
        return input;
    }

    void SetVibration(float left, float right) {
        if (!m_gamepad) return;
        try {
            GamepadVibration vibration{};
            vibration.LeftMotor = left;
            vibration.RightMotor = right;
            m_gamepad.Vibration(vibration);
            m_vibrationActive = left > 0.0f || right > 0.0f;
        } catch (hresult_error const&) {
            m_vibrationActive = false;
        }
    }
    void StopVibration() {
        if (m_vibrationActive) SetVibration(0.0f, 0.0f);
        m_vibrationActive = false;
        m_vibrationUntil = {};
    }
    void UpdateVibration(Input const& input, uint8_t pressed, Clock::time_point now) {
        if (!input.gamepadConnected || m_paused || m_busy || !Active()) {
            StopVibration();
            return;
        }
        if (m_vibrationActive && now >= m_vibrationUntil) StopVibration();
        // Native Xbox rumble: a light pulse for jump and a stronger, short
        // pulse for each new attack press. Keyboard input never rumbles.
        if (pressed & PadII) {
            SetVibration(0.22f, 0.48f);
            m_vibrationUntil = now + std::chrono::milliseconds(75);
        } else if (pressed & PadI) {
            SetVibration(0.10f, 0.20f);
            m_vibrationUntil = now + std::chrono::milliseconds(50);
        }
    }
    static Input Rising(Input const& now, Input const& before) {
        Input edge;
        edge.pause = now.pause && !before.pause;
        edge.accept = now.accept && !before.accept;
        edge.save = now.save && !before.save;
        edge.load = now.load && !before.load;
        edge.import = now.import && !before.import;
        edge.overlayNext = now.overlayNext && !before.overlayNext;
        return edge;
    }

    void CheckMemory(Clock::time_point now) {
        if (now - m_lastMemoryCheck < std::chrono::seconds(2)) return;
        m_lastMemoryCheck = now;
        const auto limit = MemoryManager::AppMemoryUsageLimit();
        const auto used = MemoryManager::AppMemoryUsage();
        if (limit && used > limit - limit / 10) {
            Pause();
            FlushBram();
            m_video.Trim();
            Status(L"Pouca memória disponível. Execução pausada; feche outros aplicativos antes de continuar.");
        }
    }
    std::wstring Overlay() const {
        if (!m_paused && !m_busy && Active()) return Clock::now() < m_statusUntil ? m_status : L"";
        std::wstring text = L"RONDORECOMP · XBOX SERIES S|X\n";
        text += m_busy ? L"Aguarde…\n\n" : Active() ? L"Pausado\n\n" : L"Preparação do jogo\n\n";
        text += m_status + L"\n\n";
        text += L"A / Enter: jogar ou procurar disco local\nX / I: importar CHD\n";
        if (Active()) text += L"Y / F5: salvar estado     B / F8: carregar estado\n";
        text += L"Moldura " + std::to_wstring(m_overlayIndex + 1) + L"/2: RB / O alterna\n";
        text += L"\nJogo: direcional / analógico, A: pular, X: atacar\nMenu: Run     LB: Select     View / Esc: pausar\n\n";
        text += to_hstring(rondo_session_build_id());
        const auto limit = MemoryManager::AppMemoryUsageLimit();
        text += L"\nMemória: " + std::to_wstring(MemoryManager::AppMemoryUsage() >> 20) +
                L" / " + std::to_wstring(limit >> 20) + L" MiB";
        if (!m_audio.Ready()) text += L"\nDispositivo de áudio indisponível.";
        return text;
    }

    CoreApplicationView m_view{nullptr};
    CoreWindow m_window{nullptr};
    SystemNavigationManager m_navigation{nullptr};
    Gamepad m_gamepad{nullptr};
    StorageFolder m_gameFolder{nullptr}, m_saveFolder{nullptr};
    rondo::Video m_video;
    rondo::Audio m_audio;
    Session m_session;
    bool m_closed = false, m_visible = true, m_paused = true, m_busy = false;
    bool m_keyboardUsed = false;
    bool m_vibrationActive = false, m_overlayReady = false;
    uint32_t m_overlayIndex = 0;
    std::atomic<bool> m_resumeRequested{false};
    std::array<bool, 256> m_keys{};
    Input m_previousInput;
    std::wstring m_status = L"Preparando o armazenamento local…";
    Clock::time_point m_statusUntil{}, m_lastBramFlush{}, m_lastMemoryCheck{};
    Clock::time_point m_vibrationUntil{};
    event_token m_activated{}, m_suspending{}, m_resuming{}, m_backToken{}, m_closedToken{}, m_visibilityToken{},
                m_activationToken{}, m_sizeToken{}, m_keyDownToken{}, m_keyUpToken{};
};
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        init_apartment(apartment_type::multi_threaded);
        CoreApplication::Run(make<App>());
        return 0;
    } catch (hresult_error const& e) {
        OutputDebugStringW((L"RondoRecomp UWP: " + std::wstring(e.message()) + L"\n").c_str());
        return static_cast<int>(e.code().value);
    }
}
