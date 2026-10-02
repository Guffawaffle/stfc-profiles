// Extracted preference semantics: see docs/PROVENANCE.json and LICENSE (GPL-3.0).
#include "stfc_profiles/prefs_store.h"
#include "stfc_profiles/identity.h"
#include "prefs_crypto.h"
#include <array>
#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#if _WIN32
#include <Windows.h>
#endif

namespace stfc::profiles {
namespace {
constexpr std::array<std::uint8_t, 8> magic{'S', 'T', 'F', 'C', 'P', 'R', 'E', 'F'};
constexpr std::uint32_t schema_version = 2;
constexpr std::size_t max_plain_bytes = detail::MaxPlainPrefsBytes;

[[noreturn]] void InvalidStore()
{ throw std::runtime_error("invalid isolated preference store"); }

void ValidateArtifact(const std::filesystem::path& file)
{
  std::error_code error;
  const auto status=std::filesystem::symlink_status(file,error);
  if (status.type()==std::filesystem::file_type::not_found) return;
  if (error || !std::filesystem::is_regular_file(status))
    throw std::runtime_error("profile preference artifact is unavailable or redirected");
#if _WIN32
  const auto attributes=GetFileAttributesW(file.c_str());
  if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("profile preference artifact is unavailable or redirected");
#endif
}

void AppendByte(std::vector<std::uint8_t>& output, std::uint8_t value)
{ output.push_back(value); }

void AppendU16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
  AppendByte(output, static_cast<std::uint8_t>(value));
  AppendByte(output, static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8)
    AppendByte(output, static_cast<std::uint8_t>(value >> shift));
}

void AppendString(std::vector<std::uint8_t>& output, std::u16string_view value)
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
  Reader(const std::uint8_t* data, std::size_t size) : bytes_(data, size) {}

  std::uint8_t Byte()
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
  std::span<const std::uint8_t> bytes_;
  std::size_t offset_ = 0;
};

} // namespace

ProfilePrefsStore::ProfilePrefsStore(const std::filesystem::path& root, std::string_view id,
                                     ProfileOpenMode mode, SessionLease& lease)
  : lease_(&lease), id_(id)
{
  std::error_code root_error;
  const bool root_matches = lease.Owns()
      && root.native().find(std::filesystem::path::value_type{}) == root.native().npos
      && std::filesystem::equivalent(root, lease.Root(), root_error) && !root_error;
  if ((mode != ProfileOpenMode::New && mode != ProfileOpenMode::Resume && mode != ProfileOpenMode::Existing)
      || !ValidId(id) || !lease.Owns() || lease.Id() != id
      || !root_matches)
    throw std::runtime_error("preference store requires the matching live profile lease");
  if (lease.PreferencesInitialized() && mode != ProfileOpenMode::Existing)
    throw std::runtime_error("established profile requires Existing preference mode");
  profile_id_.assign(id.begin(), id.end());
  const auto directory = lease.Directory();
  if (!std::filesystem::is_directory(directory))
    throw std::runtime_error("active profile directory is unavailable");
  file_path_ = directory / "player_prefs.bin";
  initialized_path_ = directory / "player_prefs.bin.initialized";
  file_exists_ = std::filesystem::exists(file_path_);
  initialized_ = std::filesystem::exists(initialized_path_);
  auto backup = file_path_;
  backup += ".bak";
  for (const auto& artifact : {file_path_,initialized_path_,backup}) ValidateArtifact(artifact);
  if (mode == ProfileOpenMode::New) {
    if (file_exists_ || initialized_ || std::filesystem::exists(backup))
      InvalidStore();
    for (const auto& entry : std::filesystem::directory_iterator(directory))
      if (entry.path().filename().string().starts_with("player_prefs.bin.tmp."))
        InvalidStore();
  } else {
    if (!file_exists_ && std::filesystem::exists(backup)) {
      values_ = LoadStore(backup);
      detail::RecoverPrefsBackup(backup, file_path_);
      file_exists_ = true;
    } else if (file_exists_) {
      values_ = LoadStore(file_path_);
    } else if (mode == ProfileOpenMode::Existing || initialized_) {
      InvalidStore();
    }
    if (file_exists_ && std::filesystem::exists(backup))
      detail::ErasePrefsFile(backup);
    for (const auto& entry : std::filesystem::directory_iterator(directory))
      if (entry.path().filename().string().starts_with("player_prefs.bin.tmp."))
        detail::ErasePrefsFile(entry.path());
  }
}

void ProfilePrefsStore::RequireLease() const
{
  if (!lease_->Owns() || lease_->Id() != id_ || lease_->Directory() != file_path_.parent_path())
    throw std::runtime_error("preference store no longer owns its profile lease");
}

