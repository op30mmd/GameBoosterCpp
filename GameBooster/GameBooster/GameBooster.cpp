#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ============================================================
// CONSTANTS
// ============================================================

namespace DwmAttrib {
    constexpr DWORD ImmersiveDarkMode = 20;
}

namespace Theme {
    const Color BgPrimary(255, 18, 18, 24);
    const Color BgSecondary(255, 26, 26, 34);
    const Color BgCard(255, 32, 32, 42);
    const Color BgCardHover(255, 42, 42, 54);
    const Color BgInput(255, 24, 24, 32);
    const Color BgInputFocus(255, 30, 30, 40);

    const Color Border(255, 50, 50, 65);
    const Color BorderFocus(255, 99, 102, 241);

    const Color TextPrimary(255, 248, 250, 252);
    const Color TextSecondary(255, 160, 165, 180);
    const Color TextMuted(255, 100, 105, 120);
    const Color TextDisabled(100, 150, 150, 160);

    const Color Accent(255, 99, 102, 241);
    const Color AccentMuted(80, 99, 102, 241);
    const Color Success(255, 34, 197, 94);
    const Color Warning(255, 250, 176, 5);

    const Color BtnPrimary(255, 99, 102, 241);
    const Color BtnPrimaryHover(255, 79, 82, 221);
    const Color BtnSecondary(255, 45, 45, 58);
    const Color BtnSecondaryHover(255, 55, 55, 70);
    const Color Disabled(100, 50, 50, 60);

    const Color StatusActive(255, 34, 197, 94);
    const Color StatusReady(255, 100, 105, 120);

    const Color Scrollbar(100, 100, 105, 120);

    constexpr COLORREF EditText = RGB(248, 250, 252);
    constexpr COLORREF EditBg = RGB(24, 24, 32);
}

namespace Layout {
    constexpr int MinWindowW = 420, MinWindowH = 480;
    constexpr int InitialW = 480, InitialH = 580;
    constexpr int Padding = 24;
    constexpr int RadiusLg = 12, RadiusSm = 8, RadiusXs = 6;
    constexpr int ControlH = 44, ItemH = 52;
    constexpr int Gap = 16, GapSm = 12, GapXs = 8;
    constexpr int AddBtnW = 100, RemoveBtnW = 140;
    constexpr float ListPadX = 12.f, ListPadY = 12.f;
    constexpr float EditInset = 12.f;

    // Typography
    constexpr int FontSizeTitle = 24;
    constexpr int FontSizeBody = 13;
    constexpr int FontSizeLabel = 11;
    constexpr int FontSizeStatus = 12;
}

enum ControlID { ID_INPUT = 100, ID_BTN_ADD, ID_BTN_REMOVE, ID_TRAY = 1000 };

constexpr UINT     WM_TRAYICON = WM_USER + 1;
constexpr UINT_PTR TIMER_ANIM = 1;
constexpr float    ANIM_SPEED = 0.12f;

static const char* const CONFIG_FILE = "games.txt";
static UINT WM_TASKBARCREATED = 0;

// ============================================================
// UTILITY FUNCTIONS
// ============================================================

