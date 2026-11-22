#include "hotspot/localization.hpp"

#include <windows.h>
#include <atomic>

namespace hotspot {
namespace {
// A single process-wide selection is shared by the GUI and its MTA worker.
// Atomic access also makes the explicit override safe for diagnostic/test callers.
std::atomic<Language>& SelectedLanguage() noexcept {
    static std::atomic<Language> language{DetectLanguage()};
    return language;
}
}

Language LanguageFromWindowsId(std::uint16_t languageId) noexcept {
    return PRIMARYLANGID(languageId) == LANG_RUSSIAN ? Language::Russian : Language::English;
}
Language DetectLanguage() noexcept { return LanguageFromWindowsId(GetUserDefaultUILanguage()); }
Language CurrentLanguage() noexcept { return SelectedLanguage().load(std::memory_order_relaxed); }
void SetLanguage(Language language) noexcept { SelectedLanguage().store(language, std::memory_order_relaxed); }
std::optional<Language> ParseLanguage(std::wstring_view value) noexcept {
    if (value == L"ru") return Language::Russian;
    if (value == L"en") return Language::English;
    return std::nullopt;
}
const wchar_t* Tr(const wchar_t* russian, const wchar_t* english) noexcept {
    return CurrentLanguage() == Language::Russian ? russian : english;
}

} // namespace hotspot
