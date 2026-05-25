// IdeaCatcher - tray-resident corner overlay for idea capture
// Win32 + DirectX 11 + Dear ImGui + nlohmann/json

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

#include "imgui.h"
#include "imgui_internal.h"   // ClearActiveID
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "json.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// =================== DX11 globals ===================
static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain1*        g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;
static UINT g_ResizeW = 0, g_ResizeH = 0;

// DirectComposition objects - own the alpha-aware visual tree.
static IDCompositionDevice*    g_dcompDevice = nullptr;
static IDCompositionTarget*    g_dcompTarget = nullptr;
static IDCompositionVisual*    g_dcompVisual = nullptr;

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT HOTKEY_ID   = 0xC0DE;
constexpr UINT TRAY_UID    = 1;

constexpr int  EDGE_MARGIN = 16;

// =================== State ===================
enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };

struct Settings {
    UINT   hotkeyMods = MOD_CONTROL | MOD_SHIFT;
    UINT   hotkeyVk   = VK_SPACE;
    Corner corner     = Corner::BottomRight;
    int    width      = 380;
    int    height     = 480;
};

struct Idea {
    std::string tag;
    std::string text;
    int64_t     ts;
};

static Settings          g_settings;
static std::vector<Idea> g_ideas;
static fs::path          g_ideasFile;
static fs::path          g_settingsFile;

static HWND  g_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};

static bool g_visible        = false;
static bool g_focusInputNext = false;
static bool g_shouldExit     = false;
static bool g_openSettings   = false;
static bool g_capturingHotkey = false;
static bool g_capturingWaitForRelease = false;

// =================== Forward decls ===================
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// =================== Paths ===================
static fs::path GetDataDir() {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &p))) {
        out = fs::path(p) / L"IdeaCatcher";
        CoTaskMemFree(p);
        std::error_code ec;
        fs::create_directories(out, ec);
    }
    return out;
}

// =================== Settings load/save ===================
static const char* CornerToStr(Corner c) {
    switch (c) {
        case Corner::TopLeft:     return "TopLeft";
        case Corner::TopRight:    return "TopRight";
        case Corner::BottomLeft:  return "BottomLeft";
        case Corner::BottomRight: return "BottomRight";
    }
    return "BottomRight";
}
static Corner CornerFromStr(const std::string& s) {
    if (s == "TopLeft")     return Corner::TopLeft;
    if (s == "TopRight")    return Corner::TopRight;
    if (s == "BottomLeft")  return Corner::BottomLeft;
    return Corner::BottomRight;
}

// MinGW's std::ofstream + std::filesystem::path combo is unreliable for
// wide paths, so we use the Win32 API directly for both read and write.
static bool ReadFileToString(const fs::path& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.assign((size_t)sz.QuadPart, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)sz.QuadPart, &read, NULL);
    CloseHandle(h);
    if (!ok) { out.clear(); return false; }
    out.resize(read);
    return true;
}

static bool WriteStringToFile(const fs::path& p, const std::string& data) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, NULL);
    CloseHandle(h);
    return ok && written == data.size();
}

static void LoadSettings() {
    if (g_settingsFile.empty()) return;
    std::string raw;
    if (!ReadFileToString(g_settingsFile, raw)) return;
    try {
        json j = json::parse(raw);
        g_settings.hotkeyMods = j.value("hotkey_mods", g_settings.hotkeyMods);
        g_settings.hotkeyVk   = j.value("hotkey_vk",   g_settings.hotkeyVk);
        g_settings.corner     = CornerFromStr(j.value("corner", std::string("BottomRight")));
        g_settings.width      = j.value("width",  g_settings.width);
        g_settings.height     = j.value("height", g_settings.height);
    } catch (...) {}
    // Migrate users stuck on the now-removed S preset (300x380) to M.
    if (g_settings.width == 300 && g_settings.height == 380) {
        g_settings.width  = 380;
        g_settings.height = 480;
    }
}