void ProfilePrefsStore::Serialize(const Values& values, std::u16string_view profile_id, std::vector<std::uint8_t>& output)
{
  if (values.size() > std::numeric_limits<std::uint32_t>::max())
    InvalidStore();
  output.insert(output.end(), magic.begin(), magic.end());
  AppendU32(output, schema_version);
  AppendString(output, profile_id);
  AppendU32(output, static_cast<std::uint32_t>(values.size()));
  for (const auto& [key, value] : values) {
    AppendString(output, key);
    if (const auto* number = std::get_if<std::int32_t>(&value)) {
      AppendByte(output, 1);
      AppendU32(output, std::bit_cast<std::uint32_t>(*number));
    } else if (const auto* decimal = std::get_if<float>(&value)) {
      AppendByte(output, 2);
      AppendU32(output, std::bit_cast<std::uint32_t>(*decimal));
    } else if (const auto* text = std::get_if<std::u16string>(&value)) {
      AppendByte(output, 3);
      AppendString(output, *text);
    } else {
      const auto& native = std::get<NativePreference>(value);
      AppendByte(output, 4);
      AppendU32(output, native.type);
      AppendU32(output, static_cast<std::uint32_t>(native.bytes.size()));
      output.insert(output.end(), native.bytes.begin(), native.bytes.end());
    }
    if (output.size() > max_plain_bytes)
      InvalidStore();
  }
}

ProfilePrefsStore::Values ProfilePrefsStore::Deserialize(const std::uint8_t* data, std::size_t size) const
{
  Reader input(data, size);
  for (const auto expected : magic)
    if (input.Byte() != expected)
      InvalidStore();
  const auto version = input.U32();
  if ((version != 1 && version != schema_version) || input.String() != profile_id_)
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
    else if (kind == 4 && version >= 2) {
      NativePreference native{key, input.U32(), {}};
      const auto length = input.U32();
      if (length > input.Remaining()) InvalidStore();
      native.bytes.reserve(length);
      for (std::uint32_t offset = 0; offset < length; ++offset) native.bytes.push_back(input.Byte());
      value = std::move(native);
    } else
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
  RequireLease();
  ValidateArtifact(path);
  auto plain = detail::ReadProtectedPrefs(path, id_);
  struct Wipe { std::vector<std::uint8_t>& data; ~Wipe() { detail::WipePrefsBytes(data); } } wipe{plain};
  return Deserialize(plain.data(), plain.size());
}

void ProfilePrefsStore::Persist(const Values& values)
{
  RequireLease();
  ValidateArtifact(file_path_);
  std::vector<std::uint8_t> plain;
  struct Wipe { std::vector<std::uint8_t>& data; ~Wipe() { detail::WipePrefsBytes(data); } } wipe{plain};
  Serialize(values, profile_id_, plain);
  detail::WriteProtectedPrefs(file_path_, id_, plain, file_exists_);
  file_exists_ = true;
  MarkInitialized();
}

void ProfilePrefsStore::MarkInitialized()
{
  RequireLease();
  if (!initialized_) {
    detail::InitializePrefsMarker(initialized_path_);
    initialized_ = true;
  }
}

void ProfilePrefsStore::Set(std::u16string_view key, Value value)
{
  std::scoped_lock lock(mutex_);
  RequireLease();
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
  RequireLease();
  const auto found = values_.find(key);
  if (found == values_.end())
    return fallback;
  const auto* value = std::get_if<std::int32_t>(&found->second);
  if (value) return *value;
  if (const auto* native = std::get_if<NativePreference>(&found->second);
      native && (native->type == 3 || native->type == 4) && native->bytes.size() == 4) {
    std::uint32_t bits = 0;
    for (unsigned i = 0; i < 4; ++i) bits |= std::uint32_t(native->bytes[i]) << (8 * i);
    return std::bit_cast<std::int32_t>(bits);
  }
  return fallback;
}

float ProfilePrefsStore::GetFloat(std::u16string_view key, float fallback) const
{
  std::scoped_lock lock(mutex_);
  RequireLease();
  const auto found = values_.find(key);
  if (found == values_.end())
    return fallback;
  const auto* value = std::get_if<float>(&found->second);
  if (value) return *value;
  if (const auto* native = std::get_if<NativePreference>(&found->second);
      native && (native->type == 3 || native->type == 4) && native->bytes.size() == 8) {
    std::uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= std::uint64_t(native->bytes[i]) << (8 * i);
    return static_cast<float>(std::bit_cast<double>(bits));
  }
  return fallback;
}

