// Extracted from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#if _WIN32

#include "stfc_profiles/windows/prefs_store.h"
#include "stfc_profiles/legacy_contract.h"

#include <wincrypt.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace stfc::profiles::windows {

namespace {

constexpr std::array<BYTE, 8> magic{'S', 'T', 'F', 'C', 'P', 'R', 'E', 'F'};
constexpr std::uint32_t schema_version = 1;
constexpr std::size_t max_plain_bytes = 16 * 1024 * 1024;
constexpr std::size_t max_encrypted_bytes = max_plain_bytes + 8192;

[[noreturn]] void InvalidStore()
{ throw std::runtime_error("invalid isolated preference store"); }

void AppendByte(std::vector<BYTE>& output, BYTE value)
{ output.push_back(value); }

void AppendU16(std::vector<BYTE>& output, std::uint16_t value)
{
  AppendByte(output, static_cast<BYTE>(value));
  AppendByte(output, static_cast<BYTE>(value >> 8));
}

void AppendU32(std::vector<BYTE>& output, std::uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8)
    AppendByte(output, static_cast<BYTE>(value >> shift));
}

void AppendString(std::vector<BYTE>& output, std::u16string_view value)
{
  if (value.size() > std::numeric_limits<std::uint32_t>::max())
    InvalidStore();
  AppendU32(output, static_cast<std::uint32_t>(value.size()));
  for (const auto ch : value)
    AppendU16(output, static_cast<std::uint16_t>(ch));
  if (output.size() > max_plain_bytes)
    InvalidStore();
}

class Reader
{
public:
  Reader(const BYTE* data, std::size_t size) : bytes_(data, size) {}

  BYTE Byte()
  {
    if (offset_ == bytes_.size())
      InvalidStore();
    return bytes_[offset_++];
  }

  std::uint16_t U16()
  {
    const auto first = static_cast<std::uint16_t>(Byte());
    return first | (static_cast<std::uint16_t>(Byte()) << 8);
  }

  std::uint32_t U32()
  {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8)
      value |= static_cast<std::uint32_t>(Byte()) << shift;
    return value;
  }

  std::u16string String()
  {
    const auto length = U32();
    if (length > Remaining() / 2)
      InvalidStore();
    std::u16string value;
    value.reserve(length);
    for (std::uint32_t i = 0; i < length; ++i)
      value.push_back(static_cast<char16_t>(U16()));
    return value;
  }

  std::size_t Remaining() const
  { return bytes_.size() - offset_; }

private:
  std::span<const BYTE> bytes_;
  std::size_t offset_ = 0;
};

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
  std::vector<BYTE>& bytes;

  ~WipeBytes()
  {
    if (!bytes.empty())
      SecureZeroMemory(bytes.data(), bytes.size());
  }
};

std::vector<BYTE> ReadEncryptedFile(const std::filesystem::path& path)
{
  const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    InvalidStore();

  std::vector<BYTE> bytes;
  bool ok = false;
  try {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > static_cast<LONGLONG>(max_encrypted_bytes))
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

void WriteEncryptedFile(const std::filesystem::path& temporary, std::span<const BYTE> bytes)
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

} // namespace

ProfilePrefsStore::ProfilePrefsStore(const std::filesystem::path& local_app_data, std::wstring_view profile_id,
                                     ProfileOpenMode mode)
{
  if (local_app_data.empty() || profile_id.empty())
    InvalidStore();
  std::string narrow_id;
  narrow_id.reserve(profile_id.size());
  for (const wchar_t ch : profile_id) {
    if (ch > 0x7f)
      InvalidStore();
    narrow_id.push_back(static_cast<char>(ch));
    profile_id_.push_back(static_cast<char16_t>(ch));
  }
  if (!stfc::profiles::legacy::ValidId(narrow_id))
    InvalidStore();

  const auto directory = local_app_data / L"STFC Community Mod" / L"Profiles" / std::wstring(profile_id);
  std::filesystem::create_directories(directory);
  file_path_ = directory / L"player_prefs.bin";
  initialized_path_ = directory / L"player_prefs.bin.initialized";
  auto lock_path = file_path_;
  lock_path += L".lock";
  lock_handle_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (lock_handle_ == INVALID_HANDLE_VALUE)
    throw std::runtime_error("isolated profile is locked or unavailable");

  try {
    file_exists_ = std::filesystem::exists(file_path_);
    initialized_ = std::filesystem::exists(initialized_path_);
    auto backup = file_path_;
    backup += L".bak";
    if (mode == ProfileOpenMode::New) {
      if (file_exists_ || initialized_ || std::filesystem::exists(backup))
        InvalidStore();
      for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().filename().wstring().starts_with(L"player_prefs.bin.tmp."))
          InvalidStore();
    } else {
      if (!file_exists_ && std::filesystem::exists(backup)) {
        // ReplaceFileW can move the committed file to .bak while leaving the
        // replacement staged. Validate before restoring it under the lock.
        values_ = LoadStore(backup);
        if (!MoveFileExW(backup.c_str(), file_path_.c_str(), MOVEFILE_WRITE_THROUGH))
          throw std::runtime_error("could not restore isolated preferences backup");
        file_exists_ = true;
      } else if (file_exists_) {
        values_ = LoadStore(file_path_);
      } else if (mode == ProfileOpenMode::Existing || initialized_) {
        InvalidStore();
      }

      // A valid committed file wins over a previous interrupted replacement.
      if (file_exists_ && std::filesystem::exists(backup) && !DeleteFileW(backup.c_str()))
        throw std::runtime_error("could not remove stale profile backup");
      for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().wstring().starts_with(L"player_prefs.bin.tmp.")
            && !DeleteFileW(entry.path().c_str()))
          throw std::runtime_error("could not remove stale profile staging file");
      }
    }
  } catch (...) {
    CloseHandle(lock_handle_);
    lock_handle_ = INVALID_HANDLE_VALUE;
    throw;
  }
}

