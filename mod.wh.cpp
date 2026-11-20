// ==WindhawkMod==
// @id              audio-output-switcher
// @name            Audio Output Switcher
// @description     Show the default playback device in the notification area and switch between recent outputs with one click.
// @version         1.0
// @author          danieltorscho
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -luuid -lshell32 -lgdi32 -ladvapi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Audio Output Switcher

The notification-area icon shows the type of your Windows default playback
device, such as headphones, speakers, or a display. Hover for its name.
If Windows puts the icon under **Show hidden icons**, drag it onto the taskbar.

- **Right-click** to pick any connected, enabled playback device. The current
  output has a radio mark beside it.
- **Left-click** to return to your most recent other output that's still
  available. If you just installed the mod, use the right-click menu to pick a
  second output first. Successive clicks switch back and forth.

History is saved across Explorer restarts and follows changes you make in
Windows Sound settings. The mod switches the default console and multimedia
playback roles. Apps with their own output selected may continue to use it.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"Windhawk.AudioOutputSwitcher.Tray";
constexpr wchar_t kTitle[] = L"Audio Output Switcher";
constexpr wchar_t kHistoryValue[] = L"outputHistory";
constexpr size_t kHistoryLimit = 8;
constexpr GUID kIconGuid{0xda535970, 0xc6b2, 0x4a19, {0x92, 0xfe, 0xef, 0x6d, 0xf7, 0x27, 0x68, 0x05}};
constexpr UINT kTrayId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kAudioChanged = WM_APP + 2;
constexpr UINT_PTR kMenuTimer = 1;

HANDLE g_stopEvent;
HANDLE g_thread;