static float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static Color LerpColor(const Color& a, const Color& b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto mix = [t](BYTE from, BYTE to) {
        return static_cast<BYTE>(Lerp(static_cast<float>(from),
            static_cast<float>(to), t));
        };
    return Color(mix(a.GetA(), b.GetA()), mix(a.GetR(), b.GetR()),
        mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
}

static Color ApplyPressEffect(const Color& c, float press) {
    if (press <= 0.01f) return c;
    const float factor = 1.0f - (press * 0.15f);
    return Color(c.GetA(),
        static_cast<BYTE>(c.GetR() * factor),
        static_cast<BYTE>(c.GetG() * factor),
        static_cast<BYTE>(c.GetB() * factor));
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::wstring ToWide(const std::string& s) {
    return { s.begin(), s.end() };
}

// ============================================================
// RAII DOUBLE BUFFER
// ============================================================

class DoubleBuffer {
public:
    DoubleBuffer(HDC target, int w, int h)
        : target_(target), w_(w), h_(h)
    {
        memDC_ = CreateCompatibleDC(target);
        bitmap_ = CreateCompatibleBitmap(target, w, h);
        old_ = static_cast<HBITMAP>(SelectObject(memDC_, bitmap_));
    }

    ~DoubleBuffer() {
        BitBlt(target_, 0, 0, w_, h_, memDC_, 0, 0, SRCCOPY);
        SelectObject(memDC_, old_);
        DeleteObject(bitmap_);
        DeleteDC(memDC_);
    }

    DoubleBuffer(const DoubleBuffer&) = delete;
    DoubleBuffer& operator=(const DoubleBuffer&) = delete;

    HDC GetDC() const { return memDC_; }

private:
    HDC target_, memDC_;
    HBITMAP bitmap_, old_;
    int w_, h_;
};

// ============================================================
// LAYOUT METRICS
// ============================================================

struct Metrics {
    float x = 0.f, contentWidth = 0.f;
    float titleY = 0.f, subtitleY = 0.f, lineY = 0.f, addLabelY = 0.f;
    RectF inputRect, addBtnRect;
    float listLabelY = 0.f;
    RectF listRect, removeBtnRect, statusRect;

    void Compute(int w, int h) {
        x = static_cast<float>(Layout::Padding);
        contentWidth = static_cast<float>(w - Layout::Padding * 2);
        float y = x;

        titleY = y;  y += Layout::FontSizeTitle + Layout::GapXs;
        subtitleY = y;  y += Layout::FontSizeBody + Layout::GapSm;
        lineY = y;  y += Layout::Gap + Layout::GapXs;
        addLabelY = y;  y += Layout::FontSizeLabel + Layout::GapXs;

        const float inputW = contentWidth - Layout::AddBtnW - Layout::GapSm;
        inputRect = { x, y, inputW, static_cast<float>(Layout::ControlH) };
        addBtnRect = { x + inputW + Layout::GapSm, y,
                       static_cast<float>(Layout::AddBtnW),
                       static_cast<float>(Layout::ControlH) };
        y += Layout::ControlH + Layout::Gap;

        listLabelY = y;  y += Layout::FontSizeLabel + Layout::GapSm;

        // Space below list: Gap + remove button + Gap + status bar + padding
        const float spaceBelow = Layout::Gap * 2.f + Layout::ControlH * 2.f + Layout::Padding;
        float listH = static_cast<float>(h) - y - spaceBelow;

        if (listH < 0.f) listH = 0.f;
        listRect = { x, y, contentWidth, listH };
        y += listH + Layout::Gap;

        removeBtnRect = { x, y, static_cast<float>(Layout::RemoveBtnW),
                          static_cast<float>(Layout::ControlH) };
        y += Layout::ControlH + Layout::Gap;
        statusRect = { x, y, contentWidth, static_cast<float>(Layout::ControlH) };
    }

    void GetEditPosition(int& ex, int& ey, int& ew, int& eh) const {
        ex = static_cast<int>(inputRect.X + Layout::EditInset);
        ey = static_cast<int>(inputRect.Y + Layout::EditInset);
        ew = static_cast<int>(inputRect.Width - Layout::EditInset * 2);
        eh = static_cast<int>(Layout::ControlH - Layout::EditInset * 2);
    }

    int VisibleListHeight() const {
        return static_cast<int>(listRect.Height - Layout::ListPadY * 2);
    }

    int MaxScroll(int count) const {
        int total = count * Layout::ItemH;
        int visible = VisibleListHeight();
        return (total > visible) ? total - visible : 0;
    }

    int HitTestButton(int mx, int my) const {
        auto contains = [mx, my](const RectF& r) {
            return mx >= r.X && mx <= r.X + r.Width
                && my >= r.Y && my <= r.Y + r.Height;
            };
        if (contains(addBtnRect))    return ID_BTN_ADD;
        if (contains(removeBtnRect)) return ID_BTN_REMOVE;
        return -1;
    }

    int HitTestItem(int mx, int my, int count, int scrollY) const {
        const float lx = listRect.X + Layout::ListPadX;
        const float ly = listRect.Y + Layout::ListPadY;
        const float lw = listRect.Width - Layout::ListPadX * 2;
        const float lh = listRect.Height - Layout::ListPadY * 2;

        if (mx < lx || mx > lx + lw || my < ly || my > ly + lh)
            return -1;
        const int idx = static_cast<int>((my - ly + scrollY) / Layout::ItemH);
        return (idx >= 0 && idx < count) ? idx : -1;
    }
};

// ============================================================
// APPLICATION STATE
// ============================================================

struct ButtonAnim { float hover = 0.f, press = 0.f; };

struct App {
    HWND     hWnd = nullptr;
    HWND     hInput = nullptr;
    WNDPROC  origEditProc = nullptr;
    HFONT    hFont = nullptr;
    HBRUSH   hEditBrush = nullptr;
    NOTIFYICONDATAA trayIcon{};
    Metrics  metrics;

    std::map<int, ButtonAnim> buttonAnims;
    float pulsePhase = 0.f, pulseValue = 0.f;
    int   hoveredItem = -1, selectedItem = -1, scrollY = 0;
    bool  inputFocused = false;
    int   hoveredButton = -1, pressedButton = -1;

    std::vector<std::string>           games;
    mutable std::mutex                 gamesMutex;
    std::atomic<bool>                  running{ true };
    std::atomic<bool>                  gameModeActive{ false };
    std::string                        activeGameName;
    std::map<std::string, std::string> killedProcesses;
    std::string                        statusText = "Ready - Monitoring for games";
    mutable std::mutex                 statusMutex;

    const std::vector<std::string> processKillList{
        "explorer.exe", "SearchHost.exe"
    };

    void RequestRedraw() const {
        if (hWnd) InvalidateRect(hWnd, nullptr, FALSE);
    }

    void SetStatus(const std::string& s) {
        { std::lock_guard lock(statusMutex); statusText = s; }
        RequestRedraw();
    }

    std::string GetStatus() const {
        std::lock_guard lock(statusMutex);
        return statusText;
    }

    void LoadGames() {
        std::lock_guard lock(gamesMutex);
        games.clear();
        std::ifstream file(CONFIG_FILE);
        for (std::string line; std::getline(file, line);)
            if (!line.empty()) games.push_back(line);
    }

    void SaveGames() const {
        std::vector<std::string> snapshot;
        { std::lock_guard lock(gamesMutex); snapshot = games; }
        std::ofstream file(CONFIG_FILE);
        for (const auto& game : snapshot) file << game << '\n';
    }

    bool AddGame(const std::string& input) {
        const std::string name = ToLower(input);
        if (name.empty()) return false;
        std::lock_guard lock(gamesMutex);
        if (std::any_of(games.begin(), games.end(),
            [&](const auto& g) { return g == name; }))
            return false;
        games.push_back(name);
        return true;
    }

    bool RemoveSelected() {
        std::lock_guard lock(gamesMutex);
        if (selectedItem < 0 || selectedItem >= static_cast<int>(games.size()))
            return false;
        games.erase(games.begin() + selectedItem);
        selectedItem = -1;
        return true;
    }

    void ClampScroll() {
        int count;
        { std::lock_guard lock(gamesMutex); count = static_cast<int>(games.size()); }
        scrollY = std::clamp(scrollY, 0, metrics.MaxScroll(count));
    }

    bool IsGameInList(const std::string& name) const {
        std::lock_guard lock(gamesMutex);
        return std::any_of(games.begin(), games.end(),
            [&](const auto& g) { return g == name; });
    }

    void CreateResources() {
        hFont = CreateFontA(Layout::FontSizeBody + 1, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        hEditBrush = CreateSolidBrush(Theme::EditBg);
    }

    void DestroyResources() {
        if (hFont) { DeleteObject(hFont); hFont = nullptr; }
        if (hEditBrush) { DeleteObject(hEditBrush); hEditBrush = nullptr; }
    }

    void AddTrayIcon() {
        trayIcon = {};
        trayIcon.cbSize = sizeof(trayIcon);
        trayIcon.hWnd = hWnd;
        trayIcon.uID = ID_TRAY;
        trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        trayIcon.uCallbackMessage = WM_TRAYICON;
        trayIcon.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
        strcpy_s(trayIcon.szTip, "Game Booster");
        Shell_NotifyIconA(NIM_ADD, &trayIcon);
    }

    void RemoveTrayIcon() {
        Shell_NotifyIconA(NIM_DELETE, &trayIcon);
    }

    void UpdateLayout(int w, int h) {
        metrics.Compute(w, h);
        if (hInput) {
            int ex, ey, ew, eh;
            metrics.GetEditPosition(ex, ey, ew, eh);
            SetWindowPos(hInput, nullptr, ex, ey, ew, eh, SWP_NOZORDER);
        }
        ClampScroll();
    }
};

static App g_app;

// ============================================================
// PROCESS UTILITIES
// ============================================================

namespace ProcessUtil {

    void EnableDebugPrivilege() {
        HANDLE tok;
        if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
            return;
        LUID luid;
        LookupPrivilegeValueA(nullptr, SE_DEBUG_NAME, &luid);
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0] = { luid, SE_PRIVILEGE_ENABLED };
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        CloseHandle(tok);
    }

    std::string GetForegroundProcessName() {
        HWND fg = GetForegroundWindow();
        if (!fg) return {};
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        HANDLE proc = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!proc) return {};
        char path[MAX_PATH]{};
        std::string name;
        if (GetModuleFileNameExA(proc, nullptr, path, MAX_PATH)) {
            name = path;
            if (auto pos = name.find_last_of("\\/"); pos != std::string::npos)
                name = name.substr(pos + 1);
        }
        CloseHandle(proc);
        return name;
    }

    void SetPriorityByName(const std::string& name, DWORD priority) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32 entry{ sizeof(entry) };
        for (BOOL ok = Process32First(snap, &entry); ok;
            ok = Process32Next(snap, &entry)) {
            if (_stricmp(entry.szExeFile, name.c_str()) != 0) continue;
            if (HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE,
                entry.th32ProcessID)) {
                SetPriorityClass(h, priority);
                CloseHandle(h);
            }
        }
        CloseHandle(snap);
    }

} // namespace ProcessUtil

