#include "stfc_profiles/catalog.h"
#include "stfc_profiles/session.h"
#include "stfc_profiles/installation.h"
#include "stfc_profiles/identity.h"
#include "stfc_profiles/prefs_store.h"
#include "stfc_profiles/user_import.h"
#include "prefs_crypto.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <limits>
#include <set>
#include <thread>
#include <vector>
#if _WIN32
#include <Windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#else
#include <CommonCrypto/CommonDigest.h>
#include <cerrno>
#include <fcntl.h>
#include <libproc.h>
#include <pwd.h>
#include <spawn.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <unistd.h>
#include <sys/wait.h>
#include "macos/launch.h"
extern char** environ;
#endif

namespace stfc::profiles {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
[[noreturn]] void Fail(std::string code, std::string message)
{ throw CatalogError(std::move(code), std::move(message)); }
std::string Utf8(const fs::path& path)
{ const auto value = path.u8string(); return {value.begin(), value.end()}; }
fs::path Path(std::string_view value)
{
  if (value.find('\0') != value.npos) Fail("invalid_path", "Paths cannot contain an embedded NUL character.");
  return fs::u8path(value.begin(), value.end());
}
void ValidateId(std::string_view id)
{
  if (!ValidId(id))
    Fail("invalid_id", "A profile ID must contain exactly 32 lowercase hexadecimal characters.");
}
std::string Hash(std::string_view value)
{
  std::array<unsigned char, 32> bytes{};
#if _WIN32
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    Fail("hash_unavailable", "SHA-256 is unavailable.");
  const auto status = BCryptHash(algorithm, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
      static_cast<ULONG>(value.size()), bytes.data(), static_cast<ULONG>(bytes.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (status < 0) Fail("hash_failed", "SHA-256 failed.");
#else
  CC_SHA256(value.data(), static_cast<CC_LONG>(value.size()), bytes.data());
#endif
  constexpr char alphabet[] = "0123456789abcdef";
  std::string result;
  for (auto byte : bytes) { result += alphabet[byte >> 4]; result += alphabet[byte & 15]; }
  return result;
}
std::string NewId()
{
  std::array<unsigned char, 16> bytes{};
#if _WIN32
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    Fail("random_failed", "The operating system could not generate a profile ID.");
#else
  arc4random_buf(bytes.data(), bytes.size());
#endif
  constexpr char alphabet[] = "0123456789abcdef";
  std::string result;
  for (auto byte : bytes) { result += alphabet[byte >> 4]; result += alphabet[byte & 15]; }
  return result;
}
void Plain(const fs::path& path, bool directory)
{
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error || (directory ? !fs::is_directory(status) : !fs::is_regular_file(status))
      || fs::is_symlink(status))
    Fail("invalid_path", "Expected an ordinary " + std::string(directory ? "directory: " : "file: ") + Utf8(path));
#if _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    Fail("invalid_path", "Profile storage must not use a reparse point: " + Utf8(path));
#endif
}
#if _WIN32
fs::path WindowsPathName(const fs::path& path)
{
  auto name = path.lexically_normal().make_preferred().native();
  if (name.starts_with(L"\\\\?\\UNC\\")) name = L"\\\\" + name.substr(8);
  else if (name.starts_with(L"\\\\?\\")) name.erase(0, 4);
  auto normalized = fs::path(name).lexically_normal().make_preferred();
  if (normalized.has_relative_path() && normalized.filename().empty())
    normalized = normalized.parent_path();
  return normalized;
}
#endif
void CheckSharedRoot(const fs::path& physical)
{
#if _WIN32
  const auto expected = WindowsPathName(DefaultCatalogRoot());
  const auto actual = WindowsPathName(physical);
  if (CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL)
    Fail("root_redirected", "Windows redirected the shared Profiles folder into private app storage. Open Profiles from a desktop shortcut or an ordinary terminal.");
#endif
}
#if _WIN32
void CheckSharedLockHandle(const fs::path& requested, HANDLE handle)
{
  const auto expected = WindowsPathName(requested);
  const auto shared = WindowsPathName(DefaultCatalogRoot());
  const auto& name = expected.native();
  const auto& prefix = shared.native();
  if (name.size() <= prefix.size() || name[prefix.size()] != L'\\'
      || CompareStringOrdinal(name.c_str(), static_cast<int>(prefix.size()),
                              prefix.c_str(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL)
    return;
  std::vector<wchar_t> buffer(32768);
  const auto length = GetFinalPathNameByHandleW(handle, buffer.data(),
      static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length || length >= buffer.size())
    Fail("lock_failed", "Could not establish the physical shared lifecycle-lock path.");
  std::wstring physical(buffer.data(), length);
  if (physical.starts_with(L"\\\\?\\UNC\\")) physical = L"\\\\" + physical.substr(8);
  else if (physical.starts_with(L"\\\\?\\")) physical.erase(0, 4);
  if (CompareStringOrdinal(physical.c_str(), -1, name.c_str(), -1, TRUE) != CSTR_EQUAL)
    Fail("root_redirected", "Windows redirected the shared Profiles lifecycle lock into private app storage. Open Profiles from a desktop shortcut or an ordinary terminal.");
}
#endif
fs::path CatalogRoot(const fs::path& requested)
{
  if (requested.native().find(fs::path::value_type{}) != requested.native().npos)
    Fail("invalid_root", "The catalog root cannot contain an embedded NUL character.");
  if (!requested.is_absolute()) Fail("invalid_root", "The catalog root must be an absolute path.");
  fs::create_directories(requested);
  Plain(requested, true);
  const auto root = fs::canonical(requested);
#if _WIN32
  const auto normalized = WindowsPathName(requested);
  const auto shared = WindowsPathName(DefaultCatalogRoot());
  std::error_code identity_error;
  if (CompareStringOrdinal(normalized.c_str(), -1, shared.c_str(), -1, TRUE) == CSTR_EQUAL
      || (fs::equivalent(requested, shared, identity_error) && !identity_error))
    CheckSharedRoot(root);
#endif
  for (const auto* name : {"profiles", "archives", "installations", ".locks", "sessions"}) {
    fs::create_directories(root / name);
    Plain(root / name, true);
  }
  return root;
}
Json Parse(std::string_view text)
{
  std::vector<std::set<std::string>> objects;
  return Json::parse(text, [&](int, Json::parse_event_t event, Json& parsed) {
    if (event == Json::parse_event_t::object_start) objects.emplace_back();
    else if (event == Json::parse_event_t::key && !objects.back().insert(parsed.get<std::string>()).second)
      Fail("invalid_json", "Duplicate JSON keys are not supported.");
    else if (event == Json::parse_event_t::object_end) objects.pop_back();
    return true;
  });
}
std::string Read(const fs::path& path)
{
  Plain(path, false);
  const auto length = fs::file_size(path);
  if (length > 65536) Fail("file_too_large", "Metadata exceeds the supported size: " + Utf8(path));
  std::ifstream input(path, std::ios::binary);
  std::string text(static_cast<std::size_t>(length), '\0');
  if (!input || (length && !input.read(text.data(), static_cast<std::streamsize>(length))))
    Fail("read_failed", "Could not read metadata: " + Utf8(path));
  return text;
}
void Atomic(const fs::path& destination, std::string_view text)
{
  const auto temporary = destination.parent_path() / (".write-" + NewId() + ".tmp");
#if _WIN32
  const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) Fail("write_failed", "Could not create metadata temporary file.");
  DWORD written = 0;
  const auto success = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
      && written == text.size() && FlushFileBuffers(handle);
  CloseHandle(handle);
  if (!success || !MoveFileExW(temporary.c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    Fail("write_failed", "Could not commit metadata; the temporary file was preserved: " + Utf8(temporary));
#else
  const auto fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) Fail("write_failed", "Could not create metadata temporary file.");
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto count = write(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += static_cast<std::size_t>(count);
  }
  const bool synced = offset == text.size() && fsync(fd) == 0;
  close(fd);
  if (!synced || rename(temporary.c_str(), destination.c_str()) != 0)
    Fail("write_failed", "Could not commit metadata; the temporary file was preserved: " + Utf8(temporary));
  const auto parent = open(destination.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (parent < 0) Fail("write_failed", "Could not open metadata directory for durability.");
  const auto status = fsync(parent); close(parent);
  if (status != 0) Fail("write_failed", "Could not flush metadata directory.");
#endif
}
class Lock {
public:
  Lock() = default;
  Lock(const fs::path& path, bool shared, bool wait = false)
  {
    const auto deadline = Clock::now() + std::chrono::seconds(wait ? 10 : 0);
    do {
#if _WIN32
      handle_ = CreateFileW(path.c_str(), shared ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                           shared ? FILE_SHARE_READ : 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle_ != INVALID_HANDLE_VALUE) {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandleEx(handle_, FileAttributeTagInfo, &attributes, sizeof(attributes))
            || !GetFileInformationByHandle(handle_, &information)
            || (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
            || information.nNumberOfLinks != 1) {
          Release(); Fail("invalid_path", "Lifecycle locks must be ordinary, singly linked files.");
        }
        try { CheckSharedLockHandle(path, handle_); }
        catch (...) { Release(); throw; }
        return;
      }
      const auto error = GetLastError();
      if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION)
        Fail("lock_failed", "Could not open lifecycle lock: " + Utf8(path));
#else
      if (fs::exists(path)) Plain(path, false);
      handle_ = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (handle_ < 0) Fail("lock_failed", "Could not open lifecycle lock: " + Utf8(path));
      if (flock(handle_, (shared ? LOCK_SH : LOCK_EX) | LOCK_NB) == 0) return;
      const auto error = errno;
      close(handle_); handle_ = -1;
      if (error != EWOULDBLOCK && error != EAGAIN)
        Fail("lock_failed", "Could not acquire lifecycle lock: " + Utf8(path));
#endif
      if (Clock::now() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (true);
    Fail("busy", "Another session or operation is using this profile or catalog.");
  }
  ~Lock() { Release(); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
  Lock(Lock&& other) noexcept : handle_(other.handle_) { other.handle_ = Invalid; }
  Lock& operator=(Lock&& other) noexcept
  { if (this != &other) { Release(); handle_ = other.handle_; other.handle_ = Invalid; } return *this; }
  bool Owns() const { return handle_ != Invalid; }
private:
  void Release() {
    if (!Owns()) return;
#if _WIN32
    CloseHandle(handle_);
#else
    close(handle_);
#endif
    handle_ = Invalid;
  }
#if _WIN32
  static inline const HANDLE Invalid = INVALID_HANDLE_VALUE;
  HANDLE handle_ = Invalid;
#else
  static constexpr int Invalid = -1;
  int handle_ = Invalid;
#endif
};
std::string Name(std::string value)
{
  const auto first = value.find_first_not_of(' '), last = value.find_last_not_of(' ');
  if (first == value.npos) Fail("invalid_name", "Provide a profile display name.");
  value = value.substr(first, last - first + 1);
  std::size_t characters = 0;
  for (unsigned char ch : value) {
    if (ch < 32 || ch == 127) Fail("invalid_name", "Display names cannot contain control characters.");
    if ((ch & 0xc0) != 0x80) ++characters;
  }
  if (characters > 48) Fail("invalid_name", "Display names may contain at most 48 characters.");
  // The JSON serializer validates UTF-8 rather than silently replacing bytes.
  (void)Json(value).dump();
  return value;
}
std::string Game(std::string value)
{
  if (value.empty()) return value;
  const auto path = Path(value);
  if (!path.is_absolute()) Fail("invalid_installation", "Choose an absolute game installation path.");
  return Utf8(fs::weakly_canonical(path));
}
std::string OwnerUserId()
{
#if _WIN32
  return CurrentUserSid();
#else
  Fail("platform_unavailable", "The Windows-user Default descriptor is available on Windows only.");
#endif
}
bool WindowsUser(const Json& metadata)
{ return metadata.value("kind", std::string{"isolated"}) == "windows-user"; }
void Isolated(const Json& metadata)
{
  if (WindowsUser(metadata))
    Fail("profile_kind", "Default uses the existing Windows setup. It cannot enter isolated profile storage or lifecycle operations.");
}
struct PhysicalInstallation { fs::path directory; std::string identity; };
PhysicalInstallation ObserveInstallation(const fs::path& requested)
{
#if _WIN32
  if (!requested.is_absolute()) Fail("invalid_installation", "Choose an absolute game installation path.");
  const auto handle = CreateFileW(requested.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) Fail("installation_unknown", "The registered installation is missing or cannot be inspected.");
  struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{handle};
  FILE_ID_INFO identity{}; FILE_STANDARD_INFO information{};
  wchar_t path[32768]{};
  const auto length = GetFinalPathNameByHandleW(handle, path, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity))
      || !GetFileInformationByHandleEx(handle, FileStandardInfo, &information, sizeof(information))
      || !information.Directory || !length || length >= 32768)
    Fail("installation_unknown", "The installation's physical directory identity could not be established.");
  std::string fingerprint(reinterpret_cast<const char*>(&identity.VolumeSerialNumber), sizeof(identity.VolumeSerialNumber));
  fingerprint.append(reinterpret_cast<const char*>(identity.FileId.Identifier), sizeof(identity.FileId.Identifier));
  return {WindowsPathName(fs::path(std::wstring(path,length))), Hash(fingerprint)};
#else
  Fail("platform_unavailable", "Installation registration requires qualified native directory identity. Windows is implemented; macOS registration is pending.");
#endif
}
void ValidateInstallationImage(const fs::path& game)
{
  Plain(game,true); Plain(game / "prime.exe",false); Plain(game / "GameAssembly.dll",false);
  Plain(game / "UnityPlayer.dll",false); Plain(game / "prime_Data",true);
  const auto marker = Read(game / ".version");
  std::string_view version=marker;
  while (!version.empty() && (version.back()=='\r' || version.back()=='\n')) version.remove_suffix(1);
  if (!version.starts_with("&game="))
    Fail("invalid_installation", "Choose an STFC game folder with the official client version marker.");
  version.remove_prefix(6);
  if (version.empty() || version.size()>10 || version.find_first_not_of("0123456789")!=version.npos
      || std::stoull(std::string(version))==0 || std::stoull(std::string(version))>2147483647)
    Fail("invalid_installation", "Choose an STFC game folder with a valid client version marker.");
}
struct InstallationEntry { Json metadata, projection; fs::path directory; std::string revision; };
InstallationEntry RegisteredInstallation(const fs::path& root, std::string_view id, bool require_available = false,
                                        bool allow_incomplete_image = false)
{
  ValidateId(id);
  const auto directory = root / "installations" / id;
  if (!fs::exists(directory)) Fail("installation_missing", "The selected installation registration was not found.");
  Plain(directory,true); const auto raw = Read(directory / "metadata.json"); const auto metadata = Parse(raw);
  if (!metadata.is_object() || !metadata.contains("schemaVersion")
      || !metadata["schemaVersion"].is_number_integer() || metadata["schemaVersion"] != Json(1)
      || !metadata.contains("name") || !metadata["name"].is_string()
      || !metadata.contains("gameDirectory") || !metadata["gameDirectory"].is_string()
      || !metadata.contains("physicalIdentity") || !metadata["physicalIdentity"].is_string()
      || metadata["physicalIdentity"].get<std::string>().size() != 64
      || metadata["physicalIdentity"].get<std::string>().find_first_not_of("0123456789abcdef") != std::string::npos)
    Fail("invalid_metadata", "Installation registration metadata is malformed.");
  Name(metadata.at("name").get<std::string>());
  const auto game = Path(metadata.at("gameDirectory").get<std::string>());
  if (!game.is_absolute()) Fail("invalid_metadata", "Registered installation paths must be absolute.");
  for (const auto& item:fs::directory_iterator(directory))
    if (item.path().filename() != "metadata.json")
      Fail("interrupted_write", "Installation metadata has an interrupted or unknown file. Inspect it before continuing.");
  std::string state = "unknown", message;
  bool identity_verified = false;
  try {
    const auto observed = ObserveInstallation(game);
    if (observed.identity != metadata.at("physicalIdentity").get<std::string>())
      Fail("installation_changed", "The directory at the registered path has changed. Choose or register the intended installation explicitly.");
    identity_verified = true;
    ValidateInstallationImage(observed.directory);
    state = "available";
  } catch (const CatalogError& error) {
    // Recovery must reach its journal when an interrupted commit has removed
    // image files, but it must still identify the original physical directory.
    if (require_available && (!allow_incomplete_image || !identity_verified)) throw;
    message = error.what();
  }
  const auto revision = Hash(raw);
  Json projection{{"id",id},{"name",metadata.at("name")},{"gameDirectory",metadata.at("gameDirectory")},
      {"physicalIdentity",metadata.at("physicalIdentity").get<std::string>()},{"revision",revision},{"state",state}};
  if (!message.empty()) projection["message"] = message;
  return {metadata, projection, directory, revision};
}
Json InstallationRegistrations(const fs::path& root)
{
  Json installations=Json::array(), issues=Json::array(); std::vector<fs::path> paths;
  for (const auto& item:fs::directory_iterator(root / "installations")) paths.push_back(item.path());
  std::sort(paths.begin(),paths.end()); std::string digest; std::set<std::string> identities;
  for (const auto& path:paths) {
    const auto id=Utf8(path.filename());
    try {
      const auto entry=RegisteredInstallation(root,id);
      if (!identities.insert(entry.metadata.at("physicalIdentity").get<std::string>()).second)
        Fail("duplicate_installation", "More than one registration identifies the same physical installation. Inspect the catalog before selecting it.");
      installations.push_back(entry.projection); digest += id+":"+entry.revision+"\n";
    } catch (const std::exception& error) {
      const auto* typed=dynamic_cast<const CatalogError*>(&error);
      issues.push_back({{"id",id},{"code",typed?typed->Code():"invalid_metadata"},{"message",error.what()}});
      digest += id+":invalid:"+error.what()+"\n";
    }
  }
  return {{"apiVersion",2},{"ok",true},{"installations",installations},{"issues",issues},{"revision",Hash(digest)}};
}
Json RegisterInstallation(const fs::path& root, const Json& request)
{
  const auto name=Name(request.at("name").get<std::string>());
  const auto observed=ObserveInstallation(Path(request.at("gameDirectory").get<std::string>()));
  ValidateInstallationImage(observed.directory);
  std::unique_ptr<InstallationEntry> existing;
  for (const auto& item:fs::directory_iterator(root / "installations")) {
    const auto entry=RegisteredInstallation(root,Utf8(item.path().filename()));
    if (entry.metadata.at("physicalIdentity").get<std::string>() != observed.identity) continue;
    if (existing) Fail("duplicate_installation", "More than one registration identifies this physical installation.");
    existing=std::make_unique<InstallationEntry>(entry);
  }
  if (existing) {
    // A moved original still has its identity, but registration is not relocation.
    // Selection cannot silently rewrite a missing path or its update journal.
    if (existing->projection.at("state") != "available")
      Fail("installation_unknown", "This physical installation has a stale registration. Relocation needs an explicit reviewed operation; the saved path was preserved.");
    return {{"apiVersion",2},{"ok",true},{"installation",existing->projection},{"created",false}};
  }
  const auto id=NewId(); const auto destination=root / "installations" / id;
  if (fs::exists(destination)) Fail("duplicate_id", "The generated installation ID already exists. Try again.");
  const auto staging=root / "installations" / (".register-"+id);
  if (!fs::create_directory(staging)) Fail("write_failed", "Installation registration could not be staged.");
  Atomic(staging / "metadata.json",Json{{"schemaVersion",1},{"name",name},
      {"gameDirectory",Utf8(observed.directory)},{"physicalIdentity",observed.identity}}.dump(2)+"\n");
  fs::rename(staging,destination);
  return {{"apiVersion",2},{"ok",true},{"installation",RegisteredInstallation(root,id,true).projection},{"created",true}};
}
std::string SelectedGame(const fs::path& root, const Json& request, const Json& metadata,
                         bool allow_incomplete_image = false)
{
  const auto selected = request.value("installationId", request.contains("gameDirectory") ? std::string{} : metadata.value("preferredInstallationId",std::string{}));
  if (!selected.empty()) {
    const auto entry=RegisteredInstallation(root,selected,true,allow_incomplete_image);
    const auto game=entry.metadata.at("gameDirectory").get<std::string>();
    if (request.contains("gameDirectory") && ObserveInstallation(Path(request.at("gameDirectory").get<std::string>())).identity != entry.metadata.at("physicalIdentity").get<std::string>())
      Fail("installation_changed", "The explicit game path does not match the selected installation registration.");
    return game;
  }
  return Game(request.value("gameDirectory",metadata.value("gameDirectory",std::string{})));
}
struct Entry { Json metadata; Json public_data; std::string revision; fs::path directory; };
Entry Load(const fs::path& root, std::string_view id, bool archived)
{
  ValidateId(id);
  const auto active = root / "profiles" / id, inactive = root / "archives" / id;
  if (fs::exists(active) && fs::exists(inactive))
    Fail("duplicate_id", "The same ID exists in both active and archived profiles.");
  const auto directory = archived ? inactive : active;
  if (!fs::exists(directory)) Fail(archived ? "archived_profile_missing" : "profile_missing",
      archived ? "The archived profile was not found." : "The active profile was not found; restore an archived profile before launching.");
  Plain(directory, true);
  const auto raw = Read(directory / "metadata.json");
  const auto metadata = Parse(raw);
  if (!metadata.is_object() || !metadata.contains("schemaVersion")
      || !metadata.contains("name") || !metadata["name"].is_string())
    Fail("invalid_metadata", "Profile metadata is incomplete or uses an unsupported version: " + Utf8(directory));
  const bool ordinary = metadata["schemaVersion"] == 2;
  if (ordinary) {
    if (metadata.value("kind", std::string{}) != "windows-user"
        || metadata.value("name", std::string{}) != "Default"
        || !metadata.contains("ownerUserId") || !metadata["ownerUserId"].is_string()
        || metadata["ownerUserId"].get<std::string>() != OwnerUserId())
      Fail("invalid_metadata", "The Default descriptor must identify the current Windows user.");
    if (archived) Fail("profile_kind", "The Windows setup cannot be archived.");
    for (auto property = metadata.begin(); property != metadata.end(); ++property)
      if (property.key() != "schemaVersion" && property.key() != "kind" && property.key() != "name"
          && property.key() != "ownerUserId" && property.key() != "gameDirectory" && property.key() != "preferredInstallationId")
        Fail("invalid_metadata", "Default metadata must not contain isolated preference or lifecycle fields.");
    for (const auto& file : fs::directory_iterator(directory))
      if (file.path().filename() != "metadata.json" && !file.path().filename().string().starts_with(".write-"))
        Fail("invalid_metadata", "Default contains descriptor metadata only; external Windows setup data must stay outside it.");
  } else if (metadata["schemaVersion"] != 1 || metadata.contains("kind")
      || !metadata.contains("preferencesInitialized") || !metadata["preferencesInitialized"].is_boolean())
    Fail("invalid_metadata", "Profile metadata is incomplete or uses an unsupported version: " + Utf8(directory));
  Name(metadata["name"].get<std::string>());
  if (metadata.contains("gameDirectory") && !metadata["gameDirectory"].is_string())
    Fail("invalid_metadata", "Preferred installation metadata must be a path string.");
  const auto game = Game(metadata.value("gameDirectory", std::string{}));
  if (metadata.contains("preferredInstallationId") && !metadata["preferredInstallationId"].is_string())
    Fail("invalid_metadata", "Preferred installation identity must be a string.");
  const auto preferred = metadata.value("preferredInstallationId",std::string{});
  if (!preferred.empty()) ValidateId(preferred);
  // Unresolved interrupted metadata writes must be inspected before mutation.
  for (const auto& file : fs::directory_iterator(directory))
    if (file.path().filename().string().starts_with(".write-"))
      Fail("interrupted_write", "An interrupted metadata write needs inspection: " + Utf8(file.path()));
  const auto revision = Hash(std::string(archived ? "archived\n" : "active\n") + raw);
  Json projection{{"id", id}, {"name", metadata["name"]}, {"gameDirectory", game},
      {"directory", Utf8(directory)}, {"revision", revision}, {"state", archived ? "archived" : "active"},
      {"kind", ordinary ? "windows-user" : "isolated"}, {"builtIn", ordinary},
      {"preferenceScope", ordinary ? "windows-user" : "profile"},
      {"configurationScope", ordinary ? "installation" : "profile"}};
  projection["preferredInstallationId"] = preferred;
  if (!preferred.empty()) {
    try { projection["installationState"] = RegisteredInstallation(root,preferred).projection.at("state"); }
    catch (const CatalogError&) { projection["installationState"] = "unknown"; }
  }
  if (ordinary) projection["ownerUserId"] = metadata["ownerUserId"];
  else {
    projection["configPath"] = Utf8(directory / "config.toml");
    projection["logPath"] = Utf8(directory / "logs" / "Player.log");
    projection["preferencesInitialized"] = metadata["preferencesInitialized"];
  }
  return {metadata, projection, revision, directory};
}
Json Discover(const fs::path& root, bool archived, int api_version = 2)
{
  Json profiles = Json::array(), issues = Json::array();
  std::vector<fs::path> paths;
  for (const auto& item : fs::directory_iterator(root / (archived ? "archives" : "profiles")))
    paths.push_back(item.path());
  std::sort(paths.begin(), paths.end());
  std::string digest;
  for (const auto& path : paths) {
    const auto id = Utf8(path.filename());
    try {
      const auto entry = Load(root, id, archived);
      if (api_version == 1 && WindowsUser(entry.metadata)) continue;
      profiles.push_back(entry.public_data);
      digest += id + ":" + entry.revision + "\n";
    } catch (const std::exception& error) {
      const auto* typed = dynamic_cast<const CatalogError*>(&error);
      issues.push_back({{"id", id}, {"path", Utf8(path)},
                        {"code", typed ? typed->Code() : "invalid_metadata"}, {"message", error.what()}});
      digest += id + ":invalid:" + error.what() + "\n";
    }
  }
  return {{"apiVersion", 1}, {"ok", true}, {"revision", Hash(digest)},
          {"profiles", profiles}, {"issues", issues}};
}
Entry DefaultEntry(const fs::path& root, bool ensure)
{
  std::unique_ptr<Entry> selected;
  // Invalid or interrupted entries must be inspected before a second built-in
  // identity could be published. Directory names remain the ID authority.
  for (const auto& item : fs::directory_iterator(root / "profiles")) {
    const auto entry = Load(root, Utf8(item.path().filename()), false);
    if (!WindowsUser(entry.metadata)) continue;
    if (selected) Fail("duplicate_default", "More than one Windows setup descriptor exists. Inspect the catalog before continuing.");
    selected = std::make_unique<Entry>(entry);
  }
  for (const auto& item : fs::directory_iterator(root / "archives")) {
    (void)Load(root, Utf8(item.path().filename()), true);
  }
  if (selected) return *selected;
  if (!ensure) Fail("default_missing", "Default has not been registered in this catalog. Ensure Default before selecting it.");
  const auto owner = OwnerUserId();
  const auto id = NewId();
  if (fs::exists(root / "profiles" / id) || fs::exists(root / "archives" / id))
    Fail("duplicate_id", "The generated Default ID already exists. Try again.");
  const auto staging = root / "profiles" / (".default-" + id);
  if (!fs::create_directory(staging)) Fail("write_failed", "Default could not be staged.");
  Atomic(staging / "metadata.json", Json{{"schemaVersion", 2}, {"kind", "windows-user"},
      {"name", "Default"}, {"ownerUserId", owner}}.dump(2) + "\n");
  fs::rename(staging, root / "profiles" / id);
  return Load(root, id, false);
}
void TypedAccess(const Json& metadata, int api_version)
{
  if (api_version == 1 && WindowsUser(metadata))
    Fail("api_version", "Default requires the typed catalog API version 2. Update this Profiles client.");
}
void Revision(const Json& request, const Entry& entry, const fs::path& root, bool archived)
{
  if (!request.contains("expectedRevision") || !request["expectedRevision"].is_string())
    Fail("revision_required", "Read the profile before modifying it and supply its expected revision.");
  const auto expected = request["expectedRevision"].get<std::string>();
  if (expected != entry.revision && expected != Discover(root, archived, request.value("apiVersion", 1))["revision"].get<std::string>())
    Fail("stale_revision", "The profile changed; refresh it before retrying.");
}
std::uint32_t CurrentPid()
{
#if _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<std::uint32_t>(getpid());
#endif
}
// Null means proven absent. Failure to inspect a potentially live process is
// an explicit failure rather than permission to delete another owner's data.
Json Process(std::uint32_t pid)
{
#if _WIN32
  const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!handle) {
    if (GetLastError() == ERROR_INVALID_PARAMETER) return nullptr;
    Fail("process_unobservable", "A session process could not be inspected safely.");
  }
  struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{handle};
  if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) return nullptr;
  FILETIME created{}, exited{}, kernel{}, user{};
  wchar_t path[32768]{}; DWORD count = 32768;
  if (!GetProcessTimes(handle, &created, &exited, &kernel, &user)
      || !QueryFullProcessImageNameW(handle, 0, path, &count))
    Fail("process_unobservable", "A session process identity could not be inspected safely.");
  const auto start = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  return {{"processId", pid}, {"started", std::to_string(start)}, {"executable", Utf8(fs::path(path))}};
#else
  struct proc_bsdinfo info{};
  if (proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) {
    if (kill(static_cast<pid_t>(pid), 0) < 0 && errno == ESRCH) return nullptr;
    Fail("process_unobservable", "A session process could not be inspected safely.");
  }
  if (info.pbi_status == SZOMB) return nullptr;
  char path[PROC_PIDPATHINFO_MAXSIZE]{};
  if (proc_pidpath(static_cast<int>(pid), path, sizeof(path)) <= 0)
    Fail("process_unobservable", "A session executable path could not be inspected safely.");
  return {{"processId", pid}, {"started", std::to_string(info.pbi_start_tvsec) + ":" + std::to_string(info.pbi_start_tvusec)},
          {"executable", path}};
#endif
}
bool Same(const Json& lhs, const Json& rhs)
{
  return !lhs.is_null() && !rhs.is_null() && lhs.value("processId", 0u) == rhs.value("processId", 0u)
      && lhs.value("started", std::string{}) == rhs.value("started", std::string{})
      && lhs.value("executable", std::string{}) == rhs.value("executable", std::string{});
}
Json Receipt(const fs::path& root, std::string_view id)
{
  const auto path = root / "sessions" / (std::string(id) + ".json");
  if (!fs::exists(path)) return nullptr;
  const auto receipt = Parse(Read(path));
  if (!receipt.is_object() || receipt.value("apiVersion", 0) != 1 || receipt.value("id", std::string{}) != id
      || !receipt.contains("processId") || !receipt["processId"].is_number_unsigned()
      || !receipt.contains("started") || !receipt["started"].is_string()
      || !receipt.contains("executable") || !receipt["executable"].is_string()
      || !receipt.contains("phase") || !receipt["phase"].is_string())
    Fail("invalid_session", "Session evidence is malformed: " + Utf8(path));
  const auto pid = receipt["processId"].get<std::uint64_t>();
  const auto phase = receipt["phase"].get<std::string>();
  if (!pid || pid > std::numeric_limits<std::uint32_t>::max()
      || (phase != "pending" && phase != "initializing" && phase != "ready" && phase != "failed")
      || receipt["started"].get<std::string>().empty()
      || !Path(receipt["executable"].get<std::string>()).is_absolute())
    Fail("invalid_session", "Session identity or phase is outside its supported range: " + Utf8(path));
  const auto current = Process(receipt["processId"].get<std::uint32_t>());
  return Same(receipt, current) ? receipt : Json(nullptr);
}
void Publish(const fs::path& root, std::string_view id, Json identity, std::string_view phase,
             std::string_view reason = {})
{
  identity["apiVersion"] = 1; identity["id"] = id; identity["phase"] = phase;
  if (!reason.empty()) identity["reason"] = reason;
  Atomic(root / "sessions" / (std::string(id) + ".json"), identity.dump(2) + "\n");
}
#if __APPLE__
bool MacBrowserActive(const fs::path& root, std::string_view id)
{
  const auto prefix = "browser-" + std::string(id) + "-";
  for (const auto& file : fs::directory_iterator(root / ".locks")) {
    if (!file.path().filename().string().starts_with(prefix)) continue;
    const auto record = Parse(Read(file.path()));
    if (!record.is_object() || record.value("apiVersion", Json{}) != 1 || record.value("id", Json{}) != id
        || !record.contains("processId") || !record["processId"].is_number_unsigned()
        || !record.contains("started") || !record["started"].is_string()
        || record["started"].get<std::string>().empty()
        || !record.contains("executable") || !record["executable"].is_string()
        || !Path(record["executable"].get<std::string>()).is_absolute())
      Fail("invalid_browser_session", "The browser identity record needs inspection.");
    const auto value = record["processId"].get<std::uint64_t>();
    if (!value || value > std::numeric_limits<std::int32_t>::max()
        || file.path().filename() != prefix + std::to_string(value) + ".json")
      Fail("invalid_browser_session", "The browser process group record is malformed.");
    const auto pid = static_cast<std::uint32_t>(value);
    if (Same(record, Process(pid))) return true;
    // The leader can exit while helpers still use its profile. A reused group
    // is conservatively busy; no process is killed on this observation path.
    errno = 0;
    const auto bytes = proc_listpids(PROC_PGRP_ONLY, pid, nullptr, 0);
    if (bytes < 0 || (!bytes && errno)) Fail("browser_unobservable", "The browser process group could not be inspected.");
    if (!bytes) continue;
    std::vector<pid_t> members(bytes / sizeof(pid_t) + 32);
    errno = 0;
    const auto observed = proc_listpids(PROC_PGRP_ONLY, pid, members.data(), members.size() * sizeof(pid_t));
    if (observed < 0 || (!observed && errno) || observed >= members.size() * sizeof(pid_t))
      Fail("browser_unobservable", "The browser process group could not be inspected completely.");
    for (std::size_t i = 0; i < observed / sizeof(pid_t); ++i)
      if (members[i] > 0 && !Process(static_cast<std::uint32_t>(members[i])).is_null()) return true;
  }
  return false;
}
#endif
void Inactive(const fs::path& root, std::string_view id)
{
  // Caller already owns the writer lock. Only a not-yet-admitted live child
  // can own admission without that lock; released runtime receipts are stale.
  const auto receipt = Receipt(root, id);
  if (!receipt.is_null() && receipt["phase"] == "pending")
    Fail("profile_running", "Wait for this profile's pending launch before changing its directory state.");
#if __APPLE__
  if (MacBrowserActive(root, id))
    Fail("browser_running", "Quit this profile's isolated browser before changing its directory state or relaunching.");
#endif
}
Json Sessions(const fs::path& root)
{
  Json sessions = Json::array(), issues = Json::array();
  for (const auto& item : fs::directory_iterator(root / "sessions")) {
    const auto id = item.path().stem().string();
    if (item.path().extension() != ".json") continue;
    try {
      ValidateId(id);
      auto receipt = Receipt(root, id);
      if (receipt.is_null()) continue;
      // Pending admission is protected by the live exact child identity. A
      // claimed active runtime additionally needs its lifetime writer lock.
      if (receipt["phase"] != "pending") {
        try { Lock probe(root / ".locks" / (id + ".lock"), false); continue; }
        catch (const CatalogError& error) { if (error.Code() != "busy") throw; }
      }
      receipt["readiness"] = receipt["phase"] == "ready" ? "ready" : "initializing";
      sessions.push_back(receipt);
    } catch (const std::exception& error) {
      issues.push_back({{"id", id}, {"message", error.what()}});
    }
  }
  return {{"apiVersion", 1}, {"ok", true}, {"sessions", sessions}, {"issues", issues}};
}
#if _WIN32
std::wstring Quote(std::wstring_view argument)
{
  std::wstring output = L"\""; std::size_t slashes = 0;
  for (auto ch : argument) {
    if (ch == L'\\') { ++slashes; continue; }
    if (ch == L'\"') output.append(slashes * 2 + 1, L'\\');
    else output.append(slashes, L'\\');
    slashes = 0; output += ch;
  }
  output.append(slashes * 2, L'\\'); output += L'\"'; return output;
}
#endif
void InstallationReady(const fs::path& root, const fs::path& game)
{
#if _WIN32
  const auto result = Parse(ExecuteInstallationRequest(Json{{"apiVersion", 1},
      {"operation", "installation-status"}, {"root", Utf8(root)}, {"gameDirectory", Utf8(game)}}.dump()));
  if (!result.value("ok", false)) {
    const auto error = result.at("error");
    Fail(error.value("code", "installation_unavailable"), error.value("message", "The installation could not be inspected."));
  }
  if (result.at("installation").value("requiresRecovery", false))
    Fail("recovery_required", "Recover the unfinished game update before launching this installation.");
#endif
}
Json LaunchOrdinary(const fs::path& root, const Json& request)
{
#if _WIN32
  Lock catalog(root / ".locks" / "catalog.lock", false, true);
  const auto entry = Load(root, request.at("id").get<std::string>(), false);
  if (!WindowsUser(entry.metadata))
    Fail("profile_kind", "Ordinary launch requires the Windows setup descriptor. Launch isolated profiles through their isolation coordinator.");
  // Reject duplicate built-in identities before starting an ordinary process.
  if (DefaultEntry(root, false).public_data.at("id") != entry.public_data.at("id"))
    Fail("duplicate_default", "The selected Windows setup is not the catalog's Default descriptor.");
  const auto game = Path(SelectedGame(root, request, entry.metadata));
  if (game.empty()) Fail("installation_required", "Choose a game installation or save a preferred installation for Default.");
  InstallationLease installation(root, game, false);
  InstallationReady(root, game);
  const auto executable = fs::canonical(game / "prime.exe");
  Plain(executable, false);
  std::wstring command = Quote(executable.native());
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_SUSPENDED, nullptr, game.c_str(), &startup, &process))
    Fail("launch_failed", "Windows could not start the selected game executable.");
  Json identity;
  try {
    identity = Process(process.dwProcessId);
    if (identity.is_null()) Fail("game_exited", "The ordinary game process exited during startup.");
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
      Fail("launch_failed", "Windows could not resume the selected game.");
  } catch (...) {
    TerminateProcess(process.hProcess, 190); CloseHandle(process.hThread); CloseHandle(process.hProcess); throw;
  }
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  // Startup access closes after the exact process snapshot. Ordinary sessions
  // are subsequently observed by installation process inspection; they do not
  // acquire isolated writer/browser leases or publish isolation readiness.
  return {{"apiVersion", 2}, {"ok", true}, {"profile", entry.public_data},
      {"processId", identity.at("processId")}, {"started", identity.at("started")},
      {"executable", identity.at("executable")}, {"readiness", "ordinary"}, {"session", identity}};
#else
  Fail("platform_unavailable", "Windows setup ordinary launch is available on Windows only.");
#endif
}
Json Launch(const fs::path& root, const Json& request)
{
  Json identity, profile;
  std::unique_ptr<InstallationLease> installation;
  {
    Lock catalog(root / ".locks" / "catalog.lock", false, true);
    const auto id = request.at("id").get<std::string>(); ValidateId(id);
    const auto entry = Load(root, id, false);
    Isolated(entry.metadata); profile = entry.public_data;
    const auto game = Path(SelectedGame(root, request, entry.metadata));
    if (game.empty()) Fail("installation_required", "Choose a game installation with --game or save a preferred installation.");
    installation = std::make_unique<InstallationLease>(root, game, false);
    InstallationReady(root, game);
    // Hosts currently resolve the OS-user root, so a caller cannot accidentally
    // request a disposable catalog while the game opens a real account store.
    if (root != CatalogRoot(DefaultCatalogRoot()))
      Fail("launch_root_unsupported", "This runtime launches profiles only from the OS-user catalog.");
    Lock admission(root / ".locks" / (id + ".lock"), false);
    Inactive(root, id);
#if _WIN32
    const auto executable = fs::canonical(game / "prime.exe");
    Plain(executable, false); Plain(game / "version.dll", false);
    const auto module = LoadLibraryExW((game / "version.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) Fail("runtime_unavailable", "The selected installation's profile runtime could not be inspected.");
    const auto marker = reinterpret_cast<const unsigned int*>(GetProcAddress(module, "STFCProfilesExplicitLaunchContractV1"));
    unsigned int contract = 0; SIZE_T inspected = 0;
    const bool supported = marker && ReadProcessMemory(GetCurrentProcess(), marker, &contract,
        sizeof(contract), &inspected) && inspected == sizeof(contract) && contract == 1;
    FreeLibrary(module);
    if (!supported) Fail("runtime_unavailable", "Install the Profiles or community-mod runtime in the selected installation first.");
    const auto logs = entry.directory / "logs";
    if (!fs::exists(logs)) fs::create_directory(logs);
    Plain(logs, true);
    const auto log = logs / "Player.log";
    std::error_code log_error;
    const auto log_status = fs::symlink_status(log, log_error);
    if (log_error && log_error != std::errc::no_such_file_or_directory)
      Fail("invalid_path", "The profile log path could not be inspected.");
    if (log_status.type() != fs::file_type::not_found) Plain(log, false);
    std::wstring command = Quote(executable.native()) + L" -stfc-profile " + Path(id).native()
        + L" -logFile " + Quote(log.native());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, game.c_str(), &startup, &process))
      Fail("launch_failed", "Windows could not start the selected game executable.");
    try {
      identity = Process(process.dwProcessId);
      Publish(root, id, identity, "pending");
      // The child's early admission waits for catalog publication to finish.
      if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        Fail("launch_failed", "Windows could not resume the selected game.");
    } catch (...) {
      TerminateProcess(process.hProcess, 190); CloseHandle(process.hThread); CloseHandle(process.hProcess); throw;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
#else
    const auto executable = fs::canonical(game / "Star Trek Fleet Command");
    Plain(executable, false);
    if (!request.contains("runtimeLibrary"))
      Fail("runtime_required", "Choose the bundled Mac profile runtime with --runtime PATH.");
    const auto supplied = Path(request.at("runtimeLibrary").get<std::string>());
    if (!supplied.is_absolute()) Fail("runtime_unavailable", "The profile runtime path must be absolute.");
    Plain(supplied, false);
    const auto runtime = fs::canonical(supplied);
    if (!detail::HasMacLaunchContract(runtime))
      Fail("runtime_unavailable", "The runtime does not support explicit profiles on this Mac architecture.");
    detail::CheckMacLoaderEntitlements(executable);
    const auto logs = entry.directory / "logs";
    if (!fs::exists(logs)) fs::create_directory(logs);
    Plain(logs, true);
    const auto log = logs / "Player.log";
    if (fs::exists(fs::symlink_status(log))) Plain(log, false);
    const auto child = detail::SpawnMacProfileSuspended(executable, runtime, id, log);
    try {
      identity = Process(static_cast<std::uint32_t>(child));
      if (identity.is_null()) Fail("game_exited", "The Mac game exited during startup.");
      Publish(root, id, identity, "pending");
      if (kill(child, SIGCONT) != 0) Fail("launch_failed", "Could not resume the selected Mac game.");
    } catch (...) {
      // Only this still-suspended owned child is terminated on publication failure.
      kill(child, SIGKILL); waitpid(child, nullptr, 0); throw;
    }
#endif
  }
  const auto id = request.at("id").get<std::string>();
  const auto deadline = Clock::now() + std::chrono::seconds(60);
  std::string code = "readiness_timeout", message = "The game started but did not confirm profile isolation. It may still be running; inspect sessions before retrying.";
  while (Clock::now() < deadline) {
    try {
      Lock catalog(root / ".locks" / "catalog.lock", false, true);
      auto receipt = Receipt(root, id);
      if (Same(receipt, identity) && receipt["phase"] == "ready") {
        try { Lock probe(root / ".locks" / (id + ".lock"), false); }
        catch (const CatalogError& error) {
          if (error.Code() != "busy") throw;
          return {{"apiVersion", 1}, {"ok", true}, {"processId", identity["processId"]},
                  {"readiness", "ready"}, {"profile", Load(root, id, false).public_data}};
        }
      }
      if (Same(receipt, identity) && receipt["phase"] == "failed") {
        code = "isolation_failed"; message = receipt.value("reason", "Profile isolation failed."); break;
      }
      if (!Same(Process(identity["processId"].get<std::uint32_t>()), identity)) {
        code = "game_exited"; message = "The game exited before profile isolation was ready."; break;
      }
    } catch (const CatalogError& error) {
      if (error.Code() != "busy") { code = error.Code(); message = error.what(); break; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return {{"apiVersion", 1}, {"ok", false}, {"processId", identity["processId"]},
          {"error", {{"code", code}, {"message", message}}}};
}
} // namespace

fs::path DefaultCatalogRoot()
{
#if _WIN32
  PWSTR value = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_NO_PACKAGE_REDIRECTION, nullptr, &value)))
    Fail("root_unavailable", "The operating system could not locate this user's profile data directory.");
  fs::path root(value); CoTaskMemFree(value); return root / "STFC Profiles";
#else
  const auto user = getpwuid(getuid());
  if (!user || !user->pw_dir || !*user->pw_dir)
    Fail("root_unavailable", "The operating system could not locate this user's home directory.");
  return fs::path(user->pw_dir) / "Library/Application Support/STFC Profiles";
#endif
}
void CheckInstallationReady(const fs::path& root, const fs::path& gameDirectory)
{ InstallationReady(root, gameDirectory); }
#if _WIN32
struct InstallationDirectoryPin {
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~InstallationDirectoryPin() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
#endif
struct InstallationLease::Impl {
  fs::path root, directory; std::string key, physical_identity; Lock access;
#if _WIN32
  std::vector<std::unique_ptr<InstallationDirectoryPin>> directory_pins;
#endif
};
InstallationLease::InstallationLease(const fs::path& requested, const fs::path& game, bool exclusive, bool observation_only)
  : impl_(std::make_unique<Impl>())
{
  impl_->root = CatalogRoot(requested);
  if (exclusive && observation_only)
    Fail("invalid_operation","Observation-only installation access cannot authorize a mutation.");
  if (!game.is_absolute()) Fail("invalid_installation", "The game installation must be an absolute directory.");
  if (game.native().find(fs::path::value_type{}) != game.native().npos)
    Fail("invalid_installation", "The game path cannot contain an embedded NUL character.");
  impl_->directory = fs::canonical(game);
#if _WIN32
  auto supplied=WindowsPathName(game.lexically_normal());
  while(supplied!=supplied.root_path() && supplied.filename().empty()) supplied=supplied.parent_path();
  const auto retained=WindowsPathName(impl_->directory);
  if(CompareStringOrdinal(supplied.c_str(),-1,retained.c_str(),-1,TRUE)!=CSTR_EQUAL)
    Fail("installation_changed","Select the installation's canonical directory before acquiring operation access.");
  std::vector<fs::path> ancestors;
  for (auto path=impl_->directory;;path=path.parent_path()) {
    ancestors.push_back(path);
    if (path==path.root_path() || path==path.parent_path()) break;
  }
  std::reverse(ancestors.begin(),ancestors.end());
  for (const auto& path:ancestors) {
    auto pin=std::make_unique<InstallationDirectoryPin>();
    // Attribute-only handles do not participate in Windows share-access checks.
    const auto handle=pin->handle=CreateFileW(path.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle==INVALID_HANDLE_VALUE)
      Fail("installation_unknown","Could not retain the selected installation directory through this operation.");
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&attributes,sizeof(attributes))
        || !(attributes.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
      Fail("installation_changed","The installation directory chain changed during admission.");
    impl_->directory_pins.push_back(std::move(pin));
  }
  impl_->physical_identity=ObserveInstallation(impl_->directory).identity;
#endif
  std::string key_input = Utf8(impl_->directory);
#if _WIN32
  // Windows paths compare without case. Both update and runtime admission use
  // the same normalized key even when a shortcut changes path capitalization.
  const auto& native = impl_->directory.native();
  const auto count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
      native.c_str(), static_cast<int>(native.size()), nullptr, 0, nullptr, nullptr, 0);
  if (!count) Fail("invalid_installation", "Could not normalize the game installation identity.");
  std::wstring folded(count, L'\0');
  if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, native.c_str(),
      static_cast<int>(native.size()), folded.data(), count, nullptr, nullptr, 0))
    Fail("invalid_installation", "Could not normalize the game installation identity.");
  key_input = Utf8(fs::path(folded));
