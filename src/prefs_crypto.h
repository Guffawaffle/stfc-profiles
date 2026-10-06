#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace stfc::profiles::detail {
constexpr std::size_t MaxPlainPrefsBytes = 16 * 1024 * 1024;
constexpr std::size_t MaxEncryptedPrefsBytes = MaxPlainPrefsBytes + 8192;
std::vector<std::uint8_t> ReadProtectedPrefs(const std::filesystem::path& file, std::string_view id);
void WriteProtectedPrefs(const std::filesystem::path& file, std::string_view id,
                         std::span<const std::uint8_t> plaintext, bool existing);
void InitializePrefsMarker(const std::filesystem::path& marker);
void RecoverPrefsBackup(const std::filesystem::path& backup, const std::filesystem::path& target);
void ErasePrefsFile(const std::filesystem::path& file);
void EraseProtectedPrefsIdentity(std::string_view id);
void WipePrefsBytes(std::span<std::uint8_t> bytes) noexcept;
} // namespace stfc::profiles::detail