// ============================================================
// GAME MODE MANAGEMENT
// ============================================================

namespace GameMode {

    void Enter(const std::string& gameName) {
        if (g_app.gameModeActive) return;
        g_app.SetStatus("Activating Game Mode...");

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32 entry{ sizeof(entry) };
            for (BOOL ok = Process32First(snap, &entry); ok;
                ok = Process32Next(snap, &entry)) {
                for (const auto& target : g_app.processKillList) {
                    if (_stricmp(entry.szExeFile, target.c_str()) != 0) continue;
                    HANDLE proc = OpenProcess(
                        PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                        FALSE, entry.th32ProcessID);
                    if (proc) {
                        char path[MAX_PATH]{};
                        g_app.killedProcesses[entry.szExeFile] =
                            GetModuleFileNameExA(proc, nullptr, path, MAX_PATH)
                            ? path : entry.szExeFile;
                        TerminateProcess(proc, 0);
                        CloseHandle(proc);
                    }
                    break;
                }
            }
            CloseHandle(snap);
        }

        g_app.activeGameName = gameName;
        ProcessUtil::SetPriorityByName(gameName, HIGH_PRIORITY_CLASS);
        ProcessUtil::SetPriorityByName("svchost.exe", IDLE_PRIORITY_CLASS);
        g_app.gameModeActive = true;
        g_app.SetStatus("Game Mode Active - " + gameName);
    }

    void Exit() {
        if (!g_app.gameModeActive) return;
        g_app.SetStatus("Restoring Desktop...");

        if (!g_app.activeGameName.empty())
            ProcessUtil::SetPriorityByName(g_app.activeGameName, NORMAL_PRIORITY_CLASS);
        ProcessUtil::SetPriorityByName("svchost.exe", NORMAL_PRIORITY_CLASS);

        for (const auto& [name, path] : g_app.killedProcesses) {
            const char* cmd = (ToLower(name) == "explorer.exe")
                ? "explorer.exe" : path.c_str();
            ShellExecuteA(nullptr, "open", cmd, nullptr, nullptr, SW_SHOWDEFAULT);
            Sleep(200);
        }

        g_app.killedProcesses.clear();
        g_app.activeGameName.clear();
        g_app.gameModeActive = false;
        Sleep(2000);
        g_app.SetStatus("Ready - Monitoring for games");
    }

    void MonitorThreadFunc() {
        ProcessUtil::EnableDebugPrivilege();
        while (g_app.running) {
            const std::string fg = ToLower(ProcessUtil::GetForegroundProcessName());
            const bool isMonitored = g_app.IsGameInList(fg);

            if (isMonitored && !g_app.gameModeActive)
                Enter(fg);
            else if (!isMonitored && g_app.gameModeActive)
                Exit();

            Sleep(1000);
        }
    }

} // namespace GameMode