static void SaveSettings() {
    if (g_settingsFile.empty()) return;
    try {
        json j;
        j["hotkey_mods"] = g_settings.hotkeyMods;
        j["hotkey_vk"]   = g_settings.hotkeyVk;
        j["corner"]      = CornerToStr(g_settings.corner);
        j["width"]       = g_settings.width;
        j["height"]      = g_settings.height;
        WriteStringToFile(g_settingsFile, j.dump(2));
    } catch (...) {}
}

// =================== Ideas load/save ===================
static void LoadIdeas() {
    if (g_ideasFile.empty()) return;
    std::string raw;
    if (!ReadFileToString(g_ideasFile, raw)) return;
    try {
        json j = json::parse(raw);
        for (auto& e : j) {
            Idea i;
            i.text = e.value("text", std::string{});
            i.tag  = e.value("tag",  std::string{});
            i.ts   = e.value("ts",   (int64_t)0);
            g_ideas.push_back(std::move(i));
        }
    } catch (...) {}
}

static void SaveIdeas() {
    if (g_ideasFile.empty()) return;
    try {
        json j = json::array();
        for (auto& i : g_ideas) {
            j.push_back({{"text", i.text}, {"tag", i.tag}, {"ts", i.ts}});
        }
        WriteStringToFile(g_ideasFile, j.dump(2));
    } catch (...) {}
}

// =================== Time ===================
static std::string FormatAbsolute(int64_t ts) {
    std::time_t t = (std::time_t)ts;
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    strftime(buf, sizeof(buf), "%b %d, %H:%M", &tm);
    return buf;
}
static std::string FormatRelative(int64_t ts) {
    int64_t now = (int64_t)std::time(nullptr);
    int64_t d = now - ts;
    if (d < 0)      return FormatAbsolute(ts);
    if (d < 60)     return "just now";
    if (d < 3600)   { auto n = d / 60;    return std::to_string(n) + (n == 1 ? " min ago" : " mins ago"); }
    if (d < 86400)  { auto n = d / 3600;  return std::to_string(n) + (n == 1 ? " hr ago"  : " hrs ago"); }
    if (d < 604800) { auto n = d / 86400; return std::to_string(n) + (n == 1 ? " day ago" : " days ago"); }
    return FormatAbsolute(ts);
}

// =================== Tag parsing / color ===================
static void ParseTag(const std::string& in, std::string& tagOut, std::string& bodyOut) {
    tagOut.clear();
    bodyOut = in;
    size_t i = 0;
    while (i < in.size() && std::isspace((unsigned char)in[i])) ++i;
    if (i >= in.size() || in[i] != '[') return;
    size_t close = in.find(']', i + 1);
    if (close == std::string::npos) return;
    std::string raw = in.substr(i + 1, close - i - 1);
    // trim raw
    size_t a = raw.find_first_not_of(" \t");
    size_t b = raw.find_last_not_of(" \t");
    if (a == std::string::npos) return;
    tagOut = raw.substr(a, b - a + 1);
    // uppercase for consistency
    for (auto& c : tagOut) c = (char)std::toupper((unsigned char)c);
    // body = everything after ']'
    bodyOut = in.substr(close + 1);
    size_t t1 = bodyOut.find_first_not_of(" \t\r\n");
    size_t t2 = bodyOut.find_last_not_of(" \t\r\n");
    if (t1 == std::string::npos) bodyOut.clear();
    else bodyOut = bodyOut.substr(t1, t2 - t1 + 1);
}

// Reject newline characters so Enter is reserved for submit.
static int InputRejectNewline(ImGuiInputTextCallbackData* data) {
    if (data->EventChar == '\n' || data->EventChar == '\r') return 1;
    return 0;
}

static void DrawTagPill(const std::string& tag) {
    if (tag.empty()) return;
    ImVec2 textSz = ImGui::CalcTextSize(tag.c_str());
    ImVec2 pad(8, 2);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    p0.y += 1;
    ImVec2 p1(p0.x + textSz.x + pad.x * 2, p0.y + textSz.y + pad.y * 2);
    ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
    ImU32 tx = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, bg, 6.0f);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), tx, tag.c_str());
    ImGui::Dummy(ImVec2(textSz.x + pad.x * 2, textSz.y + pad.y * 2));
}