// The public MMDevice API can read defaults. Setting them uses this
// undocumented Windows audio policy COM interface. Keep the slot order intact.
constexpr CLSID kPolicyClient{0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr IID kPolicyInterface{0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

struct DeviceShareMode;
struct IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, BOOL, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, BOOL, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
};

enum class IconKind { Speakers, Headphones, Display, NoOutput };

struct Output {
    std::wstring id;
    std::wstring name;
    IconKind kind = IconKind::Speakers;
};

class AudioNotifications final : public IMMNotificationClient {
public:
    explicit AudioNotifications(HWND window) : window_(window) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IMMNotificationClient) {
            return E_NOINTERFACE;
        }
        *result = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, PCWSTR) override {
        return flow == eRender && role == eMultimedia ? Notify() : S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(PCWSTR) override { return Notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(PCWSTR) override { return Notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(PCWSTR, DWORD) override { return Notify(); }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(PCWSTR, const PROPERTYKEY key) override {
        return IsEqualPropertyKey(key, PKEY_Device_FriendlyName) ||
                       IsEqualPropertyKey(key, PKEY_AudioEndpoint_FormFactor)
                   ? Notify()
                   : S_OK;
    }

private:
    HRESULT Notify() {
        PostMessageW(window_, kAudioChanged, 0, 0);
        return S_OK;
    }

    const HWND window_;
    std::atomic<ULONG> references_{1};
};

struct Tray {
    HWND window = nullptr;
    UINT taskbarCreated = 0;
    bool iconAdded = false;
    bool version4 = false;
    bool menuOpen = false;
    bool refreshRequested = true;
    bool audioUnavailable = false;
    bool notificationsRegistered = false;
    COLORREF iconColor = CLR_INVALID;
    IconKind iconKind = IconKind::NoOutput;
    std::wstring tooltip;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMNotificationClient> notifications;
    Output current;
    std::vector<std::wstring> history;
};

bool Stopping() {
    return WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT;
}

void LoadHistory(Tray& tray) {
    // Endpoint IDs are opaque. Store NUL-separated UTF-16 strings as one value.
    std::array<wchar_t, 4096> buffer{};
    const size_t bytes = Wh_GetBinaryValue(kHistoryValue, buffer.data(), sizeof(buffer));
    if (bytes > sizeof(buffer) || bytes % sizeof(wchar_t)) {
        return;
    }
    const wchar_t* end = buffer.data() + bytes / sizeof(wchar_t);
    for (const wchar_t* start = buffer.data(); start < end && tray.history.size() < kHistoryLimit;) {
        const wchar_t* separator = std::find(start, end, L'\0');
        if (separator == start || separator == end) {
            break;
        }
        tray.history.emplace_back(start, separator);
        start = separator + 1;
    }
}

void RememberOutput(Tray& tray, const std::wstring& id) {
    auto& history = tray.history;
    if (id.empty() || (!history.empty() && history.front() == id)) {
        return;
    }
    std::erase(history, id);
    history.insert(history.begin(), id);
    if (history.size() > kHistoryLimit) {
        history.resize(kHistoryLimit);
    }
    std::wstring stored;
    for (const auto& entry : history) {
        stored += entry;
        stored += L'\0';
    }
    if (!Wh_SetBinaryValue(kHistoryValue, stored.data(), stored.size() * sizeof(wchar_t))) {
        Wh_Log(L"Could not save audio output history");
    }
}

std::wstring DeviceId(IMMDevice& device) {
    PWSTR id = nullptr;
    const HRESULT result = device.GetId(&id);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> ownedId(id, CoTaskMemFree);
    return SUCCEEDED(result) && id ? id : L"";
}

IconKind ClassifyOutput(UINT formFactor, const std::wstring& name) {
    if (formFactor == Headphones || formFactor == Headset || formFactor == Handset) {
        return IconKind::Headphones;
    }
    if (formFactor == DigitalAudioDisplayDevice) {
        return IconKind::Display;
    }
    // Some Bluetooth drivers report their headphones as speakers.
    auto lowerName = name;
    CharLowerBuffW(lowerName.data(), static_cast<DWORD>(lowerName.size()));
    if (lowerName.find(L"headphone") != std::wstring::npos ||
        lowerName.find(L"headset") != std::wstring::npos ||
        lowerName.find(L"earbuds") != std::wstring::npos) {
        return IconKind::Headphones;
    }
    return IconKind::Speakers;
}

Output ReadOutput(IMMDevice& device) {
    Output output{DeviceId(device), L"Audio output"};
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device.OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) {
        PROPVARIANT name{};
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR && name.pwszVal && *name.pwszVal) {
            output.name = name.pwszVal;
        }
        PropVariantClear(&name);

        PROPVARIANT formFactor{};
        if (SUCCEEDED(properties->GetValue(PKEY_AudioEndpoint_FormFactor, &formFactor)) &&
            formFactor.vt == VT_UI4) {
            output.kind = ClassifyOutput(formFactor.ulVal, output.name);
        } else {
            output.kind = ClassifyOutput(UnknownFormFactor, output.name);
        }
        PropVariantClear(&formFactor);
    }
    return output;
}

std::optional<std::vector<Output>> AvailableOutputs(IMMDeviceEnumerator& enumerator) {
    ComPtr<IMMDeviceCollection> devices;
    UINT count = 0;
    if (FAILED(enumerator.EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.GetAddressOf())) ||
        FAILED(devices->GetCount(&count))) {
        return std::nullopt;
    }

    std::vector<Output> outputs;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(devices->Item(index, device.GetAddressOf()))) {
            auto output = ReadOutput(*device.Get());
            if (!output.id.empty()) {
                outputs.push_back(std::move(output));
            }
        }
    }
    return outputs;
}

std::wstring PreviousOutputId(const Tray& tray, const std::vector<Output>& available) {
    if (!tray.current.id.empty()) {
        for (const auto& id : tray.history) {
            if (id != tray.current.id && std::ranges::find(available, id, &Output::id) != available.end()) {
                return id;
            }
        }
    }
    return {};
}

