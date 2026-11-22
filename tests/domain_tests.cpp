#include "hotspot/domain.hpp"
#include "hotspot/localization.hpp"
#include <thread>
#include <iostream>
#include <set>
#include <string>

namespace {
int failures = 0;
int checks = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
}
int main() {
    using namespace hotspot;
    Settings s;
    s.passphrase = L"Secure123!";
    Check(Validate(s).empty(), "default settings valid");
    s.ssid.clear(); Check(!Validate(s).empty(), "empty SSID rejected");
    s.ssid = L" "; Check(!Validate(s).empty(), "blank SSID rejected");
    s.ssid = L"A"; Check(Validate(s).empty(), "one byte SSID accepted");
    s.ssid.assign(32, L'A'); Check(Validate(s).empty(), "32 byte SSID accepted");
    s.ssid.push_back(L'A'); Check(!Validate(s).empty(), "33 byte SSID rejected");
    s.ssid = L"Сеть"; Check(!Validate(s).empty(), "locale-dependent SSID rejected");
    s.ssid = L"A\nB"; Check(!Validate(s).empty(), "control character SSID rejected");
    s.ssid = std::wstring(L"A\0B", 3); Check(!Validate(s).empty(), "embedded NUL SSID rejected");
    s.ssid = L"My Wi-Fi !"; Check(Validate(s).empty(), "printable ASCII name accepted");
    s.passphrase.assign(7, L'a'); Check(!Validate(s).empty(), "short password rejected");
    s.passphrase.assign(8, L'a'); Check(Validate(s).empty(), "8 character password accepted");
    s.passphrase.assign(63, L'a'); Check(Validate(s).empty(), "63 character password accepted");
    s.passphrase.push_back(L'a'); Check(!Validate(s).empty(), "64 character password rejected");
    s.passphrase = L"Пароль123"; Check(!Validate(s).empty(), "non-ASCII password rejected");
    s.passphrase = L"hello\tworld"; Check(!Validate(s).empty(), "password control character rejected");
    s.passphrase = L"Secure123!";
    for (const auto b : {Band::Automatic, Band::Ghz24, Band::Ghz5, Band::Ghz6}) {
        for (const auto a : {Security::Wpa2, Security::Wpa3Transition, Security::Wpa3}) {
            s.band = b; s.security = a;
            const bool expected = b != Band::Ghz6 || a == Security::Wpa3;
            Check(Validate(s).empty() == expected, "band/authentication matrix");
        }
    }
    s.band = static_cast<Band>(999); Check(!Validate(s).empty(), "invalid band rejected");
    s.band = Band::Automatic; s.security = static_cast<Security>(999); Check(!Validate(s).empty(), "invalid authentication rejected");
    s.security = Security::Wpa2;
    std::set<std::wstring> generated;
    for (int i = 0; i < 128; ++i) {
        s.passphrase = GeneratePassword();
        Check(s.passphrase.size() == 16 && Validate(s).empty(), "generated password has valid length/alphabet");
        generated.insert(s.passphrase);
    }
    Check(generated.size() == 128, "random generation smoke test (no duplicate among 128)");
    Check(!StateLabel(State::Unknown).empty() && !BandLabel(Band::Ghz6).empty() && !SecurityLabel(Security::Wpa3).empty(), "UI labels exist");
    Check(CurrentLanguage() == DetectLanguage(), "default language follows Windows display language");
    Check(LanguageFromWindowsId(0x0419) == Language::Russian, "Russian display language selects Russian");
    Check(LanguageFromWindowsId(0x0819) == Language::Russian, "Russian regional variant selects Russian");
    for (const auto id : {0x0409, 0x0809, 0x0407, 0x040c, 0x0411, 0x0422, 0x0000}) {
        Check(LanguageFromWindowsId(static_cast<std::uint16_t>(id)) == Language::English, "all non-Russian display languages select English");
    }
    Check(ParseLanguage(L"en") == Language::English && ParseLanguage(L"ru") == Language::Russian, "explicit language overrides recognized");
    Check(!ParseLanguage(L"de") && !ParseLanguage(L"") && !ParseLanguage(L"english"), "invalid overrides rejected");
    const auto originalLanguage = CurrentLanguage();
    for (const auto language : {Language::English, Language::Russian}) {
        SetLanguage(language);
        const bool english = language == Language::English;
        Check(CurrentLanguage() == language, "process-wide language selection");
        Check(std::wstring_view(Tr(L"Пример", L"Example")) == (english ? L"Example" : L"Пример"), "paired translation selects requested language");
        Check(StateLabel(State::Off) == (english ? L"Hotspot is off" : L"Точка доступа выключена"), "localized Off state");
        Check(StateLabel(State::On) == (english ? L"Hotspot is running" : L"Точка доступа работает"), "localized On state");
        Check(BandLabel(Band::Ghz24) == (english ? L"2.4 GHz" : L"2,4 ГГц"), "localized frequency units");
        Check(BandLabel(Band::Automatic) == (english ? L"Automatic" : L"Автоматически"), "localized automatic band");
        Settings invalid;
        invalid.ssid.clear();
        invalid.passphrase.clear();
        const auto errors = Validate(invalid);
        Check(errors.size() == 2, "validation unchanged across languages");
        Check(!errors.empty() && errors[0].starts_with(english ? L"Network name:" : L"Имя сети:"), "localized SSID validation");
        Check(errors.size() > 1 && errors[1].starts_with(english ? L"Password:" : L"Пароль:"), "localized password validation");
        bool workerMatches = false;
        std::thread worker([&] { workerMatches = CurrentLanguage() == language && std::wstring_view(Tr(L"Пример", L"Example")) == (english ? L"Example" : L"Пример"); });
        worker.join();
        Check(workerMatches, "background thread uses the same language");
    }
    SetLanguage(originalLanguage);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