#endif
  impl_->key = Hash(key_input);
  // Status observes an active mutation without claiming its coordination lock.
  // Its directory handles still retain the selected namespace through the read.
  if (observation_only) return;
  // Installation identity is independent of a caller's profile catalog root.
  // Only lifecycle files are touched here; no other catalog is discovered.
  const auto shared_root = DefaultCatalogRoot();
  fs::create_directories(shared_root);
  Plain(shared_root, true);
  CheckSharedRoot(fs::canonical(shared_root));
  const auto lock_root = shared_root / ".locks";
  fs::create_directories(lock_root);
  Plain(lock_root.parent_path(), true);
  Plain(lock_root, true);
  // Preserve the expected neutral spelling until the actual acquired handle is
  // checked. Canonicalizing a virtualized child directory could hide redirection.
  impl_->access = Lock(lock_root / ("install-" + impl_->key + ".lock"), !exclusive);
}
InstallationLease::~InstallationLease() = default;
InstallationLease::InstallationLease(InstallationLease&&) noexcept = default;
InstallationLease& InstallationLease::operator=(InstallationLease&&) noexcept = default;
const fs::path& InstallationLease::Root() const { return impl_->root; }
const fs::path& InstallationLease::Directory() const { return impl_->directory; }
const std::string& InstallationLease::Key() const { return impl_->key; }
const std::string& InstallationLease::PhysicalIdentity() const { return impl_->physical_identity; }
bool InstallationLease::Owns() const noexcept { return impl_ && impl_->access.Owns(); }
struct SessionLease::Impl {
  fs::path root, directory; std::string id; Lock writer; Json identity;
  std::unique_ptr<InstallationLease> installation;
  bool initialized = false;
};
SessionLease::SessionLease(const fs::path& requested, std::string_view id) : impl_(std::make_unique<Impl>())
{
  ValidateId(id); impl_->root = CatalogRoot(requested); impl_->id = id;
  Lock catalog(impl_->root / ".locks" / "catalog.lock", false, true);
  const auto entry = Load(impl_->root, id, false);
  Isolated(entry.metadata);
  impl_->directory = entry.directory; impl_->initialized = entry.metadata["preferencesInitialized"];
  impl_->identity = Process(CurrentPid());
  impl_->installation = std::make_unique<InstallationLease>(impl_->root,
      Path(impl_->identity.at("executable").get<std::string>()).parent_path(), false);
#if _WIN32
  const auto executable = Path(impl_->identity.at("executable").get<std::string>());
  if (CompareStringOrdinal(executable.filename().c_str(), -1, L"prime.exe", -1, TRUE) == CSTR_EQUAL)
    InstallationReady(impl_->root, executable.parent_path());
#endif
  const auto pending = Receipt(impl_->root, id);
  if (!pending.is_null() && pending["phase"] == "pending" && !Same(pending, impl_->identity))
    Fail("profile_running", "This profile already belongs to another live game session.");
  impl_->writer = Lock(impl_->root / ".locks" / (std::string(id) + ".lock"), false);
  Publish(impl_->root, id, impl_->identity, "initializing");
}
SessionLease::~SessionLease() = default;
SessionLease::SessionLease(SessionLease&&) noexcept = default;
SessionLease& SessionLease::operator=(SessionLease&&) noexcept = default;
const fs::path& SessionLease::Root() const { return impl_->root; }
const fs::path& SessionLease::Directory() const { return impl_->directory; }
const std::string& SessionLease::Id() const { return impl_->id; }
bool SessionLease::Owns() const noexcept { return impl_ && impl_->writer.Owns(); }
bool SessionLease::PreferencesInitialized() const { return impl_->initialized; }
void SessionLease::MarkReady()
{
  if (!Owns()) Fail("lease_missing", "The session no longer owns this profile.");
  Lock catalog(impl_->root / ".locks" / "catalog.lock", false, true);
  auto entry = Load(impl_->root, impl_->id, false);
  Plain(entry.directory / "player_prefs.bin", false);
  Plain(entry.directory / "player_prefs.bin.initialized", false);
  if (!entry.metadata["preferencesInitialized"].get<bool>()) {
    entry.metadata["preferencesInitialized"] = true;
    Atomic(entry.directory / "metadata.json", entry.metadata.dump(2) + "\n");
  }
  impl_->initialized = true;
  Publish(impl_->root, impl_->id, impl_->identity, "ready");
}
void SessionLease::MarkFailed(std::string_view reason)
{
  if (!Owns()) return;
  Lock catalog(impl_->root / ".locks" / "catalog.lock", false, true);
  Publish(impl_->root, impl_->id, impl_->identity, "failed", reason);
}
struct BrowserLease::Impl { fs::path root, directory; std::string id; Lock data; };
BrowserLease::BrowserLease(const fs::path& requested, std::string_view id) : impl_(std::make_unique<Impl>())
{
  ValidateId(id); impl_->root = CatalogRoot(requested); impl_->id = id;
  Lock catalog(impl_->root / ".locks" / "catalog.lock", false, true);
  const auto entry = Load(impl_->root, id, false);
  Isolated(entry.metadata); impl_->directory = entry.directory;
  impl_->data = Lock(impl_->root / ".locks" / (std::string(id) + ".data.lock"), true);
}
BrowserLease::~BrowserLease() = default;
BrowserLease::BrowserLease(BrowserLease&&) noexcept = default;
BrowserLease& BrowserLease::operator=(BrowserLease&&) noexcept = default;
const fs::path& BrowserLease::Root() const { return impl_->root; }
const fs::path& BrowserLease::Directory() const { return impl_->directory; }
const std::string& BrowserLease::Id() const { return impl_->id; }
bool BrowserLease::Owns() const noexcept { return impl_ && impl_->data.Owns(); }
void BrowserLease::MarkBrowserStarted(std::uint32_t process_id)
{
#if __APPLE__
  if (!Owns()) Fail("lease_missing", "The browser no longer owns profile data access.");
  Lock catalog(impl_->root / ".locks" / "catalog.lock", false, true);
  if (!process_id || process_id > std::numeric_limits<std::int32_t>::max()
      || getpgid(static_cast<pid_t>(process_id)) != static_cast<pid_t>(process_id))
    Fail("invalid_browser_session", "The stopped browser must own its process group.");
  auto identity = Process(process_id);
  if (identity.is_null()) Fail("browser_exited", "The browser exited before admission.");
  identity["apiVersion"] = 1; identity["id"] = impl_->id;
  Atomic(impl_->root / ".locks" / ("browser-" + impl_->id + "-" + std::to_string(process_id) + ".json"), identity.dump(2) + "\n");
#else
  Fail("platform_unavailable", "Windows browser ownership uses a kill-on-close job.");
#endif
}