std::optional<std::u16string> ProfilePrefsStore::GetString(std::u16string_view key) const
{
  std::scoped_lock lock(mutex_);
  RequireLease();
  const auto found = values_.find(key);
  if (found == values_.end())
    return std::nullopt;
  const auto* value = std::get_if<std::u16string>(&found->second);
  if (value) return *value;
  const auto* native = std::get_if<NativePreference>(&found->second);
  if (!native || (native->type != 3 && native->type != 1)) return std::nullopt;
  const auto& bytes = native->bytes;
  if (bytes.empty()) return std::nullopt;
  if (native->type == 1 && std::any_of(bytes.begin(), bytes.end(), [](auto byte) { return byte >= 128; }))
    return std::nullopt;
  std::u16string text;
  const auto nul = std::find(bytes.begin(), bytes.end(), std::uint8_t{});
  const auto limit = static_cast<std::size_t>(nul - bytes.begin());
  std::size_t position = 0;
  // Exact current Unity decoder: masks continuation bytes without validating
  // UTF-8. The native read buffer has one appended NUL. Reads beyond that
  // allocation are undefined in Unity; our bounded equivalent returns default.
  auto next = [&]() -> std::optional<std::uint32_t> {
    if (position > bytes.size()) return std::nullopt;
    return position < bytes.size() ? bytes[position++] : (++position, 0);
  };
  while (position < limit) {
    const auto lead = bytes[position++];
    unsigned tails = (lead & 0xe0) == 0xc0 ? 1 : (lead & 0xf0) == 0xe0 ? 2 : (lead & 0xf8) == 0xf0 ? 3 : 0;
    std::uint32_t code = tails ? lead & ((1u << (6 - tails)) - 1) : lead;
    for (unsigned i = 0; i < tails; ++i) {
      const auto tail = next();
      if (!tail) return std::nullopt;
      code = (code << 6) | (*tail & 63);
    }
    if (code <= 0xffff) text.push_back(static_cast<char16_t>(code));
    else {
      text.push_back(static_cast<char16_t>((code >> 10) - 0x2840));
      text.push_back(static_cast<char16_t>((code & 0x3ff) - 0x2400));
    }
  }
  return text;
}

bool ProfilePrefsStore::HasKey(std::u16string_view key) const
{
  std::scoped_lock lock(mutex_);
  RequireLease();
  return values_.contains(key);
}

void ProfilePrefsStore::DeleteKey(std::u16string_view key)
{
  std::scoped_lock lock(mutex_);
  RequireLease();
  if (!values_.contains(key)) {
    if (file_exists_ && !std::filesystem::is_regular_file(file_path_))
      throw std::runtime_error("isolated preferences are unavailable");
    return;
  }
  auto next = values_;
  next.erase(next.find(key));
  Persist(next);
  values_.swap(next);
}

void ProfilePrefsStore::DeleteAll()
{
  std::scoped_lock lock(mutex_);
  RequireLease();
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
  RequireLease();
  if (!file_exists_)
    Persist(values_);
  if (!std::filesystem::is_regular_file(file_path_))
    throw std::runtime_error("isolated preferences are unavailable");
}

void ProfilePrefsStore::FinishNewProfile()
{
  std::scoped_lock lock(mutex_);
  RequireLease();
  if (!file_exists_)
    Persist(values_);
  else
    MarkInitialized();
}

void ProfilePrefsStore::CreateImported(const std::filesystem::path& staging, std::string_view id,
                                       const std::vector<NativePreference>& preferences)
{
  if (!ValidId(id) || staging.filename() != ".import-" + std::string(id)
      || staging.parent_path().filename() != "profiles")
    throw std::runtime_error("import requires a fresh catalog staging directory");
  ValidateArtifact(staging / "player_prefs.bin");
  if (std::filesystem::exists(staging / "player_prefs.bin") || preferences.empty())
    throw std::runtime_error("import staging is occupied or source preferences are empty");
  const auto status = std::filesystem::symlink_status(staging);
  if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
    throw std::runtime_error("import staging must be an ordinary directory");
#if _WIN32
  const auto attributes = GetFileAttributesW(staging.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("import staging must not be redirected");
#endif
  Values values;
  std::size_t total = 0;
  for (const auto& preference : preferences) {
    if (preference.key.size() > max_plain_bytes / 2 || preference.bytes.size() > max_plain_bytes)
      InvalidStore();
    total += preference.key.size() * 2 + preference.bytes.size() + 13;
    if (total > max_plain_bytes || !values.emplace(preference.key, preference).second) InvalidStore();
  }
  std::vector<std::uint8_t> plain;
  struct Wipe { std::vector<std::uint8_t>& data; ~Wipe() { detail::WipePrefsBytes(data); } } wipe{plain};
  Serialize(values, std::u16string(id.begin(), id.end()), plain);
  detail::WriteProtectedPrefs(staging / "player_prefs.bin", id, plain, false);
  detail::InitializePrefsMarker(staging / "player_prefs.bin.initialized");
}

} // namespace stfc::profiles