HRESULT SelectOutput(IMMDeviceEnumerator& enumerator, const std::wstring& id) {
    ComPtr<IPolicyConfig> policy;
    HRESULT result = CoCreateInstance(kPolicyClient, nullptr, CLSCTX_INPROC_SERVER,
                                     kPolicyInterface, reinterpret_cast<void**>(policy.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IMMDevice> oldMultimedia;
    result = enumerator.GetDefaultAudioEndpoint(eRender, eMultimedia, oldMultimedia.GetAddressOf());
    std::wstring oldId;
    if (SUCCEEDED(result)) {
        oldId = DeviceId(*oldMultimedia.Get());
    } else if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        return result;
    }

    if (oldId != id) {
        result = policy->SetDefaultEndpoint(id.c_str(), eMultimedia);
        if (FAILED(result)) {
            return result;
        }
    }
    result = policy->SetDefaultEndpoint(id.c_str(), eConsole);
    if (FAILED(result) && !oldId.empty() && oldId != id) {
        ComPtr<IMMDevice> current;
        if (SUCCEEDED(enumerator.GetDefaultAudioEndpoint(eRender, eMultimedia, current.GetAddressOf())) &&
            DeviceId(*current.Get()) == id) {
            const HRESULT rollback = policy->SetDefaultEndpoint(oldId.c_str(), eMultimedia);
            if (FAILED(rollback)) {
                Wh_Log(L"Could not restore the previous multimedia output (0x%08X)", static_cast<unsigned>(rollback));
            }
        }
    }
    return result;
}

COLORREF TrayInk() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        return GetSysColor(COLOR_BTNTEXT);
    }

    DWORD lightTheme = 0;
    DWORD bytes = sizeof(lightTheme);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &lightTheme, &bytes);
    return lightTheme ? RGB(30, 30, 30) : RGB(255, 255, 255);
}

void DrawIconShape(HDC dc, IconKind kind) {
    if (kind == IconKind::Headphones) {
        const POINT headband[]{{5, 19}, {5, 2}, {27, 2}, {27, 19}};
        PolyBezier(dc, headband, ARRAYSIZE(headband));
        RoundRect(dc, 5, 17, 12, 28, 5, 5);
        RoundRect(dc, 21, 17, 28, 28, 5, 5);
    } else if (kind == IconKind::Display) {
        RoundRect(dc, 3, 5, 30, 23, 4, 4);
        const POINT stand[]{{16, 23}, {16, 28}, {10, 28}, {22, 28}};
        Polyline(dc, stand, ARRAYSIZE(stand));
    } else {
        const POINT speaker[]{{3, 11}, {8, 11}, {16, 5}, {16, 27}, {8, 21}, {3, 21}};
        Polygon(dc, speaker, ARRAYSIZE(speaker));
        if (kind == IconKind::NoOutput) {
            const POINT cross1[]{{22, 12}, {29, 20}};
            const POINT cross2[]{{22, 20}, {29, 12}};
            Polyline(dc, cross1, ARRAYSIZE(cross1));
            Polyline(dc, cross2, ARRAYSIZE(cross2));
        } else {
            const POINT inner[]{{21, 11}, {24, 13}, {24, 19}, {21, 21}};
            const POINT outer[]{{25, 6}, {32, 11}, {32, 21}, {25, 26}};
            PolyBezier(dc, inner, ARRAYSIZE(inner));
            PolyBezier(dc, outer, ARRAYSIZE(outer));
        }
    }
}