// =================== Hotkey helpers ===================
static std::string VkName(UINT vk) {
    // Special names that GetKeyNameText handles poorly across layouts
    switch (vk) {
        case VK_SPACE:  return "Space";
        case VK_RETURN: return "Enter";
        case VK_TAB:    return "Tab";
        case VK_BACK:   return "Backspace";
        case VK_ESCAPE: return "Esc";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME:   return "Home";
        case VK_END:    return "End";
        case VK_PRIOR:  return "PgUp";
        case VK_NEXT:   return "PgDn";
        case VK_LEFT:   return "Left";
        case VK_RIGHT:  return "Right";
        case VK_UP:     return "Up";
        case VK_DOWN:   return "Down";
    }
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        return std::string(1, (char)vk);
    }
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
        vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
        vk == VK_INSERT || vk == VK_DELETE) {
        scan |= 0x100; // extended
    }
    wchar_t buf[64] = {0};
    if (GetKeyNameTextW((LONG)(scan << 16), buf, 64) > 0) {
        char out[64]; size_t n = 0;
        wcstombs_s(&n, out, buf, sizeof(out));
        return std::string(out);
    }
    return "VK_" + std::to_string(vk);
}
static std::string HotkeyToString(UINT mods, UINT vk) {
    std::string s;
    if (mods & MOD_CONTROL) s += "Ctrl + ";
    if (mods & MOD_ALT)     s += "Alt + ";
    if (mods & MOD_SHIFT)   s += "Shift + ";
    if (mods & MOD_WIN)     s += "Win + ";
    s += VkName(vk);
    return s;
}
static bool IsModifierVk(int vk) {
    return vk == VK_SHIFT  || vk == VK_LSHIFT  || vk == VK_RSHIFT  ||
           vk == VK_CONTROL|| vk == VK_LCONTROL|| vk == VK_RCONTROL||
           vk == VK_MENU   || vk == VK_LMENU   || vk == VK_RMENU   ||
           vk == VK_LWIN   || vk == VK_RWIN    || vk == VK_CAPITAL ||
           vk == VK_NUMLOCK|| vk == VK_SCROLL;
}

static void ReRegisterHotkey() {
    UnregisterHotKey(g_hwnd, HOTKEY_ID);
    RegisterHotKey(g_hwnd, HOTKEY_ID,
        g_settings.hotkeyMods | MOD_NOREPEAT, g_settings.hotkeyVk);
}

// =================== Window position ===================
static void PositionToCorner() {
    HMONITOR hm = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfo(hm, &mi);
    RECT w = mi.rcWork;
    int workW = w.right  - w.left;
    int workH = w.bottom - w.top;

    // Clamp to the monitor's work area so picking a large preset never
    // pushes the overlay off the visible screen.
    int W = g_settings.width;
    int H = g_settings.height;
    int maxW = workW - 2 * EDGE_MARGIN;
    int maxH = workH - 2 * EDGE_MARGIN;
    if (W > maxW) W = maxW;
    if (H > maxH) H = maxH;

    int x = w.left, y = w.top;
    switch (g_settings.corner) {
        case Corner::TopLeft:     x = w.left  + EDGE_MARGIN;     y = w.top    + EDGE_MARGIN;     break;
        case Corner::TopRight:    x = w.right - W - EDGE_MARGIN; y = w.top    + EDGE_MARGIN;     break;
        case Corner::BottomLeft:  x = w.left  + EDGE_MARGIN;     y = w.bottom - H - EDGE_MARGIN; break;
        case Corner::BottomRight: x = w.right - W - EDGE_MARGIN; y = w.bottom - H - EDGE_MARGIN; break;
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, W, H, SWP_SHOWWINDOW);
}

static void ShowApp() {
    if (!g_visible) {
        PositionToCorner();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        g_visible = true;
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_hwnd);
    SetActiveWindow(g_hwnd);
    // Don't auto-focus the input on show - the user clicks it when they
    // want to type, so the caret only blinks when they've engaged with it.
}
static void HideApp() {
    if (g_visible) { ShowWindow(g_hwnd, SW_HIDE); g_visible = false; }
}
static void ToggleApp() {
    if (g_visible && GetForegroundWindow() == g_hwnd) HideApp();
    else ShowApp();
}

