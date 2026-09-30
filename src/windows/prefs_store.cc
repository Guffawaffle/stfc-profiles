// Windows protection and durable replacement; see LICENSE (GPL-3.0).
#if _WIN32
#include "../prefs_crypto.h"
#include <Windows.h>
#include <wincrypt.h>
#include <atomic>
#include <chrono>
#include <stdexcept>

namespace stfc::profiles::detail {
namespace {
[[noreturn]] void InvalidStore() { throw std::runtime_error("invalid protected preference store"); }
struct LocalBlob
{
  DATA_BLOB data{};

  ~LocalBlob()
  {
    if (data.pbData) {
      SecureZeroMemory(data.pbData, data.cbData);
      LocalFree(data.pbData);
    }
  }

  LocalBlob(const LocalBlob&) = delete;
  LocalBlob& operator=(const LocalBlob&) = delete;
  LocalBlob() = default;
};

struct WipeBytes
{
  std::vector<std::uint8_t>& bytes;

  ~WipeBytes()
  {
    if (!bytes.empty())
      SecureZeroMemory(bytes.data(), bytes.size());
  }
};

std::vector<std::uint8_t> ReadEncryptedFile(const std::filesystem::path& path)
{
  const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    InvalidStore();

  std::vector<std::uint8_t> bytes;
  bool ok = false;
  try {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > static_cast<LONGLONG>(MaxEncryptedPrefsBytes))
      InvalidStore();
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      DWORD count = 0;
      if (!ReadFile(file, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr)
          || count == 0)
        InvalidStore();
      offset += count;
    }
    ok = CloseHandle(file) != 0;
  } catch (...) {
    CloseHandle(file);
    throw;
  }
  if (!ok)
    InvalidStore();
  return bytes;
}

void WriteEncryptedFile(const std::filesystem::path& temporary, std::span<const std::uint8_t> bytes)
{
  auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw std::runtime_error("could not stage isolated preferences");
  try {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      DWORD count = 0;
      if (!WriteFile(file, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr)
          || count == 0)
        throw std::runtime_error("could not write isolated preferences");
      offset += count;
    }
    if (!FlushFileBuffers(file))
      throw std::runtime_error("could not flush isolated preferences");
  } catch (...) {
    CloseHandle(file);
    DeleteFileW(temporary.c_str());
    throw;
  }
  if (!CloseHandle(file)) {
    DeleteFileW(temporary.c_str());
    throw std::runtime_error("could not close isolated preferences");
  }
}


std::vector<std::uint8_t> Entropy(std::string_view id)
{
  constexpr std::string_view domain = "stfc-profiles-prefs-v1:";
  std::vector<std::uint8_t> result(domain.begin(), domain.end());
  result.insert(result.end(), id.begin(), id.end());
  return result;
}
} // namespace

std::vector<std::uint8_t> ReadProtectedPrefs(const std::filesystem::path& file, std::string_view id)
{
  const auto encrypted = ReadEncryptedFile(file);
  auto entropy = Entropy(id);
  DATA_BLOB source{static_cast<DWORD>(encrypted.size()), const_cast<std::uint8_t*>(encrypted.data())};
  DATA_BLOB salt{static_cast<DWORD>(entropy.size()), entropy.data()};
  LocalBlob decrypted;
  if (!CryptUnprotectData(&source, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &decrypted.data)
      || decrypted.data.cbData > MaxPlainPrefsBytes)
    InvalidStore();
  return {decrypted.data.pbData, decrypted.data.pbData + decrypted.data.cbData};
}

void WriteProtectedPrefs(const std::filesystem::path& file, std::string_view id,
                         std::span<const std::uint8_t> plaintext, bool existing)
{
  if (plaintext.size() > MaxPlainPrefsBytes) InvalidStore();
  auto entropy = Entropy(id);
  DATA_BLOB salt{static_cast<DWORD>(entropy.size()), entropy.data()};
  DATA_BLOB source{static_cast<DWORD>(plaintext.size()), const_cast<std::uint8_t*>(plaintext.data())};
  LocalBlob encrypted;
  if (!CryptProtectData(&source, L"STFC Profiles preferences", &salt, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &encrypted.data))
    throw std::runtime_error("could not protect isolated preferences");
  if (encrypted.data.cbData == 0 || encrypted.data.cbData > MaxEncryptedPrefsBytes) InvalidStore();
  static std::atomic_uint64_t sequence{0};
  auto temporary = file;
  temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"."
               + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"."
               + std::to_wstring(++sequence);
  WriteEncryptedFile(temporary, {encrypted.data.pbData, encrypted.data.cbData});
  if (existing) {
    auto backup = file; backup += L".bak";
    if (!ReplaceFileW(file.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr, nullptr))
      throw std::runtime_error("could not replace isolated preferences");
    ErasePrefsFile(backup);
  } else if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH)) {
    throw std::runtime_error("could not create isolated preferences");
  }
}

void InitializePrefsMarker(const std::filesystem::path& marker)
{
  HANDLE handle = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) return;
    throw std::runtime_error("could not mark initialized preferences");
  }
  constexpr char data[] = "v1\n";
  DWORD count = 0;
  const bool ok = WriteFile(handle, data, sizeof(data)-1, &count, nullptr)
                  && count == sizeof(data)-1 && FlushFileBuffers(handle);
  const bool closed = CloseHandle(handle) != 0;
  if (!ok || !closed) throw std::runtime_error("could not finish initialized preference marker");
}

void RecoverPrefsBackup(const std::filesystem::path& backup, const std::filesystem::path& target)
{
  if (!MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("could not restore isolated preferences backup");
}
void ErasePrefsFile(const std::filesystem::path& file)
{
  if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
    throw std::runtime_error("could not remove preference recovery data");
}
void EraseProtectedPrefsIdentity(std::string_view) {} // DPAPI owns no per-profile keychain item.
void WipePrefsBytes(std::span<std::uint8_t> bytes) noexcept
{ if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
} // namespace stfc::profiles::detail
#endif
