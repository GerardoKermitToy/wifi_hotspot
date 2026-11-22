#pragma once

#include <cstdint>
#include <string_view>
#include <optional>

namespace hotspot {

enum class Language { English, Russian };

// Match the Windows display language, not the regional/formatting locale.
Language LanguageFromWindowsId(std::uint16_t languageId) noexcept;
Language DetectLanguage() noexcept;
Language CurrentLanguage() noexcept;
void SetLanguage(Language language) noexcept;
std::optional<Language> ParseLanguage(std::wstring_view value) noexcept;

// Each application-owned string carries both translations; no runtime resource files.
const wchar_t* Tr(const wchar_t* russian, const wchar_t* english) noexcept;

} // namespace hotspot