// =================== Tray ===================
static void InitTray(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImageW(NULL, IDI_INFORMATION, IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"IdeaCatcher");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void RemoveTray() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

static void ShowTrayMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING,    1, L"Show");
    AppendMenuW(m, MF_STRING,    3, L"Settings");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING,    2, L"Quit");
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) ShowApp();
    else if (cmd == 3) { ShowApp(); g_openSettings = true; }
    else if (cmd == 2) { g_shouldExit = true; DestroyWindow(hwnd); }
}

// =================== Style ===================
static void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(10, 8);
    s.ItemSpacing       = ImVec2(8, 8);
    s.WindowRounding    = 0;
    s.FrameRounding     = 8;
    s.GrabRounding      = 8;
    s.ScrollbarRounding = 8;
    s.ChildRounding     = 8;
    s.PopupRounding     = 8;
    s.WindowBorderSize  = 0;
    s.FrameBorderSize   = 1;
    s.ChildBorderSize   = 1;
    s.ScrollbarSize     = 10;

    // Strict black-and-white palette: pure black window, white text,
    // gray for inactive states. No blue / no color accents anywhere.
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f); // fully transparent
    c[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f); // transparent (per-card overrides)
    c[ImGuiCol_PopupBg]              = ImVec4(0.04f, 0.04f, 0.04f, 0.97f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.07f, 0.07f, 0.07f, 0.70f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.12f, 0.12f, 0.12f, 0.80f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.16f, 0.16f, 0.16f, 0.85f);
    c[ImGuiCol_Text]                 = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_Border]               = ImVec4(1.00f, 1.00f, 1.00f, 0.15f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    c[ImGuiCol_Button]               = ImVec4(0.13f, 0.13f, 0.13f, 1.0f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.27f, 0.27f, 0.27f, 1.0f);
    c[ImGuiCol_Header]               = ImVec4(0.13f, 0.13f, 0.13f, 1.0f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.27f, 0.27f, 0.27f, 1.0f);
    c[ImGuiCol_Separator]            = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(1.00f, 1.00f, 1.00f, 0.20f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
    c[ImGuiCol_CheckMark]            = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_NavHighlight]         = ImVec4(1.00f, 1.00f, 1.00f, 0.35f);
    c[ImGuiCol_DragDropTarget]       = ImVec4(1.00f, 1.00f, 1.00f, 0.40f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(1.00f, 1.00f, 1.00f, 0.20f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
    // No modal dim - it would paint a translucent gray over the transparent
    // overlay and look like the bg "turned black" whenever Settings is open.
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
}

// =================== Settings popup ===================
static void PollHotkeyCapture() {
    if (!g_capturingHotkey) return;
    // Cancel on Esc
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        g_capturingHotkey = false;
        g_capturingWaitForRelease = false;
        return;
    }
    // Require all keys to come up first so the mouse-click on "Change" doesn't leak.
    if (g_capturingWaitForRelease) {
        bool anyDown = false;
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
            if (GetAsyncKeyState(vk) & 0x8000) { anyDown = true; break; }
        }
        if (!anyDown) g_capturingWaitForRelease = false;
        return;
    }
    // Now capture: collect current modifiers and the first non-mod key down
    UINT mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
        if (IsModifierVk(vk)) continue;
        if (GetAsyncKeyState(vk) & 0x8000) {
            g_settings.hotkeyMods = mods;
            g_settings.hotkeyVk   = (UINT)vk;
            g_capturingHotkey = false;
            ReRegisterHotkey();
            SaveSettings();
            break;
        }
    }
}

