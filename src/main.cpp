#include "hotspot/localization.hpp"
#include "hotspot/hotspot_service.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <winrt/Windows.Foundation.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using hotspot::Tr;
constexpr wchar_t WindowClass[] = L"WiFiStudio.MainWindow";
constexpr UINT ResultMessage = WM_APP + 1;
constexpr UINT_PTR PollTimer = 1;
constexpr COLORREF Ink = RGB(24, 50, 71);
constexpr COLORREF Muted = RGB(78, 100, 119);
constexpr COLORREF Accent = RGB(8, 119, 133);
constexpr COLORREF Line = RGB(213, 225, 233);
constexpr COLORREF Side = RGB(237, 245, 247);
constexpr COLORREF Paper = RGB(255, 255, 255);

enum ControlId {
    Adapter = 1001, Source, Ssid, Password, Band, Security, Priority, ShowPassword,
    Generate, Refresh, StartStop, WindowsSettings, Log, Clients, Status, AdapterDetail,
    Notice, ClientLabel
};
enum class JobKind { Discover, Poll, Start, Stop };
struct Job {
    JobKind kind = JobKind::Discover;
    hotspot::Settings settings;
    std::wstring adapter;
    std::wstring source;
};
struct Result {
    JobKind kind;
    hotspot::Snapshot snapshot;
    std::wstring error;
};

// Serializing all WinRT access also prevents Start, Stop, and timer polling from racing.
class BackendWorker {
public:
    explicit BackendWorker(HWND window) : window_(window), thread_([this](std::stop_token stop) { Run(stop); }) {}
    ~BackendWorker() {
        thread_.request_stop();
        ready_.notify_all();
        thread_.join();
    }
    void Submit(Job job) {
        { std::lock_guard lock(mutex_); jobs_.push_back(std::move(job)); }
        ready_.notify_one();
    }
    std::optional<Result> TakeResult() {
        std::lock_guard lock(mutex_);
        if (results_.empty()) return {};
        auto result = std::move(results_.front());
        results_.pop_front();
        return result;
    }
private:
    void Run(std::stop_token stop) {
        std::unique_ptr<hotspot::HotspotService> service;
        std::wstring initializationError;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            service = std::make_unique<hotspot::HotspotService>();
        } catch (...) { initializationError = hotspot::CurrentExceptionMessage(); }
        hotspot::Snapshot last;
        while (!stop.stop_requested()) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stop.stop_requested() || !jobs_.empty(); });
                if (stop.stop_requested()) break;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            Result result{job.kind, last, initializationError};
            if (service) {
                try {
                    switch (job.kind) {
                    case JobKind::Discover: result.snapshot = service->Discover(job.adapter, job.source); break;
                    case JobKind::Poll: result.snapshot = service->Poll(); break;
                    case JobKind::Start: result.snapshot = service->Start(job.settings, job.adapter, job.source); break;
                    case JobKind::Stop: result.snapshot = service->Stop(); break;
                    }
                } catch (...) {
                    result.error = hotspot::CurrentExceptionMessage();
                    try { result.snapshot = service->Poll(); } catch (...) {
                        result.snapshot.state = hotspot::State::Unknown;
                        result.snapshot.owned = service->OwnsSession();
                    }
                }
                last = result.snapshot;
            }
            // No posted heap pointers: destroying a window cannot leak a pending result.
            {
                std::lock_guard lock(mutex_);
                results_.push_back(std::move(result));
            }
            PostMessageW(window_, ResultMessage, 0, 0);
        }
        service.reset(); // Cleanup on the same MTA thread, before uninitializing COM.
        if (initializationError.empty()) winrt::uninit_apartment();
    }
    HWND window_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    std::deque<Result> results_;
    std::jthread thread_;
};

std::wstring Text(HWND control) {
    const auto length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}
void SetText(HWND control, const std::wstring& text) { SetWindowTextW(control, text.c_str()); }

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}
    ~App() {
        worker_.reset();
        DeleteFonts();
        DeleteObject(paperBrush_);
        DeleteObject(sideBrush_);
    }
    bool Create(int show) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
        wc.hbrBackground = paperBrush_;
        wc.lpszClassName = WindowClass;
        if (!RegisterClassExW(&wc)) return false;
        const auto dpi = GetDpiForSystem();
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = (std::min)(MulDiv(1080, static_cast<int>(dpi), 96), static_cast<int>(work.right - work.left - 24));
        const int height = (std::min)(MulDiv(830, static_cast<int>(dpi), 96), static_cast<int>(work.bottom - work.top - 24));
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, Tr(L"WiFi Studio — точка доступа Windows 11", L"WiFi Studio — Windows 11 hotspot"),
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL, work.left + (work.right - work.left - width) / 2,
            work.top + (work.bottom - work.top - height) / 2, width, height,
            nullptr, nullptr, instance_, this);
        if (!window_) return false;
        ShowWindow(window_, show);
        UpdateWindow(window_);
        return true;
    }
    HWND Window() const { return window_; }
    void RefreshAdapters() {
        if (busy_) { if (activeJob_ == JobKind::Poll) refreshAfterPoll_ = true; return; }
        Job job;
        job.kind = JobKind::Discover;
        job.adapter = SelectedAdapter();
        job.source = SelectedSource();
        Queue(std::move(job));
    }