HICON MakeIcon(IconKind kind, COLORREF ink) {
    constexpr int size = 32;
    constexpr int scale = 3;
    constexpr int drawSize = size * scale;
    BITMAPINFO bitmap{};
    bitmap.bmiHeader = {sizeof(BITMAPINFOHEADER), drawSize, -drawSize, 1, 32, BI_RGB};
    void* drawing = nullptr;
    HBITMAP canvas = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &drawing, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    HICON icon = nullptr;

    if (canvas && dc && pen) {
        auto* sample = static_cast<uint32_t*>(drawing);
        std::fill_n(sample, drawSize * drawSize, 0);
        const auto oldBitmap = SelectObject(dc, canvas);
        const auto oldPen = SelectObject(dc, pen);
        const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetMapMode(dc, MM_ANISOTROPIC);
        SetWindowExtEx(dc, size, size, nullptr);
        SetViewportExtEx(dc, drawSize, drawSize, nullptr);
        DrawIconShape(dc, kind);
        GdiFlush();
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBitmap);

        bitmap.bmiHeader.biWidth = size;
        bitmap.bmiHeader.biHeight = -size;
        void* image = nullptr;
        HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &image, nullptr, 0);
        std::array<uint32_t, size> maskBits{};
        HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
        if (color && mask) {
            auto* pixels = static_cast<uint32_t*>(image);
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    unsigned covered = 0;
                    for (int dy = 0; dy < scale; ++dy) {
                        for (int dx = 0; dx < scale; ++dx) {
                            covered += sample[(y * scale + dy) * drawSize + x * scale + dx] != 0;
                        }
                    }
                    const unsigned alpha = covered * 255 / (scale * scale);
                    const unsigned red = GetRValue(ink) * alpha / 255;
                    const unsigned green = GetGValue(ink) * alpha / 255;
                    const unsigned blue = GetBValue(ink) * alpha / 255;
                    pixels[y * size + x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
                }
            }
            ICONINFO info{TRUE, 0, 0, mask, color};
            icon = CreateIconIndirect(&info);
        }
        DeleteObject(mask);
        DeleteObject(color);
    }
    DeleteObject(pen);
    DeleteDC(dc);
    DeleteObject(canvas);
    return icon;
}

NOTIFYICONDATAW IconData(HWND window) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window;
    data.uID = kTrayId;
    data.uFlags = NIF_GUID;
    data.guidItem = kIconGuid;
    return data;
}

void UpdateIcon(Tray& tray) {
    const bool hasOutput = !tray.current.id.empty();
    const auto kind = hasOutput ? tray.current.kind : IconKind::NoOutput;
    const auto color = TrayInk();
    std::wstring tooltip = L"Windows audio output is unavailable";
    if (hasOutput) {
        tooltip = tray.current.name;
    } else if (!tray.audioUnavailable) {
        tooltip = L"No active playback device";
    }
    auto data = IconData(tray.window);
    if (tooltip.size() >= ARRAYSIZE(data.szTip)) {
        tooltip.resize(ARRAYSIZE(data.szTip) - 2);
        if (!tooltip.empty() && IS_HIGH_SURROGATE(tooltip.back())) {
            tooltip.pop_back();
        }
        tooltip += L'\u2026';
    }
    if (tray.iconAdded && kind == tray.iconKind && color == tray.iconColor && tooltip == tray.tooltip) {
        return;
    }

    data.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = MakeIcon(kind, color);
    if (!data.hIcon) {
        return;
    }
    std::copy(tooltip.begin(), tooltip.end(), data.szTip);
    if (!tray.iconAdded || !Shell_NotifyIconW(NIM_MODIFY, &data)) {
        tray.iconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        data.uVersion = NOTIFYICON_VERSION_4;
        tray.version4 = tray.iconAdded && Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    DestroyIcon(data.hIcon);
    tray.iconKind = kind;
    tray.iconColor = color;
    tray.tooltip = std::move(tooltip);
}

void Refresh(Tray& tray) {
    tray.refreshRequested = false;
    if (!tray.enumerator) {
        const HRESULT result = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(tray.enumerator.GetAddressOf()));
        if (SUCCEEDED(result)) {
            tray.notifications.Attach(new AudioNotifications(tray.window));
            tray.notificationsRegistered = SUCCEEDED(tray.enumerator->RegisterEndpointNotificationCallback(tray.notifications.Get()));
        }
    }

    ComPtr<IMMDevice> device;
    const HRESULT result = tray.enumerator
        ? tray.enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.GetAddressOf())
        : E_FAIL;
    tray.current = SUCCEEDED(result) ? ReadOutput(*device.Get()) : Output{};
    tray.audioUnavailable = result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && tray.current.id.empty();
    RememberOutput(tray, tray.current.id);
    UpdateIcon(tray);
}