static void RenderSettingsPopup() {
    if (g_openSettings) {
        ImGui::OpenPopup("Settings");
        g_openSettings = false;
    }

    // Center the popup inside the overlay viewport AND clamp its max size to
    // the viewport (minus a small inset) so at the S size preset (300x380)
    // it doesn't overflow off-screen into the docked corner. Recentre every
    // frame so changing the overlay size mid-session keeps it visible.
    ImVec2 vpSize   = ImGui::GetMainViewport()->Size;
    ImVec2 vpCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    float maxW = vpSize.x - 12.0f;
    float maxH = vpSize.y - 12.0f;
    if (maxW < 200.0f) maxW = 200.0f;
    if (maxH < 200.0f) maxH = 200.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 100.0f), ImVec2(maxW, maxH));
    float initialW = (280.0f < maxW) ? 280.0f : maxW;
    ImGui::SetNextWindowSize(ImVec2(initialW, 0.0f), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.72f, 1.0f));
        ImGui::TextUnformatted("Hotkey");
        ImGui::PopStyleColor();

        std::string label;
        if (g_capturingHotkey) {
            label = g_capturingWaitForRelease ? "Release keys..." : "Press combo (Esc to cancel)";
        } else {
            label = HotkeyToString(g_settings.hotkeyMods, g_settings.hotkeyVk);
        }
        const float kChangeBtnW = 72.0f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        char buf[128]; snprintf(buf, sizeof(buf), "%s", label.c_str());
        ImGui::SetNextItemWidth(-(kChangeBtnW + 8.0f));
        ImGui::InputText("##hk", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopItemFlag();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (!g_capturingHotkey) {
            if (ImGui::Button("Change", ImVec2(kChangeBtnW, 0))) {
                g_capturingHotkey = true;
                g_capturingWaitForRelease = true;
            }
        } else {
            if (ImGui::Button("Cancel", ImVec2(kChangeBtnW, 0))) {
                g_capturingHotkey = false;
                g_capturingWaitForRelease = false;
            }
        }

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.72f, 1.0f));
        ImGui::TextUnformatted("Position");
        ImGui::PopStyleColor();

        // Corner buttons size to fill the popup so they shrink with the
        // S preset instead of pushing the popup wider than the overlay.
        float halfBtnW = (ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f;
        auto cornerButton = [halfBtnW](const char* label, Corner c) {
            bool active = (g_settings.corner == c);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 1.00f, 1.00f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.00f, 0.00f, 0.00f, 1.0f));
            }
            if (ImGui::Button(label, ImVec2(halfBtnW, 32))) {
                g_settings.corner = c;
                PositionToCorner();
                SaveSettings();
            }
            if (active) ImGui::PopStyleColor(4);
        };
        cornerButton("Top Left", Corner::TopLeft);     ImGui::SameLine(0, 6);
        cornerButton("Top Right", Corner::TopRight);
        cornerButton("Bottom Left", Corner::BottomLeft); ImGui::SameLine(0, 6);
        cornerButton("Bottom Right", Corner::BottomRight);

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.72f, 1.0f));
        ImGui::TextUnformatted("Size");
        ImGui::PopStyleColor();

        struct SizePreset { const char* label; int w, h; };
        static const SizePreset presets[3] = {
            { "M",  380, 480 },
            { "L",  460, 600 },
            { "XL", 560, 760 },
        };
        const int kNumPresets = 3;
        float fullW = ImGui::GetContentRegionAvail().x;
        float gap   = 6.0f;
        float btnW  = (fullW - gap * (kNumPresets - 1)) / (float)kNumPresets;
        for (int i = 0; i < kNumPresets; ++i) {
            bool active = (g_settings.width == presets[i].w && g_settings.height == presets[i].h);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 1.00f, 1.00f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.00f, 0.00f, 0.00f, 1.0f));
            }
            if (ImGui::Button(presets[i].label, ImVec2(btnW, 32))) {
                g_settings.width  = presets[i].w;
                g_settings.height = presets[i].h;
                PositionToCorner();
                SaveSettings();
            }
            if (active) ImGui::PopStyleColor(4);
            if (i < kNumPresets - 1) ImGui::SameLine(0, gap);
        }

        ImGui::Dummy(ImVec2(0, 8));
        if (ImGui::Button("Close", ImVec2(-1, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// =================== Main UI ===================
static void RenderUI() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Multi-line input field below: padding deliberately matches the cards
    // so the typing field looks like a card the user types directly into.
    const ImVec2 inputFramePad(14.0f, 12.0f);
    const float kInputLines = 2.0f;
    const float inputH = ImGui::GetTextLineHeight() * kInputLines
                       + inputFramePad.y * 2.0f;
    const float inputArea = inputH + ImGui::GetStyle().ItemSpacing.y;

    ImGui::BeginChild("##cards", ImVec2(0, -inputArea), false, ImGuiWindowFlags_NoScrollbar);

    // Bottom-stacking: pre-compute the total height of all cards (matching
    // the per-card formula below) and push them to the bottom with a Dummy
    // spacer when they don't yet fill the available vertical space.
    {
        const ImVec2 cardPadPre(14, 12);
        const float  kHeadBodyGap = 6.0f;
        const float  lineH   = ImGui::GetTextLineHeight();
        const float  spacing = ImGui::GetStyle().ItemSpacing.y;
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y;
        float wrapBase = availW - cardPadPre.x * 2.0f;
        float totalH = 0.0f;
        for (size_t i = 0; i < g_ideas.size(); ++i) {
            float headerExtra = g_ideas[i].tag.empty() ? 2.0f : 4.0f;
            ImVec2 ts = ImGui::CalcTextSize(g_ideas[i].text.c_str(), nullptr, false, wrapBase);
            totalH += cardPadPre.y * 2.0f + lineH + headerExtra + kHeadBodyGap + ts.y;
        }
        if (g_ideas.size() > 1) totalH += spacing * (float)(g_ideas.size() - 1);
        if (totalH < availH) {
            ImGui::Dummy(ImVec2(1.0f, availH - totalH));
        }
    }

    int deleteIdx = -1;
    for (size_t i = 0; i < g_ideas.size(); ++i) {
        const Idea& idea = g_ideas[i];
        ImGui::PushID((int)i);

        const ImVec2 cardPad(14, 12);
        const float  kHeadBodyGap = 6.0f;
        // Header line is taller than plain text only when a tag pill is present.
        const float  kHeaderExtra = idea.tag.empty() ? 2.0f : 4.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.07f, 0.70f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, cardPad);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, kHeadBodyGap));

        float availW = ImGui::GetContentRegionAvail().x;
        float wrapW  = availW - cardPad.x * 2.0f;
        ImVec2 textSize = ImGui::CalcTextSize(idea.text.c_str(), nullptr, false, wrapW);
        float cardH = cardPad.y * 2.0f + ImGui::GetTextLineHeight()
                    + kHeaderExtra + kHeadBodyGap + textSize.y;

        ImGui::BeginChild("card", ImVec2(0, cardH), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // Header row: tag, timestamp, always-visible delete button on the right.
        if (!idea.tag.empty()) {
            DrawTagPill(idea.tag);
            ImGui::SameLine(0.0f, 8.0f);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
        ImGui::TextUnformatted(FormatRelative(idea.ts).c_str());
        ImGui::PopStyleColor();

        // Delete button anchored to the right edge of the header line.
        // No background fill - only the glyph color changes on hover.
        const char* xLabel = "x";
        ImVec2 xTextSz = ImGui::CalcTextSize(xLabel);
        ImVec2 btnSz(xTextSz.x + 12.0f, xTextSz.y + 4.0f);
        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btnSz.x);
        ImGui::InvisibleButton("##delx", btnSz);
        bool xHov  = ImGui::IsItemHovered();
        bool xDown = ImGui::IsItemActive();
        if (ImGui::IsItemClicked()) deleteIdx = (int)i;

        ImVec2 minP = ImGui::GetItemRectMin();
        ImVec2 maxP = ImGui::GetItemRectMax();
        ImU32 xCol = xDown ? IM_COL32(255, 110, 110, 255)
                  : xHov  ? IM_COL32(235,  80,  80, 255)
                          : IM_COL32(150, 150, 150, 255);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(minP.x + ((maxP.x - minP.x) - xTextSz.x) * 0.5f,
                   minP.y + ((maxP.y - minP.y) - xTextSz.y) * 0.5f),
            xCol, xLabel);

        ImGui::TextWrapped("%s", idea.text.c_str());

        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    static size_t lastCount = 0;
    if (g_ideas.size() > lastCount) {
        ImGui::SetScrollHereY(1.0f);
    }
    lastCount = g_ideas.size();

    ImGui::EndChild();

    if (deleteIdx >= 0) {
        g_ideas.erase(g_ideas.begin() + deleteIdx);
        SaveIdeas();
    }

    // Multi-line input field: long text wraps to a new visual line
    // instead of running off to the right. Enter submits (newlines are
    // filtered out by the char callback so they never reach the buffer).
    static char inputBuf[1024] = {0};
    if (g_focusInputNext) {
        ImGui::SetKeyboardFocusHere();
        g_focusInputNext = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, inputFramePad);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::InputTextMultiline("##in", inputBuf, IM_ARRAYSIZE(inputBuf),
        ImVec2(-1, inputH),
        ImGuiInputTextFlags_CallbackCharFilter,
        InputRejectNewline);
    ImGui::PopStyleVar(3);
    bool inputFocused = ImGui::IsItemFocused();

    // Manual placeholder hint - InputTextMultiline has no built-in hint API.
    if (inputBuf[0] == '\0') {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(mn.x + inputFramePad.x, mn.y + inputFramePad.y),
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.45f, 0.45f, 0.50f, 1.0f)),
            "Capture an idea... ([TAG] optional)");
    }

    if (inputFocused && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        std::string raw(inputBuf);
        size_t t1 = raw.find_first_not_of(" \t\r\n");
        size_t t2 = raw.find_last_not_of(" \t\r\n");
        if (t1 != std::string::npos) {
            std::string clean = raw.substr(t1, t2 - t1 + 1);
            Idea i;
            ParseTag(clean, i.tag, i.text);
            if (i.text.empty() && !i.tag.empty()) {
                i.text = "[" + i.tag + "]";
                i.tag.clear();
            }
            if (!i.text.empty()) {
                i.ts = (int64_t)std::time(nullptr);
                g_ideas.push_back(std::move(i));
                SaveIdeas();
            }
        }
        inputBuf[0] = '\0';
        // ImGui caches the input's edit state separately from the buffer
        // while a widget is active. Deactivating it forces a reload from
        // our (now-empty) buffer on the next frame.
        ImGui::ClearActiveID();
        g_focusInputNext = true;
    }

    RenderSettingsPopup();
    ImGui::End();
}

