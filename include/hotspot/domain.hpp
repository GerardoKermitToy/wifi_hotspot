#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hotspot {

enum class Band { Automatic, Ghz24, Ghz5, Ghz6 };
enum class Security { Wpa2, Wpa3Transition, Wpa3 };
enum class State { Unknown, Off, On, InTransition };

struct Settings {
    std::wstring ssid = L"WiFi-Studio";
    std::wstring passphrase;
    Band band = Band::Automatic;
    Security security = Security::Wpa2;
    bool prioritizeHotspot = false;
};

// Intentionally ASCII: this API encodes SSIDs using the system code page.
// Printable ASCII prevents different SSIDs on clients with different locales.
std::vector<std::wstring> Validate(const Settings& settings);
std::wstring GeneratePassword();
std::wstring StateLabel(State state);
std::wstring BandLabel(Band band);
std::wstring SecurityLabel(Security security);

} // namespace hotspot