// ============================================================
// DRAWING PRIMITIVES
// ============================================================

namespace Draw {

    std::unique_ptr<GraphicsPath> RoundRectPath(const RectF& r, float radius) {
        auto path = std::make_unique<GraphicsPath>();
        float d = radius * 2;
        if (d > r.Width)  d = r.Width;
        if (d > r.Height) d = r.Height;
        path->AddArc(r.X, r.Y, d, d, 180, 90);
        path->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path->CloseFigure();
        return path;
    }

    void FillRoundRect(Graphics& gfx, const RectF& r, float rad, const Brush* b) {
        auto path = RoundRectPath(r, rad);
        gfx.FillPath(b, path.get());
    }

    void StrokeRoundRect(Graphics& gfx, const RectF& r, float rad, const Pen* p) {
        auto path = RoundRectPath(r, rad);
        gfx.DrawPath(p, path.get());
    }

    void SetupCentered(StringFormat& sf) {
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
    }

    void SetupLeftCentered(StringFormat& sf) {
        sf.SetAlignment(StringAlignmentNear);
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
    }

} // namespace Draw

// ============================================================
// UI COMPONENTS
// ============================================================

namespace UI {

    void Button(Graphics& gfx, const RectF& rect, const wchar_t* text,
        bool primary, float hover, float press, bool enabled = true)
    {
        const Color bgNormal = !enabled ? Theme::Disabled
            : primary ? Theme::BtnPrimary : Theme::BtnSecondary;
        const Color bgHover = !enabled ? Theme::Disabled
            : primary ? Theme::BtnPrimaryHover : Theme::BtnSecondaryHover;

        Color bg = LerpColor(bgNormal, bgHover, hover);
        bg = ApplyPressEffect(bg, press);

        SolidBrush brush(bg);
        Draw::FillRoundRect(gfx, rect, static_cast<float>(Layout::RadiusSm), &brush);

        if (!primary && enabled) {
            Pen pen(Theme::Border);
            Draw::StrokeRoundRect(gfx, rect,
                static_cast<float>(Layout::RadiusSm), &pen);
        }

        FontFamily ff(L"Segoe UI");
        Gdiplus::Font font(&ff, static_cast<REAL>(Layout::FontSizeBody), FontStyleBold, UnitPixel);
        SolidBrush tb(enabled ? Theme::TextPrimary : Theme::TextDisabled);
        StringFormat sf;
        Draw::SetupCentered(sf);
        gfx.DrawString(text, -1, &font, rect, &sf, &tb);
    }