// =================== WinMain ===================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"IdeaCatcher_SingleInstance_Mutex_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mtx) CloseHandle(mtx);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst,
                       NULL, LoadCursorW(NULL, IDC_ARROW), NULL, NULL,
                       L"IdeaCatcherWnd", NULL };
    RegisterClassExW(&wc);

    fs::path dir = GetDataDir();
    g_ideasFile    = dir / L"ideas.json";
    g_settingsFile = dir / L"settings.json";
    LoadSettings();
    LoadIdeas();

    // Borderless overlay: WS_POPUP (no caption, no border).
    // WS_EX_TOOLWINDOW hides from taskbar/alt-tab. WS_EX_TOPMOST keeps it on top.
    DWORD style    = WS_POPUP | WS_CLIPCHILDREN;
    DWORD exStyle  = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    g_hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"IdeaCatcher",
        style, 0, 0, g_settings.width, g_settings.height,
        NULL, NULL, hInst, NULL);

    // Disable Win11 auto-rounding so the overlay is a clean rectangle.
    DWORD corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInst);
        if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    io.Fonts->Clear();
    ImFontConfig cfg; cfg.OversampleH = 3; cfg.OversampleV = 2;
    const char* fontPath = "C:/Windows/Fonts/segoeui.ttf";
    if (fs::exists(fontPath)) io.Fonts->AddFontFromFileTTF(fontPath, 17.0f, &cfg);
    else io.Fonts->AddFontDefault();

    ApplyStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    InitTray(g_hwnd);
    ReRegisterHotkey();
    ShowApp();

    MSG msg;
    bool done = false;
    while (!done) {
        if (g_visible) {
            MsgWaitForMultipleObjects(0, NULL, FALSE, 16, QS_ALLINPUT);
            while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done || g_shouldExit) break;

            if (g_ResizeW != 0 && g_ResizeH != 0) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, g_ResizeW, g_ResizeH, DXGI_FORMAT_UNKNOWN, 0);
                g_ResizeW = g_ResizeH = 0;
                CreateRenderTarget();
            }

            PollHotkeyCapture();

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Esc behaviour: capturing handled in PollHotkeyCapture;
            // popup Esc is handled by ImGui itself.
            if (!g_capturingHotkey && !ImGui::IsPopupOpen("Settings") && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                HideApp();
            }

            RenderUI();

            ImGui::Render();
            const float clear[4] = { 0.00f, 0.00f, 0.00f, 0.0f };
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, NULL);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_pSwapChain->Present(1, 0);
        } else {
            if (GetMessage(&msg, NULL, 0U, 0U) <= 0) { done = true; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (g_shouldExit) break;
        }
    }

    UnregisterHotKey(g_hwnd, HOTKEY_ID);
    RemoveTray();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    if (IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}

