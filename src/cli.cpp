#include "hotspot/hotspot_service.hpp"
#include "hotspot/localization.hpp"
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
void Print(const std::wstring& text) { std::cout << winrt::to_string(text) << '\n'; }
void Dump(const hotspot::Snapshot& snapshot) {
    Print(L"WiFi Studio / Windows 11 session tethering");
    Print(L"--- Wi-Fi adapters ---");
    for (const auto& a : snapshot.adapters) {
        Print(a.id + L" | " + a.name + L" | " + a.description + L" | " + a.connectionState +
              (a.available ? L" | WinRT: yes" : L" | WinRT: NO"));
    }
    Print(L"--- Connection sources ---");
    for (const auto& s : snapshot.sources) {
        Print(s.id + L" | " + s.name + (s.internet ? L" | Internet" : L" | Local access") + (s.isDefault ? L" | Default" : L""));
    }
    Print(L"Selected adapter: " + snapshot.adapterId);
    Print(L"Selected source:  " + snapshot.sourceId);
    Print(L"State: " + hotspot::StateLabel(snapshot.state));
    Print(L"Session owned: " + std::wstring(snapshot.owned ? L"yes" : L"no"));
    Print(L"Clients: " + std::to_wstring(snapshot.clientCount) + L" / " + std::to_wstring(snapshot.maxClients));
    if (!snapshot.notice.empty()) Print(snapshot.notice);
}
std::wstring CanonicalGuid(const wchar_t* value) {
    return std::wstring(winrt::to_hstring(winrt::guid(value)));
}
}
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    bool cycle = false;
    std::wstring adapter;
    std::wstring source;
    hotspot::Settings settings;
    settings.band = hotspot::Band::Ghz24;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--diagnose") continue;
            if (arg == L"--lang" && i + 1 < argc) {
                const auto language = hotspot::ParseLanguage(argv[++i]);
                if (!language) throw std::invalid_argument("Language must be ru or en.");
                hotspot::SetLanguage(*language);
            }
            else if (arg == L"--test-cycle") cycle = true;
            else if (arg == L"--adapter" && i + 1 < argc) adapter = CanonicalGuid(argv[++i]);
            else if (arg == L"--source" && i + 1 < argc) source = CanonicalGuid(argv[++i]);
            else if (arg == L"--band" && i + 1 < argc) {
                const std::wstring value = argv[++i];
                if (value == L"auto") settings.band = hotspot::Band::Automatic;
                else if (value == L"2.4") settings.band = hotspot::Band::Ghz24;
                else if (value == L"5") settings.band = hotspot::Band::Ghz5;
                else if (value == L"6") settings.band = hotspot::Band::Ghz6;
                else throw std::invalid_argument("Band must be auto, 2.4, 5 or 6.");
            } else if (arg == L"--security" && i + 1 < argc) {
                const std::wstring value = argv[++i];
                if (value == L"wpa2") settings.security = hotspot::Security::Wpa2;
                else if (value == L"wpa3") settings.security = hotspot::Security::Wpa3;
                else if (value == L"transition") settings.security = hotspot::Security::Wpa3Transition;
                else throw std::invalid_argument("Security must be wpa2, wpa3 or transition.");
            } else if (arg == L"--help" || arg == L"-h") {
                Print(L"wifi-hotspot-cli [--diagnose] [--lang ru|en] [--adapter GUID] [--source GUID]\n"
                      L"wifi-hotspot-cli --test-cycle [--adapter GUID] [--source GUID] [--band auto|2.4|5|6] [--security wpa2|wpa3|transition]\n"
                      L"Diagnose does not start a hotspot. Test-cycle creates a protected temporary network for 5 seconds,\n"
                      L"then stops it and checks that the persistent Windows configuration is unchanged.\n"
                      L"An existing hotspot is never stopped. No password is printed or written to disk.");
                return 0;
            } else throw std::invalid_argument("Unknown/incomplete argument. Use --help.");
        }
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        hotspot::HotspotService service;
        auto snapshot = service.Discover(adapter, source);
        Dump(snapshot);
        if (!cycle) return snapshot.adapterId.empty() || snapshot.sourceId.empty() || snapshot.state == hotspot::State::Unknown ? 2 : 0;
        if (adapter.empty() && snapshot.adapters.size() != 1) throw std::runtime_error("Pass --adapter GUID explicitly when there is more than one Wi-Fi adapter.");
        if (snapshot.state != hotspot::State::Off) throw std::runtime_error("Hardware test refused: hotspot state must be Off. Nothing was changed.");
        settings.passphrase = hotspot::GeneratePassword();
        settings.ssid = L"WiFiStudio-Test-" + settings.passphrase.substr(0, 6);
        const auto errors = hotspot::Validate(settings);
        if (!errors.empty()) { Print(errors.front()); return 2; }
        const auto before = service.PersistentConfigurationSignature();
        Print(L"TEST: starting " + hotspot::BandLabel(settings.band) + L" / " + hotspot::SecurityLabel(settings.security));
        try {
            snapshot = service.Start(settings, snapshot.adapterId, snapshot.sourceId);
            if (snapshot.state != hotspot::State::On || !snapshot.owned) throw std::runtime_error("Start completed but Windows did not report On/owned.");
            Print(L"TEST: On (confirmed by Windows), waiting 5 seconds...");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            snapshot = service.Poll();
            if (snapshot.state != hotspot::State::On) throw std::runtime_error("Hotspot did not remain On during the test.");
            snapshot = service.Stop();
            if (snapshot.state != hotspot::State::Off) throw std::runtime_error("Windows did not report Off after Stop.");
            Print(L"TEST: Off (confirmed by Windows)");
            if (before != service.PersistentConfigurationSignature()) throw std::runtime_error("Persistent Windows configuration changed unexpectedly.");
            Print(L"PASS: Start -> On -> Stop -> Off; persistent configuration unchanged.");
            Print(L"NOTE: A second client device is required to verify association, DHCP and Internet traffic.");
            return 0;
        } catch (...) {
            const auto original = hotspot::CurrentExceptionMessage();
            Print(L"FAIL: " + original);
            if (service.OwnsSession()) {
                try { service.Stop(); Print(L"CLEANUP: hotspot stopped."); }
                catch (...) { Print(L"CLEANUP FAILED: " + hotspot::CurrentExceptionMessage()); }
            }
            return 1;
        }
    } catch (...) {
        Print(L"ERROR: " + hotspot::CurrentExceptionMessage());
        return 1;
    }
}