    void GameItem(Graphics& gfx, const RectF& rect,
        const std::string& name, bool selected, bool hovered)
    {
        if (selected) {
            SolidBrush bg(Theme::AccentMuted);
            Draw::FillRoundRect(gfx, rect, static_cast<float>(Layout::RadiusSm), &bg);
            Pen border(Theme::Accent, 1.5f);
            Draw::StrokeRoundRect(gfx, rect, static_cast<float>(Layout::RadiusSm), &border);
        }
        else if (hovered) {
            SolidBrush bg(Theme::BgCardHover);
            Draw::FillRoundRect(gfx, rect, static_cast<float>(Layout::RadiusSm), &bg);
        }

        // Icon circle
        constexpr float iconSz = 32.f;
        const float ix = rect.X + static_cast<float>(Layout::GapSm), iy = rect.Y + (rect.Height - iconSz) / 2;
        const RectF iconR(ix, iy, iconSz, iconSz);

        SolidBrush iconBg(selected ? Theme::Accent : Theme::BgSecondary);
        Draw::FillRoundRect(gfx, iconR, static_cast<float>(Layout::RadiusSm), &iconBg);

        FontFamily ff(L"Segoe UI");
        Gdiplus::Font iconFont(&ff, static_cast<REAL>(Layout::FontSizeBody + 1), FontStyleBold, UnitPixel);
        const wchar_t letter[2] = {
            name.empty() ? L'G' : static_cast<wchar_t>(toupper(name[0])), 0
        };
        SolidBrush letterBrush(selected ? Theme::TextPrimary : Theme::TextSecondary);
        StringFormat cf;
        Draw::SetupCentered(cf);
        gfx.DrawString(letter, 1, &iconFont, iconR, &cf, &letterBrush);

        // Name text
        const float tx = ix + iconSz + static_cast<float>(Layout::GapSm);
        RectF textR(tx, rect.Y, rect.Width - tx + rect.X - static_cast<float>(Layout::GapSm), rect.Height);
        Gdiplus::Font nameFont(&ff, static_cast<REAL>(Layout::FontSizeBody), FontStyleRegular, UnitPixel);
        SolidBrush nameBrush(Theme::TextPrimary);
        StringFormat tf;
        Draw::SetupLeftCentered(tf);
        gfx.DrawString(ToWide(name).c_str(), -1, &nameFont, textR, &tf, &nameBrush);
    }

    void StatusBar(Graphics& gfx, const RectF& rect, bool active,
        float pulse, const std::string& text)
    {
        SolidBrush bg(Theme::BgCard);
        Draw::FillRoundRect(gfx, rect, static_cast<float>(Layout::RadiusSm), &bg);
        Pen border(Theme::Border);
        Draw::StrokeRoundRect(gfx, rect, static_cast<float>(Layout::RadiusSm), &border);

        constexpr float dotSz = 10.f;
        const float dx = rect.X + static_cast<float>(Layout::Gap), dy = rect.Y + (rect.Height - dotSz) / 2;

        if (active && pulse > 0) {
            const float glow = dotSz + 8 * pulse;
            SolidBrush gb(Color(static_cast<BYTE>(60 * pulse),
                Theme::StatusActive.GetR(),
                Theme::StatusActive.GetG(),
                Theme::StatusActive.GetB()));
            gfx.FillEllipse(&gb, RectF(dx - (glow - dotSz) / 2,
                dy - (glow - dotSz) / 2, glow, glow));
        }

        SolidBrush dotBrush(active ? Theme::StatusActive : Theme::StatusReady);
        gfx.FillEllipse(&dotBrush, RectF(dx, dy, dotSz, dotSz));

        FontFamily ff(L"Segoe UI");
        Gdiplus::Font font(&ff, static_cast<REAL>(Layout::FontSizeStatus), FontStyleRegular, UnitPixel);
        RectF tr(dx + dotSz + static_cast<float>(Layout::GapSm), rect.Y,
            rect.Width - dx - dotSz - static_cast<float>(Layout::GapSm + Layout::Gap), rect.Height);
        SolidBrush tb(Theme::TextSecondary);
        StringFormat sf;
        Draw::SetupLeftCentered(sf);
        gfx.DrawString(ToWide(text).c_str(), -1, &font, tr, &sf, &tb);
    }

} // namespace UI

// ============================================================
// PAINT SECTIONS
// ============================================================

namespace Painter {