static std::string ExecuteCatalogRequestInternal(std::string_view request_utf8)
{
  try {
    if (request_utf8.size() > 65536) Fail("request_too_large", "The catalog request exceeds its supported size.");
    const auto request = Parse(request_utf8);
    if (!request.is_object() || !request.contains("apiVersion") || !request["apiVersion"].is_number_integer()
        || (request["apiVersion"] != 1 && request["apiVersion"] != 2))
      Fail("api_version", "The catalog request requires integer apiVersion 1 or 2.");
    const auto api_version = request["apiVersion"].get<int>();
    const auto operation = request.at("operation").get<std::string>();
    if (operation == "catalog-location") {
      if (request.contains("root")) Fail("invalid_request", "Catalog location reports the OS-user root; omit root.");
      return Json{{"apiVersion", 1}, {"ok", true}, {"catalogRoot", Utf8(DefaultCatalogRoot())}}.dump();
    }
    if (operation == "installation-status" || operation == "check-game-update"
        || operation == "update-game" || operation == "recover-game-update")
    {
      auto installation_request = request; installation_request["apiVersion"] = 1;
      if (request.contains("installationId")) {
        if (api_version != 2) Fail("api_version", "Installation registration requires typed catalog API version 2.");
        const auto root=CatalogRoot(request.contains("root")?Path(request.at("root").get<std::string>()):DefaultCatalogRoot());
        Lock catalog(root / ".locks" / "catalog.lock",false,true);
        const bool recovery = operation == "installation-status" || operation == "recover-game-update";
        installation_request["gameDirectory"]=SelectedGame(root,request,Json::object(),recovery);
        installation_request["installationPhysicalIdentity"]=RegisteredInstallation(root,
            request.at("installationId").get<std::string>(),true,recovery).metadata.at("physicalIdentity");
      }
      return ExecuteInstallationRequest(installation_request.dump());
    }
    if (operation == "import-sources") {
#if _WIN32
      Json users = Json::array();
      const auto destination = CurrentImportUser();
      const bool elevated = request.value("allowElevation", false);
      if (elevated && request.value("expectedDestinationSid",std::string{}) != destination.sid)
        Fail("destination_changed", "The destination Windows user changed. Check the user list again.");
      const auto discovery = DiscoverImportSources(elevated);
      for (const auto& user : discovery.users)
        users.push_back({{"sid",user.sid},{"name",user.name},{"currentUser",user.current_user}});
      return Json{{"apiVersion",1},{"ok",true},{"users",users},
          {"destinationUser",{{"sid",destination.sid},{"name",destination.name}}},
          {"requiresElevation",discovery.requires_elevation},{"unavailableUsers",discovery.unavailable_users}}.dump();
#else
      Fail("platform_unavailable", "Windows user import is available on Windows. macOS user import is not implemented yet.");
#endif
    }
    const auto root = CatalogRoot(request.contains("root") ? Path(request.at("root").get<std::string>()) : DefaultCatalogRoot());
    if (operation == "prepare-user-import" || operation == "import-user") {
#if _WIN32
      const auto source = ResolveImportUser(request.at("sourceUserSid").get<std::string>());
      const auto destination = CurrentImportUser();
      const auto name = Name(request.at("name").get<std::string>());
      const auto preferred = request.value("preferredInstallationId",std::string{});
      Json choice=request;
      if (!preferred.empty()) {
        if (api_version != 2) Fail("api_version", "Installation registration requires typed catalog API version 2.");
        choice["installationId"]=preferred;
      }
      const auto game = SelectedGame(root,choice,Json::object());
      if (operation == "prepare-user-import") {
        bool elevation = false; std::string reason;
        try { CheckImportAccess(source); }
        catch (const CatalogError& error) {
          if (error.Code() != "elevation_required") throw;
          elevation = true; reason = source.current_user
            ? "Windows needs administrator approval to read this user's protected saved game data."
            : "Windows needs administrator approval to read another Windows user's saved game data.";
        }
        return Json{{"apiVersion",1},{"ok",true},{"importPlan",{
          {"sourceUserSid",source.sid},{"sourceUserName",source.name},
          {"destinationUserSid",destination.sid},{"destinationUserName",destination.name},
          {"name",name},{"gameDirectory",game},{"preferredInstallationId",preferred},
          {"installationRevision",preferred.empty()?std::string{}:RegisteredInstallation(root,preferred,true).revision},
          {"requiresElevation",elevation},{"reason",reason}}}}.dump();
      }
      if (request.value("expectedDestinationSid",std::string{}) != destination.sid)
        Fail("destination_changed", "The destination Windows user changed. Prepare the import again.");
      const auto selected_revision = request.value("expectedInstallationRevision",std::string{});
      if (!preferred.empty() && selected_revision != RegisteredInstallation(root,preferred,true).revision)
        Fail("installation_changed", "The selected installation changed. Prepare the import again.");
      // The elevated helper captures only. Publication and encryption always run
      // under this original destination user, after capture succeeds.
      auto preferences = CaptureImport(source,request.value("allowElevation",false));
      Lock catalog(root / ".locks" / "catalog.lock",false,true);
      const auto id = NewId();
      if (fs::exists(root / "profiles" / id) || fs::exists(root / "archives" / id))
        Fail("duplicate_id", "The generated profile ID already exists. Try importing again.");
      const auto staging = root / "profiles" / (".import-" + id);
      if (!fs::create_directory(staging)) Fail("write_failed", "The new profile could not be staged.");
      try {
        fs::create_directory(staging / "logs");
        ProfilePrefsStore::CreateImported(staging,id,preferences);
        Json metadata{{"schemaVersion",1},{"name",name},{"gameDirectory",game},
          {"preferencesInitialized",true},{"importSourceUserSid",source.sid}};
        if (!preferred.empty()) {
          // Revalidate after capture/elevation before publishing any imported ID.
          if (selected_revision != RegisteredInstallation(root,preferred,true).revision)
            Fail("installation_changed", "The selected installation changed during capture. No profile was published.");
          metadata["preferredInstallationId"]=preferred;
        }
        Atomic(staging / "metadata.json",metadata.dump(2) + "\n");
        fs::rename(staging,root / "profiles" / id);
      } catch (...) {
        // A failed unpublished copy is safe to remove, never the source store.
        std::error_code cleanup;
        fs::remove_all(staging,cleanup);
        if (!cleanup) detail::EraseProtectedPrefsIdentity(id);
        throw;
      }
      return Json{{"apiVersion",1},{"ok",true},{"profile",Load(root,id,false).public_data},
                  {"revision",Discover(root,false,api_version)["revision"]}}.dump();
#else
      Fail("platform_unavailable", "Windows user import is available on Windows. macOS user import is not implemented yet.");
#endif
    }

    if (operation == "launch" || operation == "launch-ordinary") {
      if (api_version == 1) {
        Lock catalog(root / ".locks" / "catalog.lock", false, true);
        TypedAccess(Load(root, request.at("id").get<std::string>(), false).metadata, api_version);
      }
      if (operation == "launch-ordinary" && api_version != 2)
        Fail("api_version", "Ordinary profile launch requires the typed catalog API version 2.");
      return (operation == "launch-ordinary" ? LaunchOrdinary(root, request) : Launch(root, request)).dump();
    }
    Lock catalog(root / ".locks" / "catalog.lock", false, true);
    const bool archived = request.value("archived", false);
    if (operation == "installations" || operation == "register-installation" || operation == "installation-paths") {
      if (api_version != 2) Fail("api_version", "Installation registration requires typed catalog API version 2.");
      if (operation == "installations") return InstallationRegistrations(root).dump();
      if (operation == "register-installation") return RegisterInstallation(root,request).dump();
      return Json{{"apiVersion",2},{"ok",true},{"installation",RegisteredInstallation(root,request.at("installationId").get<std::string>()).projection}}.dump();
    }
    if (operation == "ensure-default" || operation == "resolve-default") {
      if (api_version != 2) Fail("api_version", "Default requires the typed catalog API version 2.");
      if (archived || request.contains("id") || request.contains("name") || request.contains("gameDirectory"))
        Fail("invalid_request", "Resolve or ensure Default without a selector, name, installation or archive state.");
      const auto entry = DefaultEntry(root, operation == "ensure-default");
      return Json{{"apiVersion", 2}, {"ok", true}, {"profile", entry.public_data},
          {"revision", Discover(root, false, 2)["revision"]}}.dump();
    }
    if (operation == "list") return Discover(root, archived, api_version).dump();
    if (operation == "sessions") return Sessions(root).dump();
    if (operation == "create") {
      const auto name = Name(request.at("name").get<std::string>());
      const auto preferred = request.value("preferredInstallationId",std::string{});
      Json choice=request;
      if (!preferred.empty()) {
        if (api_version != 2) Fail("api_version", "Installation registration requires typed catalog API version 2.");
        choice["installationId"]=preferred;
      }
      const auto game = SelectedGame(root,choice,Json::object());
      const auto id = NewId();
      if (fs::exists(root / "profiles" / id) || fs::exists(root / "archives" / id))
        Fail("duplicate_id", "The generated profile ID already exists; create again.");
      const auto staging = root / "profiles" / (".create-" + id);
      fs::create_directory(staging); fs::create_directory(staging / "logs");
      Json metadata{{"schemaVersion",1},{"name",name},{"gameDirectory",game},{"preferencesInitialized",false}};
      if (!preferred.empty()) metadata["preferredInstallationId"]=preferred;
      Atomic(staging / "metadata.json",metadata.dump(2) + "\n");
      fs::rename(staging, root / "profiles" / id);
      return Json{{"apiVersion", 1}, {"ok", true}, {"profile", Load(root, id, false).public_data},
                  {"revision", Discover(root, false, api_version)["revision"]}}.dump();
    }
    const auto id = request.at("id").get<std::string>(); ValidateId(id);
    const bool from_archive = operation == "restore" || archived;
    auto entry = Load(root, id, from_archive);
    TypedAccess(entry.metadata, api_version);
    if (operation == "paths")
      return Json{{"apiVersion", 1}, {"ok", true}, {"profile", entry.public_data}}.dump();
    if (WindowsUser(entry.metadata) && operation != "edit")
      Fail("profile_kind", "Default keeps the existing Windows setup. It cannot be renamed, archived, restored or deleted.");
    if (WindowsUser(entry.metadata) && request.contains("name"))
      Fail("profile_kind", "The built-in Windows setup keeps its Default name.");
    Revision(request, entry, root, from_archive);
    if (operation == "rename" || operation == "edit") {
      if (request.contains("name")) entry.metadata["name"] = Name(request.at("name").get<std::string>());
      if (request.contains("preferredInstallationId")) {
        if (api_version != 2) Fail("api_version", "Preferred installation registration requires typed catalog API version 2.");
        const auto preferred=request.at("preferredInstallationId").get<std::string>();
        if (preferred.empty()) entry.metadata.erase("preferredInstallationId");
        else {
          const auto installation=RegisteredInstallation(root,preferred,true);
          if (request.contains("gameDirectory") && ObserveInstallation(Path(request.at("gameDirectory").get<std::string>())).identity != installation.metadata.at("physicalIdentity").get<std::string>())
            Fail("installation_changed", "The preferred registration and explicit game path do not match.");
          entry.metadata["preferredInstallationId"]=preferred;
          entry.metadata["gameDirectory"]=installation.metadata.at("gameDirectory");
        }
      }
      if (request.contains("gameDirectory") && (!request.contains("preferredInstallationId")
          || request.at("preferredInstallationId").get<std::string>().empty())) {
        // An explicit path edit replaces the previous registration preference.
        entry.metadata.erase("preferredInstallationId");
        entry.metadata["gameDirectory"] = Game(request.at("gameDirectory").get<std::string>());
      }
      Atomic(entry.directory / "metadata.json", entry.metadata.dump(2) + "\n");
      return Json{{"apiVersion", 1}, {"ok", true}, {"profile", Load(root, id, from_archive).public_data},
                  {"revision", Discover(root, from_archive, api_version)["revision"]}}.dump();
    }
    if (operation != "archive" && operation != "restore" && operation != "delete")
      Fail("unknown_operation", "Unknown catalog operation: " + operation);
    Lock writer(root / ".locks" / (id + ".lock"), false);
    Lock data(root / ".locks" / (id + ".data.lock"), false);
    Inactive(root, id);
    if (operation == "delete") {
      if (!archived || !request.value("permanent", false))
        Fail("confirmation_required", "Permanent deletion requires an archived profile and explicit permanent confirmation.");
      if (fs::canonical(entry.directory).parent_path() != root / "archives")
        Fail("invalid_path", "Permanent deletion must remain inside the selected archive directory.");
      for (const auto& item : fs::recursive_directory_iterator(entry.directory))
        Plain(item.path(), item.is_directory());
      fs::remove_all(entry.directory);
      detail::EraseProtectedPrefsIdentity(id);
      return Json{{"apiVersion", 1}, {"ok", true}, {"id", id}, {"deleted", true}}.dump();
    }
    if (operation == "archive" && archived) Fail("invalid_state", "This profile is already archived.");
    const auto destination = root / (operation == "archive" ? "archives" : "profiles") / id;
    if (fs::exists(destination)) Fail("destination_exists", "The destination already contains this profile ID.");
    fs::rename(entry.directory, destination);
    return Json{{"apiVersion", 1}, {"ok", true}, {"profile", Load(root, id, operation == "archive").public_data},
                {"revision", Discover(root, operation == "archive", api_version)["revision"]}}.dump();
  } catch (const CatalogError& error) {
    return Json{{"apiVersion", 1}, {"ok", false}, {"error", {{"code", error.Code()}, {"message", error.what()}}}}.dump();
  } catch (const std::exception& error) {
    return Json{{"apiVersion", 1}, {"ok", false}, {"error", {{"code", "operation_failed"}, {"message", error.what()}}}}.dump();
  }
}
std::string ExecuteCatalogRequest(std::string_view request_utf8)
{
  const auto response = ExecuteCatalogRequestInternal(request_utf8);
  // The exported allocator/entrypoint ABI stays v1; JSON v2 explicitly opts in
  // to storage kinds. Existing isolated JSON v1 clients retain their contract.
  try {
    if (request_utf8.size() <= 65536 && Parse(request_utf8).value("apiVersion", Json{}) == Json(2)) {
      auto typed = Parse(response); typed["apiVersion"] = 2; return typed.dump();
    }
  } catch (...) {}
  return response;
}
} // namespace stfc::profiles
