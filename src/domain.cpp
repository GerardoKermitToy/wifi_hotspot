#include "hotspot/localization.hpp"
#include "hotspot/domain.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace hotspot {
namespace {
bool PrintableAscii(std::wstring_view text) {
    return std::all_of(text.begin(), text.end(), [](wchar_t c) {
        return c >= L' ' && c <= L'~';
    });
}
}

std::vector<std::wstring> Validate(const Settings& settings) {
    std::vector<std::wstring> errors;
    if (settings.ssid.empty() || settings.ssid.size() > 32 || !PrintableAscii(settings.ssid) ||
        settings.ssid.find_first_not_of(L' ') == std::wstring::npos) {
        errors.emplace_back(Tr(L"Имя сети: от 1 до 32 печатных ASCII-символов (латиница, цифры, знаки), не только пробелы.", L"Network name: 1–32 printable ASCII characters (letters, numbers, symbols), not just spaces."));
    }
    if (settings.passphrase.size() < 8 || settings.passphrase.size() > 63 || !PrintableAscii(settings.passphrase)) {
        errors.emplace_back(Tr(L"Пароль: от 8 до 63 печатных ASCII-символов (латиница, цифры, знаки).", L"Password: 8–63 printable ASCII characters (letters, numbers, symbols)."));
    }
    switch (settings.band) {
    case Band::Automatic: case Band::Ghz24: case Band::Ghz5: case Band::Ghz6: break;
    default: errors.emplace_back(Tr(L"Неизвестный диапазон Wi-Fi.", L"Unknown Wi-Fi band.")); break;
    }
    switch (settings.security) {
    case Security::Wpa2: case Security::Wpa3Transition: case Security::Wpa3: break;
    default: errors.emplace_back(Tr(L"Неизвестный тип защиты.", L"Unknown security mode.")); break;
    }
    if (settings.band == Band::Ghz6 && settings.security != Security::Wpa3) {
        errors.emplace_back(Tr(L"Для 6 ГГц выберите WPA3. WPA2 и переходный режим в этом диапазоне не используются.", L"Select WPA3 for 6 GHz. WPA2 and transition mode are not available on this band."));
    }
    return errors;
}

std::wstring GeneratePassword() {
    // 32 symbols; masking gives an unbiased distribution. 16 chars = 80 bits.
    constexpr std::wstring_view alphabet = L"ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static_assert(alphabet.size() == 32);
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
    std::wstring result;
    result.reserve(bytes.size());
    for (const auto byte : bytes) result.push_back(alphabet[byte & 31]);
    SecureZeroMemory(bytes.data(), bytes.size());
    return result;
}

std::wstring StateLabel(State state) {
    switch (state) {
    case State::Off: return Tr(L"Точка доступа выключена", L"Hotspot is off");
    case State::On: return Tr(L"Точка доступа работает", L"Hotspot is running");
    case State::InTransition: return Tr(L"Windows меняет состояние сети", L"Windows is changing the network state");
    default: return Tr(L"Состояние недоступно", L"Status unavailable");
    }
}
std::wstring BandLabel(Band band) {
    switch (band) {
    case Band::Ghz24: return Tr(L"2,4 ГГц", L"2.4 GHz");
    case Band::Ghz5: return Tr(L"5 ГГц", L"5 GHz");
    case Band::Ghz6: return Tr(L"6 ГГц", L"6 GHz");
    default: return Tr(L"Автоматически", L"Automatic");
    }
}
std::wstring SecurityLabel(Security security) {
    switch (security) {
    case Security::Wpa3Transition: return L"WPA2 / WPA3";
    case Security::Wpa3: return L"WPA3";
    default: return L"WPA2";
    }
}
} // namespace hotspot