    void Header(Graphics& gfx, const Metrics& m) {
        FontFamily ff(L"Segoe UI");

        Gdiplus::Font titleFont(&ff, static_cast<REAL>(Layout::FontSizeTitle), FontStyleBold, UnitPixel);
        SolidBrush titleBrush(Theme::TextPrimary);
        gfx.DrawString(L"Game Booster", -1, &titleFont,
            PointF(m.x, m.titleY), &titleBrush);

        Gdiplus::Font subFont(&ff, static_cast<REAL>(Layout::FontSizeBody), FontStyleRegular, UnitPixel);
        SolidBrush subBrush(Theme::TextSecondary);
        gfx.DrawString(L"Optimize your system for gaming", -1, &subFont,
            PointF(m.x, m.subtitleY), &subBrush);

        Color accentTransparent(0, Theme::Accent.GetR(), Theme::Accent.GetG(), Theme::Accent.GetB());
        LinearGradientBrush lb(PointF(m.x, m.lineY), PointF(m.x + 80, m.lineY),
            Theme::Accent, accentTransparent);
        Pen lp(&lb, 3);
        lp.SetStartCap(LineCapRound);
        lp.SetEndCap(LineCapRound);
        gfx.DrawLine(&lp, m.x, m.lineY, m.x + 80, m.lineY);
    }

    void AddSection(Graphics& gfx, const Metrics& m) {
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font labelFont(&ff, static_cast<REAL>(Layout::FontSizeLabel), FontStyleBold, UnitPixel);
        SolidBrush labelBrush(Theme::TextMuted);
        gfx.DrawString(L"ADD GAME", -1, &labelFont,
            PointF(m.x, m.addLabelY), &labelBrush);

        // Input field background
        const Color inputBg = g_app.inputFocused ? Theme::BgInputFocus : Theme::BgInput;
        SolidBrush ib(inputBg);
        Draw::FillRoundRect(gfx, m.inputRect,
            static_cast<float>(Layout::RadiusSm), &ib);

        const Color borderCol = g_app.inputFocused ? Theme::BorderFocus : Theme::Border;
        Pen bp(borderCol, g_app.inputFocused ? 2.f : 1.f);
        Draw::StrokeRoundRect(gfx, m.inputRect,
            static_cast<float>(Layout::RadiusSm), &bp);

        // Placeholder text [REMOVED]

        const auto& anim = g_app.buttonAnims[ID_BTN_ADD];
        UI::Button(gfx, m.addBtnRect, L"+ Add", true, anim.hover, anim.press);
    }

    void GameList(Graphics& gfx, const Metrics& m) {
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font labelFont(&ff, static_cast<REAL>(Layout::FontSizeLabel), FontStyleBold, UnitPixel);
        SolidBrush labelBrush(Theme::TextMuted);
        gfx.DrawString(L"MONITORED GAMES", -1, &labelFont,
            PointF(m.x, m.listLabelY), &labelBrush);

        std::lock_guard lock(g_app.gamesMutex);
        const int count = static_cast<int>(g_app.games.size());

        // Count badge
        const std::wstring countStr = std::to_wstring(count);
        RectF measureR;
        gfx.MeasureString(countStr.c_str(), -1, &labelFont, PointF(0, 0), &measureR);
        float badgeW = measureR.Width + 14;
        if (badgeW < 22.f) badgeW = 22.f;
        RectF badgeR(m.x + 130, m.listLabelY - 2, badgeW, 18);
        SolidBrush badgeBg(Theme::AccentMuted);
        Draw::FillRoundRect(gfx, badgeR, static_cast<float>(Layout::RadiusXs), &badgeBg);
        SolidBrush badgeText(Theme::Accent);
        StringFormat cf;
        Draw::SetupCentered(cf);
        gfx.DrawString(countStr.c_str(), -1, &labelFont, badgeR, &cf, &badgeText);

        // List card background
        SolidBrush cardBg(Theme::BgCard);
        Draw::FillRoundRect(gfx, m.listRect,
            static_cast<float>(Layout::RadiusLg), &cardBg);
        Pen cardBorder(Theme::Border);
        Draw::StrokeRoundRect(gfx, m.listRect,
            static_cast<float>(Layout::RadiusLg), &cardBorder);

        // Empty state
        if (g_app.games.empty()) {
            Gdiplus::Font ef(&ff, static_cast<REAL>(Layout::FontSizeBody), FontStyleItalic, UnitPixel);
            SolidBrush eb(Theme::TextMuted);
            StringFormat esf;
            Draw::SetupCentered(esf);
            gfx.DrawString(L"No games added yet", -1, &ef, m.listRect, &esf, &eb);
            return;
        }

        // Clipped item drawing
        const RectF clip(m.listRect.X + Layout::ListPadX,
            m.listRect.Y + Layout::ListPadY,
            m.listRect.Width - Layout::ListPadX * 2,
            m.listRect.Height - Layout::ListPadY * 2);
        gfx.SetClip(clip);

        float itemY = m.listRect.Y + Layout::ListPadY
            - static_cast<float>(g_app.scrollY);
        for (int i = 0; i < count; ++i, itemY += Layout::ItemH) {
            if (itemY + Layout::ItemH < clip.Y)    continue;
            if (itemY > clip.Y + clip.Height)      break;
            RectF itemR(clip.X, itemY, clip.Width,
                static_cast<float>(Layout::ItemH) - 4);
            UI::GameItem(gfx, itemR, g_app.games[i],
                g_app.selectedItem == i, g_app.hoveredItem == i);
        }
        gfx.ResetClip();

        // Scrollbar
        const int totalH = count * Layout::ItemH;
        const int visibleH = static_cast<int>(clip.Height);
        if (totalH > visibleH) {
            const float ratio = static_cast<float>(visibleH) / totalH;
            float barH = visibleH * ratio;
            if (barH < 30.f) barH = 30.f;
            const float maxS = static_cast<float>(totalH - visibleH);
            const float pos = maxS > 0 ? g_app.scrollY / maxS : 0.f;
            const float barY = clip.Y + pos * (visibleH - barH);
            RectF barR(m.listRect.X + m.listRect.Width - 8.f,
                barY, 4.f, barH);
            SolidBrush sb(Theme::Scrollbar);
            Draw::FillRoundRect(gfx, barR, 2, &sb);
        }
    }