ProfilePrefsStore::~ProfilePrefsStore()
{
  if (lock_handle_ != INVALID_HANDLE_VALUE)
    CloseHandle(lock_handle_);
}

std::vector<BYTE> ProfilePrefsStore::Entropy() const
{
  std::vector<BYTE> entropy{'s', 't', 'f', 'c', '-', 'm', 'o', 'd', '-', 'p', 'r', 'o', 'f', 'i', 'l', 'e', '-', 'v', '1'};
  for (const auto ch : profile_id_)
    AppendU16(entropy, static_cast<std::uint16_t>(ch));
  return entropy;
}

void ProfilePrefsStore::Serialize(const Values& values, std::vector<BYTE>& output) const
{
  if (values.size() > std::numeric_limits<std::uint32_t>::max())
    InvalidStore();
  output.insert(output.end(), magic.begin(), magic.end());
  AppendU32(output, schema_version);
  AppendString(output, profile_id_);
  AppendU32(output, static_cast<std::uint32_t>(values.size()));
  for (const auto& [key, value] : values) {
    AppendString(output, key);
    if (const auto* number = std::get_if<std::int32_t>(&value)) {
      AppendByte(output, 1);
      AppendU32(output, std::bit_cast<std::uint32_t>(*number));
    } else if (const auto* decimal = std::get_if<float>(&value)) {
      AppendByte(output, 2);
      AppendU32(output, std::bit_cast<std::uint32_t>(*decimal));
    } else {
      AppendByte(output, 3);
      AppendString(output, std::get<std::u16string>(value));
    }
    if (output.size() > max_plain_bytes)
      InvalidStore();
  }
}

ProfilePrefsStore::Values ProfilePrefsStore::Deserialize(const BYTE* data, std::size_t size) const
{
  Reader input(data, size);
  for (const auto expected : magic)
    if (input.Byte() != expected)
      InvalidStore();
  if (input.U32() != schema_version || input.String() != profile_id_)
    InvalidStore();
  const auto count = input.U32();
  if (count > max_plain_bytes / 7)
    InvalidStore();
  Values result;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto key = input.String();
    const auto kind = input.Byte();
    Value value;
    if (kind == 1)
      value = std::bit_cast<std::int32_t>(input.U32());
    else if (kind == 2)
      value = std::bit_cast<float>(input.U32());
    else if (kind == 3)
      value = input.String();
    else
      InvalidStore();
    if (!result.emplace(std::move(key), std::move(value)).second)
      InvalidStore();
  }
  if (input.Remaining() != 0)
    InvalidStore();
  return result;
}

ProfilePrefsStore::Values ProfilePrefsStore::LoadStore(const std::filesystem::path& path) const
{
  const auto encrypted = ReadEncryptedFile(path);
  const auto entropy = Entropy();
  DATA_BLOB source{static_cast<DWORD>(encrypted.size()), const_cast<BYTE*>(encrypted.data())};
  DATA_BLOB salt{static_cast<DWORD>(entropy.size()), const_cast<BYTE*>(entropy.data())};
  LocalBlob decrypted;
  if (!CryptUnprotectData(&source, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &decrypted.data)
      || decrypted.data.cbData > max_plain_bytes)
    InvalidStore();
  return Deserialize(decrypted.data.pbData, decrypted.data.cbData);
}

