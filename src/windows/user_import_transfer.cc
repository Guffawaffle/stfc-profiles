#include "stfc_profiles/user_import.h"
#include "stfc_profiles/catalog.h"
#include "../prefs_crypto.h"
#include <Windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <charconv>
#include <sstream>
#include <memory>

namespace stfc::profiles {
namespace {
using Json = nlohmann::json;
constexpr DWORD MaxTransfer = 64 * 1024 * 1024;
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
[[noreturn]] void Fail(const char* code, const char* message) { throw CatalogError(code, message); }
std::wstring PipeName(std::wstring_view nonce) {
  if (nonce.size() != 32 || nonce.find_first_not_of(L"0123456789abcdef") != nonce.npos)
    Fail("import_transfer", "The import transfer identifier is invalid.");
  return L"\\\\.\\pipe\\STFCProfilesImport-" + std::wstring(nonce);
}
void Transfer(HANDLE pipe, void* data, DWORD length, bool write, bool overlapped) {
  auto bytes = static_cast<unsigned char*>(data);
  while (length) {
    DWORD count = 0;
    Handle event; OVERLAPPED operation{};
    if (overlapped) {
      event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!event.value) Fail("import_transfer", "Windows could not prepare the private import transfer.");
      operation.hEvent = event.value;
    }
    const auto ok = write ? WriteFile(pipe, bytes, length, &count, overlapped ? &operation : nullptr)
                          : ReadFile(pipe, bytes, length, &count, overlapped ? &operation : nullptr);
    if (!ok) {
      if (!overlapped || GetLastError() != ERROR_IO_PENDING)
        Fail("import_transfer", "The private import transfer was interrupted. No profile was created.");
      if (WaitForSingleObject(event.value, 180000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &count, TRUE);
        Fail("import_transfer", "The import transfer timed out. Try importing again.");
      }
      if (!GetOverlappedResult(pipe, &operation, &count, FALSE))
        Fail("import_transfer", "The private import transfer was interrupted. No profile was created.");
    }
    if (!count) Fail("import_transfer", "The private import transfer ended unexpectedly.");
    bytes += count; length -= count;
  }
}
void Send(HANDLE pipe, const Json& value, bool overlapped) {
  auto bytes = Json::to_cbor(value);
  struct Wipe { std::vector<std::uint8_t>& bytes; ~Wipe() { detail::WipePrefsBytes(bytes); } } wipe{bytes};
  if (bytes.size() > MaxTransfer) Fail("import_transfer", "The saved game data exceeds the import limit.");
  auto length = static_cast<DWORD>(bytes.size());
  Transfer(pipe, &length, sizeof(length), true, overlapped);
  Transfer(pipe, bytes.data(), length, true, overlapped);
}
Json Receive(HANDLE pipe, bool overlapped) {
  DWORD length = 0; Transfer(pipe, &length, sizeof(length), false, overlapped);
  if (!length || length > MaxTransfer) Fail("import_transfer", "The import transfer size is invalid.");
  std::vector<std::uint8_t> bytes(length);
  struct Wipe { std::vector<std::uint8_t>& bytes; ~Wipe() { detail::WipePrefsBytes(bytes); } } wipe{bytes};
  Transfer(pipe, bytes.data(), length, false, overlapped);
  try { return Json::from_cbor(bytes); }
  catch (...) { Fail("import_transfer", "The private import transfer could not be read."); }
}
Json Encode(const std::vector<NativePreference>& preferences) {
  Json values = Json::array();
  for (const auto& value : preferences) {
    // UTF-16 code units remain byte-exact across the private pipe.
    std::vector<std::uint16_t> key(value.key.begin(), value.key.end());
    values.push_back({{"key", key}, {"type", value.type}, {"bytes", Json::binary(value.bytes)}});
  }
  return {{"ok", true}, {"values", values}};
}
std::vector<NativePreference> Decode(const Json& response) {
  if (!response.value("ok", false))
    throw CatalogError(response.value("code", "import_failed"), response.value("message", "The saved game data could not be read."));
  const auto& values = response.at("values");
  if (!values.is_array() || values.empty() || values.size() > 10000)
    Fail("import_transfer", "The imported preferences have an invalid entry count.");
  std::vector<NativePreference> result;
  std::size_t total = 0;
  for (const auto& entry : values) {
    const auto key = entry.at("key").get<std::vector<std::uint16_t>>();
    const auto& binary = entry.at("bytes").get_binary();
    total += key.size() * 2 + binary.size();
    if (total > detail::MaxPlainPrefsBytes || key.size() > 32768)
      Fail("import_transfer", "The imported preferences exceed the supported bounds.");
    result.push_back({std::u16string(key.begin(), key.end()), entry.at("type").get<std::uint32_t>(),
                      {binary.begin(), binary.end()}});
  }
  return result;
}
std::wstring RandomNonce() {
  std::array<unsigned char,16> bytes{};
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    Fail("import_transfer", "Windows could not prepare a private import transfer.");
  constexpr wchar_t hex[] = L"0123456789abcdef"; std::wstring result;
  for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
Json ElevatedRequest(const Json& request) {
  const auto nonce = RandomNonce();
  const auto name = PipeName(nonce);
  const auto sid = CurrentUserSid();
  const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid.begin(), sid.end()) + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    Fail("import_transfer", "Windows could not protect the private import transfer.");
  std::unique_ptr<void, decltype(&LocalFree)> owned(descriptor, LocalFree);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  Handle pipe{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security)};
  if (pipe.value == INVALID_HANDLE_VALUE) Fail("import_transfer", "Windows could not open the private import transfer.");
  Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event.value) Fail("import_transfer", "Windows could not prepare the private import connection.");
  OVERLAPPED connection{}; connection.hEvent = event.value;
  bool connected = ConnectNamedPipe(pipe.value, &connection) != FALSE;
  const auto connection_error = connected ? ERROR_SUCCESS : GetLastError();
  connected = connected || connection_error == ERROR_PIPE_CONNECTED;
  if (!connected && connection_error != ERROR_IO_PENDING)
    Fail("import_transfer", "Windows could not prepare the private import connection.");
  struct PendingConnection {
    HANDLE pipe; OVERLAPPED& operation; bool& complete;
    ~PendingConnection() {
      if (!complete) { DWORD count = 0; CancelIoEx(pipe, &operation);
                       GetOverlappedResult(pipe, &operation, &count, TRUE); }
    }
  } pending{pipe.value, connection, connected};
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&CaptureImport), &module))
    Fail("import_helper", "The import helper could not be identified.");
  std::array<wchar_t,32768> path{};
  auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (!count || count >= path.size()) Fail("import_helper", "The import helper path is unavailable.");
  std::wstring executable(path.data(), count), parameters;
  // Keep the exact loaded module from being replaced while Windows starts it.
  Handle image{CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
  if (image.value == INVALID_HANDLE_VALUE) Fail("import_helper", "The import helper could not be held for launch.");
  const auto suffix = nonce + L" " + std::to_wstring(GetCurrentProcessId());
  if (module == GetModuleHandleW(nullptr)) parameters = L"--internal-user-import " + suffix;
  else {
    parameters = L"\"" + executable + L"\",STFCProfilesUserImport " + suffix;
    std::array<wchar_t,MAX_PATH+1> system{};
    count = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (!count || count >= system.size()) Fail("import_helper", "The Windows helper location is unavailable.");
    executable = std::filesystem::path(system.data()).append(L"rundll32.exe").native();
  }
  SHELLEXECUTEINFOW launch{sizeof(launch)};
  launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  launch.lpVerb = L"runas"; launch.lpFile = executable.c_str(); launch.lpParameters = parameters.c_str(); launch.nShow = SW_HIDE;
  if (!ShellExecuteExW(&launch)) {
    if (GetLastError() == ERROR_CANCELLED) Fail("import_cancelled", "Import was cancelled. Your selection is kept, and no profile was created.");
    Fail("import_permission", "Windows could not approve this import. You can try again or choose another user.");
  }
  Handle process{launch.hProcess};
  if (!process.value || process.value == INVALID_HANDLE_VALUE)
    Fail("import_helper", "Windows did not return the approved import helper.");
  const auto helper_pid = GetProcessId(process.value);
  DWORD transferred = 0;
  if (!connected) {
    HANDLE waits[]{event.value, process.value};
    const auto wait = WaitForMultipleObjects(2, waits, FALSE, 180000);
    if (wait != WAIT_OBJECT_0 || !GetOverlappedResult(pipe.value, &connection, &transferred, FALSE)) {
      CancelIoEx(pipe.value, &connection);
      GetOverlappedResult(pipe.value, &connection, &transferred, TRUE);
      Fail("import_helper", "The approved helper could not connect. No profile was created.");
    }
  }
  connected = true;
  ULONG client = 0;
  if (!GetNamedPipeClientProcessId(pipe.value, &client) || client != helper_pid)
    Fail("import_transfer", "The import connection did not belong to the approved helper.");
  Send(pipe.value, request, true);
  auto result = Receive(pipe.value, true);
  DisconnectNamedPipe(pipe.value);
  return result;
}
}
std::vector<NativePreference> CaptureImport(const ImportUser& user, bool allow_elevation) {
  try { return CaptureUserPreferences(user); }
  catch (const CatalogError& error) {
    if (error.Code() != "elevation_required") throw;
    if (!allow_elevation) throw;
    return Decode(ElevatedRequest({{"operation","capture-user"},{"sourceUserSid",user.sid}}));
  }
}
ImportUserDiscovery DiscoverImportSources(bool allow_elevation) {
  const auto sid = CurrentUserSid();
  auto result = DiscoverImportUsers(sid);
  if (!result.requires_elevation || !allow_elevation) return result;
  const auto response = ElevatedRequest({{"operation","discover-users"},{"destinationUserSid",sid}});
  if (!response.value("ok",false))
    throw CatalogError(response.value("code","import_failed"),response.value("message","Windows user discovery could not finish."));
  const auto& users = response.at("users");
  if (response.contains("values") || !users.is_array() || users.size() > 10000) Fail("import_transfer", "The user discovery response is invalid.");
  result.users.clear();
  for (const auto& entry : users) {
    const auto source = entry.at("sid").get<std::string>();
    ValidateImportUserSid(source);
    const auto name = entry.at("name").get<std::string>();
    if (name.empty() || name.size() > 32768 || std::any_of(result.users.begin(),result.users.end(),
        [&](const auto& user) { return user.sid == source; }))
      Fail("import_transfer", "The user discovery response is invalid.");
    result.users.push_back({source,name,{},source == sid});
  }
  result.requires_elevation = response.at("requiresElevation").get<bool>();
  result.unavailable_users = response.at("unavailableUsers").get<std::size_t>();
  if (result.unavailable_users > 10000) Fail("import_transfer", "The user discovery response is invalid.");
  return result;
}
void RunUserImportHelper(std::wstring_view command_line) {
  std::wstringstream input{std::wstring(command_line)};
  std::wstring nonce, extra; DWORD parent = 0;
  if (!(input >> nonce >> parent) || !parent || (input >> extra))
    Fail("import_helper", "The private import helper arguments are invalid.");
  const auto name = PipeName(nonce);
  if (!WaitNamedPipeW(name.c_str(), 30000)) Fail("import_helper", "The requesting Profiles app is no longer available.");
  Handle pipe{CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
  if (pipe.value == INVALID_HANDLE_VALUE) Fail("import_helper", "The private import connection is unavailable.");
  ULONG server = 0;
  if (!GetNamedPipeServerProcessId(pipe.value, &server) || server != parent)
    Fail("import_helper", "The private connection belongs to a different app.");
  const auto request = Receive(pipe.value, true);
  try {
    const auto operation = request.at("operation").get<std::string>();
    if (operation == "discover-users") {
      const auto result = DiscoverImportUsers(request.at("destinationUserSid").get<std::string>());
      Json users = Json::array();
      for (const auto& user : result.users)
        users.push_back({{"sid",user.sid},{"name",user.name},{"currentUser",user.current_user}});
      Send(pipe.value, {{"ok",true},{"users",users},{"requiresElevation",result.requires_elevation},
                       {"unavailableUsers",result.unavailable_users}}, true);
    } else if (operation == "capture-user") {
      auto values = CaptureUserPreferences(ResolveImportUser(request.at("sourceUserSid").get<std::string>()));
      Send(pipe.value, Encode(values), true);
    } else Fail("import_helper", "The private import operation is invalid.");
  } catch (const CatalogError& error) {
    Send(pipe.value, {{"ok", false}, {"code", error.Code()}, {"message", error.what()}}, true);
  } catch (...) {
    Send(pipe.value, {{"ok", false}, {"code", "import_failed"},
         {"message", "The saved game data could not be read. No profile was created."}}, true);
  }
}
}