void NotifyUser(const Tray& tray, const wchar_t* message, DWORD type) {
    auto data = IconData(tray.window);
    data.uFlags |= NIF_INFO;
    data.dwInfoFlags = type | NIIF_NOSOUND;
    wcscpy_s(data.szInfoTitle, kTitle);
    wcscpy_s(data.szInfo, message);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void SwitchOutput(Tray& tray, const std::wstring& id) {
    if (Stopping() || !tray.enumerator) {
        return;
    }
    const HRESULT result = SelectOutput(*tray.enumerator.Get(), id);
    Refresh(tray);
    if (FAILED(result)) {
        Wh_Log(L"Could not switch the Windows default audio output (0x%08X)", static_cast<unsigned>(result));
        NotifyUser(tray, L"Windows could not switch to that output.", NIIF_WARNING);
    }
}

void ToggleOutput(Tray& tray) {
    Refresh(tray);
    const auto outputs = tray.enumerator ? AvailableOutputs(*tray.enumerator.Get()) : std::nullopt;
    if (!outputs) {
        NotifyUser(tray, L"Windows playback devices are unavailable.", NIIF_WARNING);
        return;
    }
    const auto previousId = PreviousOutputId(tray, *outputs);
    if (previousId.empty()) {
        NotifyUser(tray, L"Right-click to choose another playback device first.", NIIF_INFO);
        return;
    }
    SwitchOutput(tray, previousId);
}

std::wstring MenuLabel(std::wstring_view name) {
    std::wstring label;
    for (const wchar_t character : name) {
        if (character == L'&') {
            label += L'&';
        }
        label += character < L' ' ? L' ' : character;
    }
    return label;
}

void ShowMenu(Tray& tray, POINT anchor) {
    Refresh(tray);
    auto outputs = tray.enumerator ? AvailableOutputs(*tray.enumerator.Get()) : std::nullopt;
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    if (!outputs || outputs->empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                    outputs ? L"No active playback devices" : L"Playback devices are unavailable");
    } else {
        std::ranges::stable_sort(*outputs, [](const Output& left, const Output& right) {
            return CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        UINT activeCommand = 0;
        for (size_t index = 0; index < outputs->size(); ++index) {
            const auto& output = (*outputs)[index];
            const auto command = static_cast<UINT>(index + 1);
            if (AppendMenuW(menu, MF_STRING, command, MenuLabel(output.name).c_str()) && output.id == tray.current.id) {
                activeCommand = command;
            }
        }
        if (activeCommand) {
            CheckMenuRadioItem(menu, 1, static_cast<UINT>(outputs->size()), activeCommand, MF_BYCOMMAND);
        }
    }

    UINT command = 0;
    // A popup has its own message loop. Let it see the stop event on unload.
    if (!Stopping() && SetTimer(tray.window, kMenuTimer, 100, nullptr)) {
        tray.menuOpen = true;
        SetForegroundWindow(tray.window);
        command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                   anchor.x, anchor.y, tray.window, nullptr);
        PostMessageW(tray.window, WM_NULL, 0, 0);
        tray.menuOpen = false;
        KillTimer(tray.window, kMenuTimer);
        auto data = IconData(tray.window);
        Shell_NotifyIconW(NIM_SETFOCUS, &data);
    }
    DestroyMenu(menu);
    if (outputs && command && command <= outputs->size()) {
        SwitchOutput(tray, (*outputs)[command - 1].id);
    }
}

LRESULT CALLBACK TrayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    auto* tray = reinterpret_cast<Tray*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!tray) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    if (message == tray->taskbarCreated) {
        tray->iconAdded = false;
        tray->refreshRequested = true;
        return 0;
    }
    if (message == kAudioChanged || message == WM_SETTINGCHANGE || message == WM_THEMECHANGED ||
        message == WM_SYSCOLORCHANGE) {
        tray->refreshRequested = true;
        return 0;
    }
    if (message == kTrayMessage) {
        if (tray->menuOpen || Stopping()) {
            return 0;
        }
        const UINT event = LOWORD(lParam);
        const bool activate = tray->version4 ? event == NIN_SELECT || event == NIN_KEYSELECT : event == WM_LBUTTONUP;
        const bool contextMenu = tray->version4 ? event == WM_CONTEXTMENU : event == WM_RBUTTONUP;
        if (activate) {
            ToggleOutput(*tray);
        } else if (contextMenu) {
            POINT point{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
            if (!tray->version4 || (point.x == -1 && point.y == -1)) {
                NOTIFYICONIDENTIFIER icon{sizeof(icon), window, kTrayId, kIconGuid};
                RECT bounds;
                if (SUCCEEDED(Shell_NotifyIconGetRect(&icon, &bounds))) {
                    point = {bounds.left, bounds.top};
                } else if (!GetCursorPos(&point)) {
                    return 0;
                }
            }
            ShowMenu(*tray, point);
        }
        return 0;
    }
    if (message == WM_TIMER && wParam == kMenuTimer && Stopping()) {
        EndMenu();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void RunTray() {
    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<PCWSTR>(&TrayWindowProc), &module)) {
        return;
    }
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Tray tray;
    LoadHistory(tray);
    tray.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    const WNDCLASSW windowClass{0, TrayWindowProc, 0, 0, module, nullptr, nullptr, nullptr, nullptr, kWindowClass};
    if (!tray.taskbarCreated || !RegisterClassW(&windowClass)) {
        Wh_Log(L"Could not register the tray window (%lu)", GetLastError());
        return;
    }
    tray.window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kTitle, WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, module, &tray);
    if (tray.window) {
        while (!Stopping()) {
            if (tray.refreshRequested) {
                Refresh(tray);
            }

            const DWORD retry = tray.iconAdded && tray.notificationsRegistered && !tray.audioUnavailable
                ? INFINITE : 2000;
            const DWORD wait = MsgWaitForMultipleObjectsEx(1, &g_stopEvent, retry, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_TIMEOUT) {
                tray.refreshRequested = true;
            } else if (wait != WAIT_OBJECT_0 + 1) {
                break;
            }
            MSG message;
            while (!Stopping() && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (tray.notificationsRegistered) {
            tray.enumerator->UnregisterEndpointNotificationCallback(tray.notifications.Get());
        }
        tray.enumerator.Reset();
        tray.notifications.Reset();
        if (tray.iconAdded) {
            auto data = IconData(tray.window);
            Shell_NotifyIconW(NIM_DELETE, &data);
        }
        DestroyWindow(tray.window);
    } else {
        Wh_Log(L"Could not create the tray window (%lu)", GetLastError());
    }
    UnregisterClassW(kWindowClass, module);
}

DWORD WINAPI TrayThread(void*) {
    // Explorer can load the mod before its desktop window exists at sign-in.
    while (!Stopping()) {
        DWORD shellProcess = 0;
        GetWindowThreadProcessId(GetShellWindow(), &shellProcess);
        if (shellProcess == GetCurrentProcessId()) {
            const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(result)) {
                RunTray();
                CoUninitialize();
            } else {
                Wh_Log(L"Could not initialize Core Audio COM (0x%08X)", static_cast<unsigned>(result));
            }
            break;
        }
        if (shellProcess || WaitForSingleObject(g_stopEvent, 500) != WAIT_TIMEOUT) {
            break;
        }
    }
    return 0;
}

}  // namespace

BOOL Wh_ModInit() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return g_stopEvent != nullptr;
}

void Wh_ModAfterInit() {
    g_thread = CreateThread(nullptr, 0, TrayThread, nullptr, 0, nullptr);
    if (!g_thread) {
        Wh_Log(L"Could not start the audio output tray thread (%lu)", GetLastError());
    }
}

void Wh_ModUninit() {
    SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
    }
    CloseHandle(g_stopEvent);
}