// =================== WndProc ===================
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeW = (UINT)LOWORD(lParam);
        g_ResizeH = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        if (g_shouldExit) DestroyWindow(hWnd);
        else              HideApp();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_HOTKEY:
        if (wParam == HOTKEY_ID) ToggleApp();
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK) ShowApp();
        else if (LOWORD(lParam) == WM_RBUTTONUP) ShowTrayMenu(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// =================== DX11 + DirectComposition ===================
// Per-pixel transparency on Windows requires a DirectComposition swap chain.
// HWND swap chains cannot carry alpha to the desktop compositor; only a
// composition swap chain attached to a DComp visual can.
bool CreateDeviceD3D(HWND hWnd) {
    // 1. Create the D3D11 device. BGRA_SUPPORT is required by DComp.
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 2, D3D11_SDK_VERSION,
        &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 2, D3D11_SDK_VERSION,
            &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    }
    if (FAILED(hr)) return false;

    // 2. Walk Device -> Adapter -> Factory2.
    IDXGIDevice*   dxgiDevice  = nullptr;
    IDXGIAdapter*  dxgiAdapter = nullptr;
    IDXGIFactory2* dxgiFactory = nullptr;
    g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));

    // 3. Composition swap chain - this is the magic for per-pixel alpha.
    RECT rc; GetClientRect(hWnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width            = (UINT)(rc.right - rc.left);
    sd.Height           = (UINT)(rc.bottom - rc.top);
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.Scaling          = DXGI_SCALING_STRETCH;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED;

    hr = dxgiFactory->CreateSwapChainForComposition(g_pd3dDevice, &sd, NULL, &g_pSwapChain);

    // 4. Build the DComp tree: device -> target(hwnd) -> visual -> swap chain.
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDevice, IID_PPV_ARGS(&g_dcompDevice));
    if (SUCCEEDED(hr)) hr = g_dcompDevice->CreateTargetForHwnd(hWnd, TRUE, &g_dcompTarget);
    if (SUCCEEDED(hr)) hr = g_dcompDevice->CreateVisual(&g_dcompVisual);
    if (SUCCEEDED(hr)) hr = g_dcompVisual->SetContent(g_pSwapChain);
    if (SUCCEEDED(hr)) hr = g_dcompTarget->SetRoot(g_dcompVisual);
    if (SUCCEEDED(hr)) hr = g_dcompDevice->Commit();

    dxgiFactory->Release();
    dxgiAdapter->Release();
    dxgiDevice->Release();

    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}
void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_dcompVisual)       { g_dcompVisual->Release();       g_dcompVisual = nullptr; }
    if (g_dcompTarget)       { g_dcompTarget->Release();       g_dcompTarget = nullptr; }
    if (g_dcompDevice)       { g_dcompDevice->Release();       g_dcompDevice = nullptr; }
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}
void CreateRenderTarget() {
    ID3D11Texture2D* bb = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) {
        g_pd3dDevice->CreateRenderTargetView(bb, NULL, &g_mainRTV);
        bb->Release();
    }
}
void CleanupRenderTarget() {
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
}