    void Footer(Graphics& gfx, const Metrics& m) {
        const auto& anim = g_app.buttonAnims[ID_BTN_REMOVE];
        UI::Button(gfx, m.removeBtnRect, L"Remove", false,
            anim.hover, anim.press, g_app.selectedItem >= 0);
        UI::StatusBar(gfx, m.statusRect,
            g_app.gameModeActive, g_app.pulseValue, g_app.GetStatus());
    }

} // namespace Painter

// ============================================================
// MAIN PAINT
// ============================================================

static void Paint(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    DoubleBuffer buffer(hdc, rc.right, rc.bottom);
    Graphics gfx(buffer.GetDC());
    gfx.SetSmoothingMode(SmoothingModeHighQuality);
    gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    SolidBrush bg(Theme::BgPrimary);
    gfx.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

    Painter::Header(gfx, g_app.metrics);
    Painter::AddSection(gfx, g_app.metrics);
    Painter::GameList(gfx, g_app.metrics);
    Painter::Footer(gfx, g_app.metrics);
}

// ============================================================
// EDIT CONTROL SUBCLASS
// ============================================================

static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg,
    WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETFOCUS:
        g_app.inputFocused = true;
        g_app.RequestRedraw();
        break;
    case WM_KILLFOCUS:
        g_app.inputFocused = false;
        g_app.RequestRedraw();
        break;
    case WM_CHAR:
        g_app.RequestRedraw();
        break;
    case WM_KEYDOWN:
        g_app.RequestRedraw();
        if (wp == VK_RETURN) {
            SendMessage(GetParent(hwnd), WM_COMMAND,
                MAKEWPARAM(ID_BTN_ADD, BN_CLICKED), 0);
            return 0;
        }
        break;
    }
    return CallWindowProc(g_app.origEditProc, hwnd, msg, wp, lp);
}

// ============================================================
// ANIMATION UPDATE
// ============================================================

static bool UpdateAnimations() {
    bool dirty = false;

    for (auto& [id, anim] : g_app.buttonAnims) {
        const float th = (g_app.hoveredButton == id) ? 1.f : 0.f;
        const float tp = (g_app.pressedButton == id) ? 1.f : 0.f;

        if (std::abs(anim.hover - th) > 0.01f) {
            anim.hover = Lerp(anim.hover, th, ANIM_SPEED);
            dirty = true;
        }
        if (std::abs(anim.press - tp) > 0.01f) {
            anim.press = Lerp(anim.press, tp, ANIM_SPEED * 2.f);
            dirty = true;
        }
    }

    g_app.pulsePhase += 0.08f;
    const float target = g_app.gameModeActive
        ? (sinf(g_app.pulsePhase) + 1.f) / 2.f : 0.f;

    if (std::abs(g_app.pulseValue - target) > 0.01f) {
        g_app.pulseValue = g_app.gameModeActive
            ? target : Lerp(g_app.pulseValue, 0.f, ANIM_SPEED);
        dirty = true;
    }
    return dirty;
}

// ============================================================
// WINDOW PROCEDURE HELPERS
// ============================================================

static void OnCreate(HWND hwnd) {
    g_app.hWnd = hwnd;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DwmAttrib::ImmersiveDarkMode,
        &dark, sizeof(dark));

    RECT rc;
    GetClientRect(hwnd, &rc);
    g_app.metrics.Compute(rc.right, rc.bottom);
    g_app.CreateResources();

    int ex, ey, ew, eh;
    g_app.metrics.GetEditPosition(ex, ey, ew, eh);
    g_app.hInput = CreateWindowExA(
        0, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        ex, ey, ew, eh, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_INPUT)),
        nullptr, nullptr);
    SendMessage(g_app.hInput, WM_SETFONT,
        reinterpret_cast<WPARAM>(g_app.hFont), TRUE);
    g_app.origEditProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(g_app.hInput, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(InputProc)));

    SendMessageW(g_app.hInput, EM_SETCUEBANNER, TRUE,
        (LPARAM)L"Enter game executable (e.g., game.exe)");

    g_app.AddTrayIcon();
    g_app.buttonAnims[ID_BTN_ADD] = {};
    g_app.buttonAnims[ID_BTN_REMOVE] = {};
    SetTimer(hwnd, TIMER_ANIM, 16, nullptr);
}