void ProfilePrefsStore::Persist(const Values& values)
{
  const auto entropy = Entropy();
  DATA_BLOB salt{static_cast<DWORD>(entropy.size()), const_cast<BYTE*>(entropy.data())};
  std::vector<BYTE> plain;
  WipeBytes wipe{plain};
  Serialize(values, plain);
  if (plain.size() > max_plain_bytes)
    InvalidStore();
  DATA_BLOB source{static_cast<DWORD>(plain.size()), plain.data()};
  LocalBlob encrypted;
  if (!CryptProtectData(&source, L"STFC isolated profile preferences", &salt, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &encrypted.data))
    throw std::runtime_error("could not protect isolated preferences");
  if (encrypted.data.cbData == 0 || encrypted.data.cbData > max_encrypted_bytes)
    throw std::runtime_error("isolated preferences exceed the storage limit");

  static std::atomic_uint64_t sequence{0};
  auto temporary = file_path_;
  temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"."
               + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"."
               + std::to_wstring(++sequence);
  WriteEncryptedFile(temporary, {encrypted.data.pbData, encrypted.data.cbData});

  if (file_exists_) {
    auto backup = file_path_;
    backup += L".bak";
    if (!ReplaceFileW(file_path_.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr, nullptr))
      throw std::runtime_error("could not replace isolated preferences");
    if (!DeleteFileW(backup.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
      throw std::runtime_error("could not remove previous isolated preferences");
  } else {
    if (!MoveFileExW(temporary.c_str(), file_path_.c_str(), MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("could not create isolated preferences");
  }
  file_exists_ = true;
  MarkInitialized();
}

void ProfilePrefsStore::MarkInitialized()
{
  if (initialized_)
    return;
  const HANDLE marker = CreateFileW(initialized_path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (marker == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
      initialized_ = true;
      return;
    }
    throw std::runtime_error("could not mark initialized profile preferences");
  }
  constexpr char contents[] = "v1\n";
  DWORD written = 0;
  const bool ok = WriteFile(marker, contents, sizeof(contents) - 1, &written, nullptr)
                  && written == sizeof(contents) - 1 && FlushFileBuffers(marker);
  const bool closed = CloseHandle(marker) != 0;
  if (!ok || !closed)
    throw std::runtime_error("could not finish marking initialized profile preferences");
  initialized_ = true;
}

void ProfilePrefsStore::Set(std::u16string_view key, Value value)
{
  std::scoped_lock lock(mutex_);
  bool unchanged = false;
  if (const auto found = values_.find(key); found != values_.end() && found->second.index() == value.index()) {
    if (std::holds_alternative<float>(value))
      unchanged = std::bit_cast<std::uint32_t>(std::get<float>(found->second))
                  == std::bit_cast<std::uint32_t>(std::get<float>(value));
    else
      unchanged = found->second == value;
  }
  if (unchanged) {
    if (!std::filesystem::is_regular_file(file_path_))
      throw std::runtime_error("isolated preferences are unavailable");
    return;
  }
  auto next = values_;
  next.insert_or_assign(std::u16string(key), std::move(value));
  Persist(next);
  values_.swap(next);
}

void ProfilePrefsStore::SetInt(std::u16string_view key, std::int32_t value)
{ Set(key, value); }

void ProfilePrefsStore::SetFloat(std::u16string_view key, float value)
{ Set(key, value); }

void ProfilePrefsStore::SetString(std::u16string_view key, std::u16string_view value)
{ Set(key, std::u16string(value)); }

std::int32_t ProfilePrefsStore::GetInt(std::u16string_view key, std::int32_t fallback) const
{
  std::scoped_lock lock(mutex_);
  const auto found = values_.find(key);
  if (found == values_.end())
    return fallback;
  const auto* value = std::get_if<std::int32_t>(&found->second);
  return value ? *value : fallback;
}

float ProfilePrefsStore::GetFloat(std::u16string_view key, float fallback) const
{
  std::scoped_lock lock(mutex_);
  const auto found = values_.find(key);
  if (found == values_.end())
    return fallback;
  const auto* value = std::get_if<float>(&found->second);
  return value ? *value : fallback;
}

std::optional<std::u16string> ProfilePrefsStore::GetString(std::u16string_view key) const
{
  std::scoped_lock lock(mutex_);
  const auto found = values_.find(key);
  if (found == values_.end())
    return std::nullopt;
  const auto* value = std::get_if<std::u16string>(&found->second);
  return value ? std::optional{*value} : std::nullopt;
}

bool ProfilePrefsStore::HasKey(std::u16string_view key) const
{
  std::scoped_lock lock(mutex_);
  return values_.contains(key);
}

void ProfilePrefsStore::DeleteKey(std::u16string_view key)
{
  std::scoped_lock lock(mutex_);
  if (!values_.contains(key)) {
    if (file_exists_ && !std::filesystem::is_regular_file(file_path_))
      throw std::runtime_error("isolated preferences are unavailable");
    return;
  }
  auto next = values_;
  next.erase(key);
  Persist(next);
  values_.swap(next);
}

void ProfilePrefsStore::DeleteAll()
{
  std::scoped_lock lock(mutex_);
  if (values_.empty() && file_exists_) {
    if (!std::filesystem::is_regular_file(file_path_))
      throw std::runtime_error("isolated preferences are unavailable");
    return;
  }
  Values empty;
  Persist(empty);
  values_.swap(empty);
}

void ProfilePrefsStore::Save()
{
  std::scoped_lock lock(mutex_);
  if (!file_exists_)
    Persist(values_);
  if (!std::filesystem::is_regular_file(file_path_))
    throw std::runtime_error("isolated preferences are unavailable");
}

void ProfilePrefsStore::FinishNewProfile()
{
  std::scoped_lock lock(mutex_);
  if (!file_exists_)
    Persist(values_);
  else
    MarkInitialized();
}

} // namespace stfc::profiles::windows

#endif