private:
    HWND Control(int id) const { return GetDlgItem(window_, id); }
    int Px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    int Dip(int value) const { return MulDiv(value, 96, static_cast<int>(dpi_)); }
    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        const auto control = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!control) throw winrt::hresult_error(E_FAIL, Tr(L"Не удалось создать элемент интерфейса.", L"Could not create a window control."));
        SetWindowTheme(control, L"Explorer", nullptr);
        controls_.push_back(control);
        return control;
    }
    HWND Label(const wchar_t* text, int id = -1) { return Add(WC_STATICW, text, SS_LEFT, id); }
    HWND Button(const wchar_t* text, int id) { return Add(WC_BUTTONW, text, WS_TABSTOP | BS_OWNERDRAW, id); }
    HWND Combo(int id) { return Add(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS, id); }
    void Move(HWND control, int x, int y, int width, int height) {
        MoveWindow(control, Px(x), Px(y) - scrollOffset_, Px(width), Px(height), TRUE);
    }
    void DeleteFonts() {
        for (const auto font : fonts_) DeleteObject(font);
        fonts_.clear();
    }
    HFONT Font(int size, int weight = FW_NORMAL, const wchar_t* face = L"Segoe UI Variable Text") {
        const auto font = CreateFontW(-Px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, face);
        if (!font) throw winrt::hresult_error(E_FAIL, Tr(L"Не удалось создать шрифт интерфейса.", L"Could not create a UI font."));
        fonts_.push_back(font);
        return font;
    }
    void ConfigureFonts() {
        DeleteFonts();
        body_ = Font(14);
        small_ = Font(12);
        heading_ = Font(18, FW_SEMIBOLD);
        titleFont_ = Font(30, FW_SEMIBOLD, L"Segoe UI Variable Display");
        glyph_ = Font(29, FW_NORMAL, L"Segoe Fluent Icons");
        for (const auto control : controls_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body_), TRUE);
        for (const auto control : {subtitle_, adapterDetail_, hint_, Control(Notice), sessionHint_}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(small_), TRUE);
        }
        for (const auto control : {title_, connectionsHeading_, settingsHeading_, Control(Status)}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(control == title_ ? titleFont_ : heading_), TRUE);
        }
        SendMessageW(Control(Log), WM_SETFONT, reinterpret_cast<WPARAM>(small_), TRUE);
        SendMessageW(Control(Band), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(23));
        SendMessageW(Control(Security), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(23));
        SendMessageW(Control(Adapter), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(23));
        SendMessageW(Control(Source), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(23));
    }
    void Initialize() {
        dpi_ = GetDpiForWindow(window_);
        const BOOL dark = FALSE;
        const auto backdrop = DWMSBT_MAINWINDOW;
        const auto corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(window_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
        DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

        title_ = Label(Tr(L"Точка доступа", L"Wi-Fi hotspot"));
        subtitle_ = Label(Tr(L"WiFi Studio   /   Мобильный хот-спот Windows 11", L"WiFi Studio   /   Windows 11 Mobile Hotspot"));
        connectionsHeading_ = Label(Tr(L"Подключение", L"Connection"));
        Button(Tr(L"Обновить", L"Refresh"), Refresh);
        adapterLabel_ = Label(Tr(L"Wi-Fi-адаптер для &раздачи", L"Hotspot Wi-Fi &adapter")); Combo(Adapter);
        adapterDetail_ = Label(Tr(L"Поиск адаптеров…", L"Finding adapters…"), AdapterDetail);
        sourceLabel_ = Label(Tr(L"&Источник Интернета", L"Internet &source")); Combo(Source);
        settingsHeading_ = Label(Tr(L"Параметры сети", L"Network settings"));
        ssidLabel_ = Label(Tr(L"&Имя сети (SSID)", L"Network &name (SSID)"));
        Add(WC_EDITW, L"WiFi-Studio", WS_TABSTOP | ES_AUTOHSCROLL, Ssid, WS_EX_CLIENTEDGE);
        // Let the validator see oversized input instead of silently truncating to a valid value.
        SendMessageW(Control(Ssid), EM_SETLIMITTEXT, 256, 0);
        passwordLabel_ = Label(Tr(L"&Пароль сети", L"Network &password"));
        Add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, Password, WS_EX_CLIENTEDGE);
        SendMessageW(Control(Password), EM_SETLIMITTEXT, 256, 0);
        SendMessageW(Control(Password), EM_SETPASSWORDCHAR, L'\x25CF', 0);
        Button(Tr(L"Показать", L"Show"), ShowPassword);
        Button(Tr(L"Новый", L"New"), Generate);
        bandLabel_ = Label(Tr(L"&Диапазон", L"Frequency &band")); Combo(Band);
        securityLabel_ = Label(Tr(L"&Защита", L"Securit&y")); Combo(Security);
        for (const auto band : {hotspot::Band::Automatic, hotspot::Band::Ghz24, hotspot::Band::Ghz5, hotspot::Band::Ghz6}) {
            const auto text = hotspot::BandLabel(band);
            SendMessageW(Control(Band), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        for (const auto security : {hotspot::Security::Wpa2, hotspot::Security::Wpa3Transition, hotspot::Security::Wpa3}) {
            const auto text = hotspot::SecurityLabel(security);
            SendMessageW(Control(Security), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        SendMessageW(Control(Band), CB_SETCURSEL, 0, 0);
        SendMessageW(Control(Security), CB_SETCURSEL, 0, 0);
        Add(WC_BUTTONW, Tr(L"Приоритет точки доступа над Wi-Fi-подключением", L"Prioritize the hotspot over the Wi-Fi connection"), WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE, Priority);
        hint_ = Label(Tr(L"Имя: 1–32 ASCII-символа. Пароль: 8–63. Для 6 ГГц нужна WPA3.", L"Name: 1–32 ASCII characters. Password: 8–63. 6 GHz requires WPA3."));
        statusCaption_ = Label(Tr(L"Состояние сети", L"Network status"));
        Label(Tr(L"Проверка подключения", L"Checking connection"), Status);
        Label(Tr(L"Настройки применяются только к этой сессии. Параметры системного хот-спота сохраняются.", L"Settings apply to this session only. Saved Windows hotspot settings are preserved."), Notice);
        Label(Tr(L"Подключённые устройства", L"Connected devices"), ClientLabel);
        Add(WC_LISTBOXW, L"", WS_TABSTOP | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, Clients, WS_EX_CLIENTEDGE);
        sessionHint_ = Label(Tr(L"Закрытие приложения остановит созданную им точку доступа.", L"Closing this app stops the hotspot it created."));
        Button(Tr(L"Включить точку доступа", L"Start hotspot"), StartStop);
        Button(Tr(L"Параметры хот-спота Windows", L"Windows hotspot settings"), WindowsSettings);
        logLabel_ = Label(Tr(L"Журнал работы", L"Activity log"));
        Add(WC_EDITW, L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            Log, WS_EX_CLIENTEDGE);
        SendMessageW(Control(Log), EM_SETLIMITTEXT, 64000, 0);
        ConfigureFonts();
        SetText(Control(Password), hotspot::GeneratePassword());
        Layout();
        AppendLog(Tr(L"Современный сессионный API Windows 11. Пароли не записываются в журнал или файлы.", L"Windows 11 per-session hotspot API. Passwords are never logged or written to files."));
        worker_ = std::make_unique<BackendWorker>(window_);
        Queue(Job{});
        SetTimer(window_, PollTimer, 3000, nullptr);
    }
    void Layout() {
        if (controls_.empty() || layingOut_) return;
        layingOut_ = true;
        RECT client{}; GetClientRect(window_, &client);
        SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
        scroll.nMin = 0;
        scroll.nMax = (std::max)(static_cast<int>(client.bottom), Px(740)) - 1;
        scroll.nPage = static_cast<UINT>(client.bottom);
        scroll.nPos = scrollOffset_;
        SetScrollInfo(window_, SB_VERT, &scroll, TRUE);
        scroll.fMask = SIF_POS;
        GetScrollInfo(window_, SB_VERT, &scroll);
        scrollOffset_ = scroll.nPos;
        GetClientRect(window_, &client); // Showing/hiding the scrollbar changes the width.
        const int w = Dip(client.right), h = (std::max)(740, Dip(client.bottom));
        const int right = w - 314;
        const int leftWidth = right - 64;
        Move(title_, 86, 15, w - 120, 44);
        Move(subtitle_, 88, 58, w - 120, 22);
        Move(connectionsHeading_, 28, 108, leftWidth - 120, 30);
        Move(Control(Refresh), 28 + leftWidth - 100, 106, 100, 30);
        Move(adapterLabel_, 28, 146, leftWidth, 24);
        Move(Control(Adapter), 28, 170, leftWidth, 210);
        Move(adapterDetail_, 30, 204, leftWidth - 4, 35);
        Move(sourceLabel_, 28, 245, leftWidth, 24);
        Move(Control(Source), 28, 269, leftWidth, 210);
        Move(settingsHeading_, 28, 313, leftWidth, 30);
        Move(ssidLabel_, 28, 351, leftWidth, 24);
        Move(Control(Ssid), 28, 376, leftWidth, 32);
        Move(passwordLabel_, 28, 422, leftWidth, 24);
        Move(Control(Password), 28, 447, leftWidth - 190, 32);
        Move(Control(ShowPassword), 28 + leftWidth - 180, 447, 96, 32);
        Move(Control(Generate), 28 + leftWidth - 74, 447, 74, 32);
        const int half = (leftWidth - 16) / 2;
        Move(bandLabel_, 28, 493, half, 24);
        Move(securityLabel_, 28 + half + 16, 493, half, 24);
        Move(Control(Band), 28, 517, half, 180);
        Move(Control(Security), 28 + half + 16, 517, half, 180);
        Move(Control(Priority), 28, 558, leftWidth, 27);
        Move(hint_, 30, 590, leftWidth, 23);
        Move(statusCaption_, right, 123, 270, 24);
        Move(Control(Status), right, 156, 270, 56);
        Move(Control(Notice), right, 220, 270, 98);
        Move(Control(ClientLabel), right, 330, 270, 43);
        Move(Control(Clients), right, 378, 270, 113);
        Move(sessionHint_, right, 501, 270, 40);
        Move(Control(StartStop), right, 551, 270, 44);
        Move(Control(WindowsSettings), right, 605, 270, 30);
        Move(logLabel_, 28, 635, leftWidth, 24);
        Move(Control(Log), 28, 662, w - 56, (std::max)(46, h - 682));
        layingOut_ = false;
        InvalidateRect(window_, nullptr, TRUE);
    }
    void ScrollTo(int position) {
        SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE};
        GetScrollInfo(window_, SB_VERT, &scroll);
        const int maximum = (std::max)(0, scroll.nMax - static_cast<int>(scroll.nPage) + 1);
        const int next = std::clamp(position, 0, maximum);
        if (next != scrollOffset_) { scrollOffset_ = next; Layout(); }
    }
    void AppendLog(const std::wstring& message) {
        const auto control = Control(Log);
        if (GetWindowTextLengthW(control) > 42000) {
            auto text = Text(control);
            text.erase(0, text.find(L'\n', 16000) + 1);
            SetText(control, text);
        }
        SYSTEMTIME time{}; GetLocalTime(&time);
        wchar_t prefix[32]{};
        swprintf_s(prefix, L"%02u:%02u:%02u  ", time.wHour, time.wMinute, time.wSecond);
        const auto line = std::wstring(prefix) + message + L"\r\n";
        SendMessageW(control, EM_SETSEL, static_cast<WPARAM>(-1), -1);
        SendMessageW(control, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageW(control, EM_SCROLLCARET, 0, 0);
    }
    std::wstring SelectedAdapter() const {
        const auto selected = static_cast<int>(SendMessageW(Control(Adapter), CB_GETCURSEL, 0, 0));
        if (selected < 0 || static_cast<size_t>(selected) >= snapshot_.adapters.size()) return {};
        return snapshot_.adapters[static_cast<size_t>(selected)].id;
    }
    std::wstring SelectedSource() const {
        const auto selected = static_cast<int>(SendMessageW(Control(Source), CB_GETCURSEL, 0, 0));
        if (selected < 0 || static_cast<size_t>(selected) >= snapshot_.sources.size()) return {};
        return snapshot_.sources[static_cast<size_t>(selected)].id;
    }
    void Populate() {
        SendMessageW(Control(Adapter), CB_RESETCONTENT, 0, 0);
        SendMessageW(Control(Source), CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < snapshot_.adapters.size(); ++i) {
            const auto& a = snapshot_.adapters[i];
            const auto label = a.name + (a.available ? L"" : Tr(L" (недоступен)", L" (unavailable)"));
            SendMessageW(Control(Adapter), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (a.id == snapshot_.adapterId) SendMessageW(Control(Adapter), CB_SETCURSEL, i, 0);
        }
        for (size_t i = 0; i < snapshot_.sources.size(); ++i) {
            const auto& s = snapshot_.sources[i];
            const auto label = s.name + (s.internet ? Tr(L" — Интернет", L" — Internet") : Tr(L" — локальное подключение", L" — local connection"));
            SendMessageW(Control(Source), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (s.id == snapshot_.sourceId) SendMessageW(Control(Source), CB_SETCURSEL, i, 0);
        }
        std::wstring detail = Tr(L"Адаптер не выбран или отключён. Нажмите «Обновить».", L"No adapter selected or the adapter is disconnected. Click Refresh.");
        for (const auto& a : snapshot_.adapters) if (a.id == snapshot_.adapterId) {
            detail = a.description + L"\r\n" + a.connectionState;
            break;
        }
        SetText(adapterDetail_, detail);
    }
    void UpdateState() {
        using hotspot::State;
        std::wstring label;
        if (busy_ && activeJob_ == JobKind::Start) label = Tr(L"Запуск сети…", L"Starting hotspot…");
        else if (busy_ && activeJob_ == JobKind::Stop) label = Tr(L"Остановка сети…", L"Stopping hotspot…");
        else if (busy_ && activeJob_ == JobKind::Discover) label = Tr(L"Поиск адаптеров…", L"Finding adapters…");
        else if (snapshot_.state == State::On) label = snapshot_.owned ? Tr(L"В эфире", L"On air") : Tr(L"Уже включена", L"Already on");
        else if (snapshot_.state == State::Off) label = Tr(L"Выключена", L"Off");
        else if (snapshot_.state == State::InTransition) label = Tr(L"Переключение…", L"Changing state…");
        else label = Tr(L"Нет подключения", L"No connection");
        SetText(Control(Status), label);
        SetText(Control(Notice), snapshot_.notice.empty() ?
            Tr(L"Выберите адаптер и источник Интернета, затем включите точку доступа. Сохранённые настройки Windows не изменятся.", L"Choose an adapter and Internet source, then start the hotspot. Saved Windows settings will not change.") : snapshot_.notice);
        SetText(Control(ClientLabel), Tr(L"Подключённые устройства: ", L"Connected devices: ") + std::to_wstring(snapshot_.clientCount) +
            (snapshot_.maxClients ? L" / " + std::to_wstring(snapshot_.maxClients) : L""));
        SendMessageW(Control(Clients), LB_RESETCONTENT, 0, 0);
        if (snapshot_.clients.empty()) {
            SendMessageW(Control(Clients), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr(L"Устройств пока нет", L"No devices yet")));
        } else {
            for (const auto& client : snapshot_.clients) {
                const auto text = client.mac + (client.name.empty() ? L"" : L"  " + client.name);
                SendMessageW(Control(Clients), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            }
        }
        const bool locked = (busy_ && activeJob_ != JobKind::Poll) || snapshot_.owned ||
            snapshot_.state == State::On || snapshot_.state == State::InTransition;
        for (const auto id : {Adapter, Source, Ssid, Password, Band, Security, Priority, Generate, Refresh}) {
            EnableWindow(Control(id), !locked);
        }
        EnableWindow(Control(ShowPassword), !busy_ || activeJob_ == JobKind::Poll);
        bool validAdapter = false;
        for (const auto& a : snapshot_.adapters) if (a.id == snapshot_.adapterId) validAdapter = a.available;
        bool validSource = false;
        for (const auto& s : snapshot_.sources) if (s.id == snapshot_.sourceId) validSource = true;
        SetText(Control(WindowsSettings), NeedsWifiPermission() ? Tr(L"Разрешения расположения", L"Location permissions") : Tr(L"Параметры хот-спота Windows", L"Windows hotspot settings"));
        const bool ready = snapshot_.owned || (validAdapter && validSource && snapshot_.state == State::Off);
        EnableWindow(Control(StartStop), (!busy_ || activeJob_ == JobKind::Poll) && ready && snapshot_.state != State::InTransition);
        SetText(Control(StartStop), snapshot_.owned ? Tr(L"Выключить точку доступа", L"Stop hotspot") : Tr(L"Включить точку доступа", L"Start hotspot"));
        if (busy_ && activeJob_ == JobKind::Start) SetText(Control(StartStop), Tr(L"Включаем…", L"Starting…"));
        if (busy_ && activeJob_ == JobKind::Stop) SetText(Control(StartStop), Tr(L"Выключаем…", L"Stopping…"));
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool NeedsWifiPermission() const {
        return !snapshot_.adapters.empty() && std::none_of(snapshot_.adapters.begin(), snapshot_.adapters.end(),
            [](const auto& adapter) { return adapter.available; });
    }
    void Queue(Job job) {
        if (busy_ || !worker_) return;
        busy_ = true;
        activeJob_ = job.kind;
        if (job.kind != JobKind::Poll) {
            UpdateState();
            AppendLog(job.kind == JobKind::Discover ? Tr(L"Обновляем адаптеры и подключения…", L"Refreshing adapters and connections…") :
                      job.kind == JobKind::Start ? Tr(L"Windows запускает точку доступа…", L"Windows is starting the hotspot…") : Tr(L"Windows выключает точку доступа…", L"Windows is stopping the hotspot…"));
        }
        worker_->Submit(std::move(job));
    }
    int Dialog(const wchar_t* heading, const wchar_t* message, PCWSTR icon, bool confirmation = false) {
        // Explicit captions keep our dialogs in English even on non-Russian,
        // non-English Windows installations (rather than using OS-localized Yes/No).
        const TASKDIALOG_BUTTON ok[]{ {IDOK, Tr(L"ОК", L"OK")} };
        const TASKDIALOG_BUTTON yesNo[]{ {IDYES, Tr(L"Да", L"Yes")}, {IDNO, Tr(L"Нет", L"No")} };
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hwndParent = window_;
        config.hInstance = instance_;
        config.dwFlags = TDF_SIZE_TO_CONTENT | TDF_ALLOW_DIALOG_CANCELLATION;
        config.pszWindowTitle = L"WiFi Studio";
        config.pszMainInstruction = heading;
        config.pszContent = message;
        config.pszMainIcon = icon;
        config.cButtons = confirmation ? 2 : 1;
        config.pButtons = confirmation ? yesNo : ok;
        config.nDefaultButton = confirmation ? IDNO : IDOK;
        int choice = IDCANCEL;
        winrt::check_hresult(TaskDialogIndirect(&config, &choice, nullptr, nullptr));
        return choice;
    }
    void ErrorDialog(const std::wstring& message) {
        Dialog(Tr(L"Операция не выполнена", L"Operation failed"), message.c_str(), TD_ERROR_ICON);
    }
    void StartOrStop() {
        if (busy_) { if (activeJob_ == JobKind::Poll) startAfterPoll_ = true; return; }
        Job job;
        if (snapshot_.owned) job.kind = JobKind::Stop;
        else {
            job.kind = JobKind::Start;
            job.adapter = SelectedAdapter();
            job.source = SelectedSource();
            job.settings.ssid = Text(Control(Ssid));
            job.settings.passphrase = Text(Control(Password));
            job.settings.band = static_cast<hotspot::Band>(SendMessageW(Control(Band), CB_GETCURSEL, 0, 0));
            job.settings.security = static_cast<hotspot::Security>(SendMessageW(Control(Security), CB_GETCURSEL, 0, 0));
            job.settings.prioritizeHotspot = SendMessageW(Control(Priority), BM_GETCHECK, 0, 0) == BST_CHECKED;
            const auto errors = hotspot::Validate(job.settings);
            if (!errors.empty()) {
                std::wstring message;
                for (const auto& error : errors) { if (!message.empty()) message += L"\n\n"; message += error; }
                ErrorDialog(message);
                return;
            }
            if (job.adapter.empty() || job.source.empty()) { ErrorDialog(Tr(L"Выберите доступные Wi-Fi-адаптер и источник Интернета.", L"Select an available Wi-Fi adapter and Internet source.")); return; }
            if (job.settings.prioritizeHotspot) {
                const auto choice = Dialog(Tr(L"Изменить приоритет радиосвязи?", L"Change wireless priority?"),
                    Tr(L"Драйвер может перевести текущее Wi-Fi-подключение на другой диапазон. Это может временно повлиять на доступ в Интернет.", L"The driver may move the current Wi-Fi connection to another band. This may temporarily affect Internet access."),
                    TD_WARNING_ICON, true);
                if (choice != IDYES) return;
            }
        }
        Queue(std::move(job));
    }
    void HandleResults() {
        while (auto result = worker_->TakeResult()) {
            busy_ = false;
            const bool wasOwned = snapshot_.owned;
            snapshot_ = std::move(result->snapshot);
            if (result->kind == JobKind::Discover) Populate();
            if (!result->error.empty()) {
                if (result->kind != JobKind::Poll || result->error != lastPollError_) AppendLog(result->error);
                if (result->kind == JobKind::Poll) lastPollError_ = result->error;
                else {
                    closeWhenStopped_ = false;
                    ErrorDialog(result->error);
                }
            } else {
                lastPollError_.clear();
                if (result->kind == JobKind::Discover) {
                    AppendLog(Tr(L"Найдено Wi-Fi-адаптеров: ", L"Wi-Fi adapters found: ") + std::to_wstring(snapshot_.adapters.size()) + Tr(L". Источников: ", L". Sources: ") + std::to_wstring(snapshot_.sources.size()) + L".");
                    if (!snapshot_.notice.empty()) AppendLog(snapshot_.notice);
                }
                if (result->kind == JobKind::Start) AppendLog(Tr(L"Точка доступа включена на выбранном адаптере. Настройки действуют только в этой сессии.", L"Hotspot started on the selected adapter. Settings apply to this session only."));
                if (result->kind == JobKind::Stop) AppendLog(Tr(L"Точка доступа выключена.", L"Hotspot is off."));
                if (result->kind == JobKind::Poll && wasOwned && !snapshot_.owned) AppendLog(Tr(L"Windows остановила точку доступа (например, тайм-аут без клиентов, отключение адаптера или внешнее управление).", L"Windows stopped the hotspot (for example, no-client timeout, adapter removal, or external control)."));
            }
            UpdateState();
            if (closeWhenStopped_ && result->error.empty() && !snapshot_.owned && snapshot_.state == hotspot::State::Off) {
                DestroyWindow(window_);
                return;
            }
        }
    }
    void RequestClose() {
        if (busy_) {
            // Closing during discovery/start/stop is queued, not a modal dialog
            // that blocks interaction and can leave automation or the user stuck.
            closeAfterOperation_ = true;
            if (activeJob_ != JobKind::Poll) {
                AppendLog(Tr(L"Выход запрошен. Ожидаем завершения текущей операции Windows…", L"Exit requested. Waiting for the current Windows operation to finish…"));
                SetText(Control(Status), Tr(L"Завершаем операцию…", L"Finishing operation…"));
            }
            return;
        }
        if (snapshot_.owned) {
            const auto choice = Dialog(Tr(L"Выключить точку доступа и выйти?", L"Stop the hotspot and exit?"),
                Tr(L"Подключённые устройства потеряют соединение с этой точкой доступа.", L"Connected devices will lose their connection to this hotspot."), TD_WARNING_ICON, true);
            if (choice != IDYES) return;
            closeWhenStopped_ = true;
            Job job; job.kind = JobKind::Stop; Queue(std::move(job));
        } else DestroyWindow(window_);
    }
    void DrawButton(const DRAWITEMSTRUCT& draw) {
        const auto dc = draw.hDC;
        const auto oldFont = SelectObject(dc, body_);
        const bool enabled = !(draw.itemState & ODS_DISABLED);
        const bool primary = draw.CtlID == StartStop;
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const COLORREF fill = !enabled ? RGB(231, 236, 240) : primary ?
            (pressed ? RGB(6, 90, 103) : Accent) : (pressed ? RGB(224, 233, 239) : RGB(245, 248, 251));
        const auto brush = CreateSolidBrush(fill);
        const auto pen = CreatePen(PS_SOLID, 1, primary ? fill : Line);
        const auto oldBrush = SelectObject(dc, brush);
        const auto oldPen = SelectObject(dc, pen);
        RoundRect(dc, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right, draw.rcItem.bottom, Px(10), Px(10));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, !enabled ? RGB(127, 142, 153) : primary ? Paper : Ink);
        auto rect = draw.rcItem;
        InflateRect(&rect, -Px(6), 0);
        const auto text = Text(draw.hwndItem);
        DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (draw.itemState & ODS_FOCUS) { InflateRect(&rect, -Px(3), -Px(5)); DrawFocusRect(dc, &rect); }
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); SelectObject(dc, oldFont);
        DeleteObject(brush); DeleteObject(pen);
    }
    void Paint() {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window_, &paint);
        RECT client{}; GetClientRect(window_, &client);
        FillRect(dc, &client, paperBrush_);
        SetViewportOrgEx(dc, 0, -scrollOffset_, nullptr);
        const int right = Dip(client.right) - 314;
        const auto oldBrush = SelectObject(dc, sideBrush_);
        const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, Px(right - 18), Px(105), client.right - Px(24), Px(643), Px(18), Px(18));
        const auto accentBrush = CreateSolidBrush(Accent);
        SelectObject(dc, accentBrush);
        RoundRect(dc, Px(28), Px(22), Px(73), Px(70), Px(14), Px(14));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(accentBrush);
        const auto oldFont = SelectObject(dc, glyph_);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, Paper);
        RECT logo{Px(30), Px(27), Px(71), Px(67)};
        DrawTextW(dc, L"\xE701", 1, &logo, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        const auto linePen = CreatePen(PS_SOLID, 1, Line);
        const auto previous = SelectObject(dc, linePen);
        MoveToEx(dc, Px(28), Px(92), nullptr); LineTo(dc, client.right - Px(28), Px(92));
        SelectObject(dc, previous); DeleteObject(linePen);
        EndPaint(window_, &paint);
    }
    LRESULT Message(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: Initialize(); return 0;
        case WM_SIZE: Layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {Px(920), Px(620)};
            return 0;
        }
        case WM_DPICHANGED: {
            scrollOffset_ = MulDiv(scrollOffset_, HIWORD(wParam), static_cast<int>(dpi_));
            dpi_ = HIWORD(wParam);
            const auto* rect = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ConfigureFonts(); Layout(); return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO scroll{sizeof(scroll), SIF_ALL};
            GetScrollInfo(window_, SB_VERT, &scroll);
            switch (LOWORD(wParam)) {
            case SB_LINEUP: ScrollTo(scrollOffset_ - Px(28)); break;
            case SB_LINEDOWN: ScrollTo(scrollOffset_ + Px(28)); break;
            case SB_PAGEUP: ScrollTo(scrollOffset_ - static_cast<int>(scroll.nPage)); break;
            case SB_PAGEDOWN: ScrollTo(scrollOffset_ + static_cast<int>(scroll.nPage)); break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: ScrollTo(scroll.nTrackPos); break;
            case SB_TOP: ScrollTo(0); break;
            case SB_BOTTOM: ScrollTo(scroll.nMax); break;
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            ScrollTo(scrollOffset_ - GET_WHEEL_DELTA_WPARAM(wParam) * Px(48) / WHEEL_DELTA);
            return 0;
        case WM_PAINT: Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
        case WM_CTLCOLORSTATIC: {
            const auto dc = reinterpret_cast<HDC>(wParam);
            const auto control = reinterpret_cast<HWND>(lParam);
            const auto id = GetDlgCtrlID(control);
            RECT rect{}; GetWindowRect(control, &rect);
            MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&rect), 2);
            RECT client{}; GetClientRect(window_, &client);
            const bool side = rect.left >= client.right - Px(332);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, id == Status ? Accent : control == title_ || control == connectionsHeading_ || control == settingsHeading_ ? Ink : Muted);
            return reinterpret_cast<LRESULT>(side ? sideBrush_ : paperBrush_);
        }
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wParam), Ink);
            SetBkColor(reinterpret_cast<HDC>(wParam), Paper);
            return reinterpret_cast<LRESULT>(paperBrush_);
        case WM_TIMER:
            if (wParam == PollTimer && !busy_) { Job job; job.kind = JobKind::Poll; Queue(std::move(job)); }
            return 0;
        case ResultMessage:
            HandleResults();
            if (!busy_ && IsWindow(window_)) {
                if (closeAfterOperation_) { closeAfterOperation_ = false; RequestClose(); }
                else if (refreshAfterPoll_) { refreshAfterPoll_ = false; RefreshAdapters(); }
                else if (startAfterPoll_) { startAfterPoll_ = false; StartOrStop(); }
            }
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam), notification = HIWORD(wParam);
            if ((id == Adapter || id == Source) && notification == CBN_SELCHANGE) RefreshAdapters();
            else if (id == Refresh && notification == BN_CLICKED) RefreshAdapters();
            else if ((id == StartStop || id == IDOK) && notification == BN_CLICKED) StartOrStop();
            else if (id == ShowPassword && notification == BN_CLICKED) {
                showPassword_ = !showPassword_;
                SendMessageW(Control(Password), EM_SETPASSWORDCHAR, showPassword_ ? 0 : L'\x25CF', 0);
                SetText(Control(ShowPassword), showPassword_ ? Tr(L"Скрыть", L"Hide") : Tr(L"Показать", L"Show"));
                InvalidateRect(Control(Password), nullptr, TRUE);
            } else if (id == Generate && notification == BN_CLICKED) SetText(Control(Password), hotspot::GeneratePassword());
            else if (id == WindowsSettings && notification == BN_CLICKED) {
                const auto uri = NeedsWifiPermission() ? L"ms-settings:privacy-location" : L"ms-settings:network-mobilehotspot";
                const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32) ErrorDialog(Tr(L"Не удалось открыть параметры Windows.", L"Could not open Windows Settings."));
            } else if (id == IDCANCEL) RequestClose();
            return 0;
        }
        case WM_CLOSE: RequestClose(); return 0;
        case WM_QUERYENDSESSION:
            // Graceful OS shutdown will run the same worker-owned cleanup in WM_DESTROY.
            return !busy_;
        case WM_ENDSESSION:
            if (wParam) DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, PollTimer);
            worker_.reset();
            SetText(Control(Password), L"");
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        try { return self->Message(message, wParam, lParam); }
        catch (...) {
            const auto error = hotspot::CurrentExceptionMessage();
            MessageBoxW(window, error.c_str(), L"WiFi Studio", MB_OK | MB_ICONERROR);
            if (message == WM_CREATE) return -1;
            return 0;
        }
    }
    HINSTANCE instance_;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    HBRUSH paperBrush_ = CreateSolidBrush(Paper);
    HBRUSH sideBrush_ = CreateSolidBrush(Side);
    std::vector<HFONT> fonts_;
    HFONT body_ = nullptr, small_ = nullptr, heading_ = nullptr, titleFont_ = nullptr, glyph_ = nullptr;
    std::vector<HWND> controls_;
    HWND title_ = nullptr, subtitle_ = nullptr, connectionsHeading_ = nullptr, adapterLabel_ = nullptr,
        adapterDetail_ = nullptr, sourceLabel_ = nullptr, settingsHeading_ = nullptr,
        ssidLabel_ = nullptr, passwordLabel_ = nullptr, bandLabel_ = nullptr, securityLabel_ = nullptr,
        hint_ = nullptr, statusCaption_ = nullptr, sessionHint_ = nullptr, logLabel_ = nullptr;
    std::unique_ptr<BackendWorker> worker_;
    hotspot::Snapshot snapshot_;
    bool busy_ = false, showPassword_ = false, closeWhenStopped_ = false, closeAfterOperation_ = false;
    bool refreshAfterPoll_ = false, startAfterPoll_ = false, layingOut_ = false;
    int scrollOffset_ = 0;
    JobKind activeJob_ = JobKind::Discover;
    std::wstring lastPollError_;
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const auto mutex = CreateMutexW(nullptr, FALSE, L"Local\\WiFiStudio.Desktop.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const auto existing = FindWindowW(WindowClass, nullptr)) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        CloseHandle(mutex);
        return 0;
    }
    int exitCode = 0;
    try {
        int argumentCount = 0;
        const auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments) winrt::throw_last_error();
        const std::unique_ptr<wchar_t*, decltype(&LocalFree)> argumentBuffer(arguments, &LocalFree);
        bool noActivate = false;
        for (int i = 1; i < argumentCount; ++i) {
            const std::wstring_view argument(arguments[i]);
            if (argument == L"--no-activate") noActivate = true;
            else if (argument == L"--lang" && i + 1 < argumentCount) {
                const auto language = hotspot::ParseLanguage(arguments[++i]);
                if (!language) throw winrt::hresult_error(E_INVALIDARG, Tr(L"Язык должен быть ru или en.", L"Language must be ru or en."));
                hotspot::SetLanguage(*language);
            } else {
                throw winrt::hresult_error(E_INVALIDARG, Tr(L"Неизвестный или неполный аргумент. Доступны --lang ru|en и --no-activate.",
                    L"Unknown or incomplete argument. Available options: --lang ru|en and --no-activate."));
            }
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        App app(instance);
        if (!app.Create(noActivate ? SW_SHOWNOACTIVATE : show)) {
            throw winrt::hresult_error(E_FAIL, Tr(L"Не удалось создать окно приложения.", L"Could not create the application window."));
        }
        MSG message{};
        int status;
        while ((status = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_F5) { app.RefreshAdapters(); continue; }
            if (!IsDialogMessageW(app.Window(), &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        exitCode = status == -1 ? 1 : static_cast<int>(message.wParam);
    } catch (...) {
        const auto error = hotspot::CurrentExceptionMessage();
        MessageBoxW(nullptr, error.c_str(), L"WiFi Studio", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }
    CloseHandle(mutex);
    return exitCode;
}