static void OnMouseMove(HWND hwnd, int mx, int my) {
    const int newBtn = g_app.metrics.HitTestButton(mx, my);
    int newItem;
    {
        std::lock_guard lock(g_app.gamesMutex);
        newItem = g_app.metrics.HitTestItem(
            mx, my, static_cast<int>(g_app.games.size()), g_app.scrollY);
    }

    if (newBtn != g_app.hoveredButton || newItem != g_app.hoveredItem) {
        g_app.hoveredButton = newBtn;
        g_app.hoveredItem = newItem;
        g_app.RequestRedraw();
    }

    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
    TrackMouseEvent(&tme);
}

static void OnCommand(int id) {
    switch (id) {
    case ID_BTN_ADD: {
        char buf[256]{};
        GetWindowTextA(g_app.hInput, buf, sizeof(buf));
        if (g_app.AddGame(buf)) {
            SetWindowTextA(g_app.hInput, "");
            g_app.SaveGames();
            g_app.RequestRedraw();
        }
    } break;
    case ID_BTN_REMOVE:
        if (g_app.RemoveSelected()) {
            g_app.SaveGames();
            g_app.RequestRedraw();
        }
        break;
    }
}

// ============================================================
// WINDOW PROCEDURE
// ============================================================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
    WPARAM wp, LPARAM lp) {
    if (msg == WM_TASKBARCREATED && WM_TASKBARCREATED != 0) {
        g_app.AddTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        OnCreate(hwnd);
        break;

    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_app.UpdateLayout(rc.right, rc.bottom);
        g_app.RequestRedraw();
    } break;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = { Layout::MinWindowW, Layout::MinWindowH };
    } break;

    case WM_TIMER:
        if (wp == TIMER_ANIM && UpdateAnimations())
            g_app.RequestRedraw();
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
    } return 0;

    case WM_CTLCOLOREDIT: {
        auto hdc = reinterpret_cast<HDC>(wp);
        SetTextColor(hdc, Theme::EditText);
        SetBkColor(hdc, Theme::EditBg);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_app.hEditBrush);
    }

    case WM_MOUSEMOVE:
        OnMouseMove(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;

    case WM_MOUSELEAVE:
        g_app.hoveredButton = -1;
        g_app.hoveredItem = -1;
        g_app.RequestRedraw();
        break;

    case WM_LBUTTONDOWN: {
        const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (int btn = g_app.metrics.HitTestButton(mx, my); btn >= 0) {
            g_app.pressedButton = btn;
            g_app.RequestRedraw();
        }
        int item;
        {
            std::lock_guard lock(g_app.gamesMutex);
            item = g_app.metrics.HitTestItem(
                mx, my, static_cast<int>(g_app.games.size()), g_app.scrollY);
        }
        if (item >= 0 && item != g_app.selectedItem) {
            g_app.selectedItem = item;
            g_app.RequestRedraw();
        }
    } break;

    case WM_LBUTTONUP: {
        const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (int btn = g_app.metrics.HitTestButton(mx, my);
            btn == g_app.pressedButton && btn >= 0)
            SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(btn, BN_CLICKED), 0);
        g_app.pressedButton = -1;
        g_app.RequestRedraw();
    } break;

    case WM_MOUSEWHEEL:
        g_app.scrollY -= GET_WHEEL_DELTA_WPARAM(wp) / 3;
        g_app.ClampScroll();
        g_app.RequestRedraw();
        break;

    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        break;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        break;

    case WM_DESTROY:
        g_app.RemoveTrayIcon();
        KillTimer(hwnd, TIMER_ANIM);
        g_app.DestroyResources();
        g_app.running = false;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

// ============================================================
// ENTRY POINT
// ============================================================

int APIENTRY WinMain(
    _In_ HINSTANCE hInst,
    _In_opt_ HINSTANCE,
    _In_ LPSTR,
    _In_ int nShow)
{
    GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken;
    GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WM_TASKBARCREATED = RegisterWindowMessageA("TaskbarCreated");
    g_app.LoadGames();

    std::thread monitor(GameMode::MonitorThreadFunc);

    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "GameBoosterUI";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassA(&wc);

    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT wr{ 0, 0, Layout::InitialW, Layout::InitialH };
    AdjustWindowRect(&wr, style, FALSE);

    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName, "Game Booster", style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd) {
        g_app.running = false;
        if (monitor.joinable()) monitor.join();
        GdiplusShutdown(gdipToken);
        return 1;
    }

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (monitor.joinable()) monitor.join();
    GdiplusShutdown(gdipToken);
    return 0;
}