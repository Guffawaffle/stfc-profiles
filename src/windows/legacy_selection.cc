// Extracted from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#if _WIN32

#include "stfc_profiles/windows/legacy_selection.h"
#include "stfc_profiles/legacy_contract.h"

#include <ShlObj.h>
#include <Windows.h>
#include <shellapi.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stfc::profiles::windows {

namespace
{

HANDLE install_lock = INVALID_HANDLE_VALUE;

struct Resolution {
  ProfileSelection      selection;
  std::filesystem::path receipt_path;
  std::filesystem::path pending_path;
  std::string           receipt_content;
};

std::filesystem::path ExecutablePath()
{
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length)
      AbortProfileLaunch("Could not locate game executable");
    if (length < buffer.size())
      return std::filesystem::path(std::wstring(buffer.data(), length));
    if (buffer.size() >= 32768)
      AbortProfileLaunch("Game executable path is too long");
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path LocalAppData()
{
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) || !raw) {
    CoTaskMemFree(raw);
    AbortProfileLaunch("Local application data is unavailable");
  }
  std::filesystem::path result(raw);
  CoTaskMemFree(raw);
  return result;
}

std::wstring LowerInvariant(std::wstring_view value)
{
  const int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr, 0);
  if (!length)
    AbortProfileLaunch("Could not normalize game path");
  std::wstring result(length, L'\0');
  if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), result.data(),
                    length, nullptr, nullptr, 0)
      != length)
    AbortProfileLaunch("Could not normalize game path");
  return result;
}

std::string Utf8(std::wstring_view value)
{
  if (value.empty())
    return {};
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (!length)
    AbortProfileLaunch("Could not encode game path");
  std::string result(length, '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                          length, nullptr, nullptr)
      != length)
    AbortProfileLaunch("Could not encode game path");
  return result;
}

std::optional<std::string> ReadSmallFile(const std::filesystem::path& path, DWORD maximum)
{
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      return std::nullopt;
    AbortProfileLaunch("Could not read profile marker or enrollment");
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > maximum) {
    CloseHandle(file);
    AbortProfileLaunch("Profile marker or enrollment is too large");
  }
  std::string result(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD       read = 0;
  const bool  ok   = result.empty() || ReadFile(file, result.data(), static_cast<DWORD>(result.size()), &read, nullptr);
  const bool  closed = CloseHandle(file) != 0;
  if (!ok || read != result.size() || !closed)
    AbortProfileLaunch("Could not finish reading profile marker or enrollment");
  return result;
}

void WriteEnrollment(const std::filesystem::path& path, const std::string& content)
{
  std::filesystem::create_directories(path.parent_path());
  const auto   temporary = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"."
                           + std::to_wstring(GetTickCount64()) + L".tmp";
  const HANDLE file =
      CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    AbortProfileLaunch("Could not create profile enrollment temporary file");
  DWORD      written = 0;
  const bool ok      = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr)
                       && written == content.size() && FlushFileBuffers(file);
  const bool closed  = CloseHandle(file) != 0;
  if (!ok || !closed) {
    DeleteFileW(temporary.c_str());
    AbortProfileLaunch("Could not write profile enrollment");
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
    const DWORD error = GetLastError();
    DeleteFileW(temporary.c_str());
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
      AbortProfileLaunch("Could not finalize profile enrollment");
  }
  const auto saved = ReadSmallFile(path, 65536);
  if (!saved || *saved != content)
    AbortProfileLaunch("Profile enrollment does not match this install");
}

void CheckCommandLine(const std::filesystem::path& expected)
{
  int     argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv)
    AbortProfileLaunch("Could not inspect profile launch arguments");
  bool seen = false;
  for (int i = 1; i < argc; ++i) {
    if (std::wstring_view(argv[i]) != L"-ccm")
      continue;
    if (seen || ++i >= argc) {
      LocalFree(argv);
      AbortProfileLaunch("Invalid -ccm with a marked profile");
    }
    seen              = true;
    const auto actual = std::filesystem::weakly_canonical(std::filesystem::path(argv[i]));
    const auto wanted = std::filesystem::weakly_canonical(expected);
    if (CompareStringOrdinal(actual.c_str(), -1, wanted.c_str(), -1, TRUE) != CSTR_EQUAL) {
      LocalFree(argv);
      AbortProfileLaunch("-ccm conflicts with the marked profile");
    }
  }
  LocalFree(argv);
}

bool ExistingBindingForId(const std::filesystem::path& directory, std::string_view id,
                          const std::filesystem::path& ignore = {},
                          const std::filesystem::path& also_ignore = {})
{
  if (!std::filesystem::exists(directory))
    return false;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path() == ignore || entry.path() == also_ignore)
      continue;
    if (entry.path().extension() != L".binding" && entry.path().extension() != L".pending")
      continue;
    const auto receipt = ReadSmallFile(entry.path(), 65536);
    if (receipt && stfc::profiles::legacy::ReceiptClaimsId(*receipt, id))
      return true;
  }
  return false;
}

Resolution Select()
{
  const auto game_dir = std::filesystem::canonical(ExecutablePath()).parent_path();
  const auto path_id  = Utf8(LowerInvariant(game_dir.wstring()));

  std::array<char, 17> hash{};
  std::snprintf(hash.data(), hash.size(), "%016llx",
                static_cast<unsigned long long>(stfc::profiles::legacy::PathHash(path_id)));
  const auto receipt_path =
      LocalAppData() / L"STFC Community Mod" / L"ProfileBindingsV2" / (std::string(hash.data()) + ".binding");
  auto pending_path = receipt_path;
  pending_path += L".pending";
  auto marker = ReadSmallFile(game_dir / L"stfc_community_mod.profile", 64);
  auto receipt = ReadSmallFile(receipt_path, 65536);
  auto pending = ReadSmallFile(pending_path, 65536);
  if (!marker && !receipt && !pending)
    return {};

  // An ordinary unmarked install keeps its existing launch behavior. Marked
  // installs re-read all selection state under this lifetime lock.
  std::filesystem::create_directories(receipt_path.parent_path());
  auto lock_path = receipt_path;
  lock_path += L".lock";
  install_lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (install_lock == INVALID_HANDLE_VALUE)
    AbortProfileLaunch("Another game instance already uses this install");

  marker = ReadSmallFile(game_dir / L"stfc_community_mod.profile", 64);
  receipt = ReadSmallFile(receipt_path, 65536);
  pending = ReadSmallFile(pending_path, 65536);
  const auto decision = stfc::profiles::legacy::Decide(marker ? std::optional<std::string_view>(*marker) : std::nullopt,
                                                 receipt ? std::optional<std::string_view>(*receipt) : std::nullopt,
                                                 pending ? std::optional<std::string_view>(*pending) : std::nullopt,
                                                 path_id);

  using stfc::profiles::legacy::SelectionState;
  switch (decision.state) {
    case SelectionState::Default:
      CloseHandle(install_lock);
      install_lock = INVALID_HANDLE_VALUE;
      return {};
    case SelectionState::MissingMarker:
      AbortProfileLaunch("Enrolled game install is missing its profile marker");
    case SelectionState::InvalidMarker:
      AbortProfileLaunch("Invalid profile marker");
    case SelectionState::ReceiptConflict:
      AbortProfileLaunch("Profile enrollment conflicts with marker or install path");
    case SelectionState::Enroll:
    case SelectionState::Resume:
    case SelectionState::Bound:
      break;
  }

  if (decision.state == SelectionState::Enroll && ExistingBindingForId(receipt_path.parent_path(), decision.id))
    AbortProfileLaunch("Profile ID is already enrolled to another install");

  const auto id          = std::wstring(decision.id.begin(), decision.id.end());
  const auto config_path = game_dir / L"stfc-mod" / id / (id + L".toml");
  CheckCommandLine(config_path);
  // File::Init uses the corresponding log path before hook installation.
  std::filesystem::create_directories(config_path.parent_path());
  const bool enroll = decision.state == SelectionState::Enroll || decision.state == SelectionState::Resume;
  const bool resume = decision.state == SelectionState::Resume;
  return {{true, enroll, resume, id, config_path},
          enroll ? receipt_path : std::filesystem::path{},
          enroll ? pending_path : std::filesystem::path{},
          enroll ? stfc::profiles::legacy::Receipt(decision.id, path_id) : std::string{}};
}

const Resolution& Resolved()
{
  static const Resolution result = [] {
    try {
      return Select();
    } catch (...) {
      AbortProfileLaunch("Could not resolve game profile");
    }
  }();
  return result;
}

} // namespace

[[noreturn]] void AbortProfileLaunch(const char* reason)
{
  OutputDebugStringA("[STFC profile bootstrap] ");
  OutputDebugStringA(reason);
  OutputDebugStringA("\n");
  try {
    wchar_t     temporary_dir[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporary_dir);
    if (length > 0 && length < MAX_PATH) {
      const auto   path = std::wstring(temporary_dir, length) + L"stfc-mod-profile-bootstrap.log";
      const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        const auto line    = std::to_string(GetCurrentProcessId()) + ": " + reason + "\r\n";
        DWORD      written = 0;
        WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(file);
      }
    }
  } catch (...) {
  }
  TerminateProcess(GetCurrentProcess(), 190);
  std::abort();
}

const ProfileSelection& ResolveProfileSelection()
{ return Resolved().selection; }

void StartProfileEnrollment()
{
  const auto& result = Resolved();
  if (!result.selection.enroll)
    return;
  try {
    if (ExistingBindingForId(result.receipt_path.parent_path(),
                             Utf8(result.selection.id),
                             result.pending_path, result.receipt_path))
      AbortProfileLaunch("Profile ID is already enrolled to another install");
    WriteEnrollment(result.pending_path, result.receipt_content);
  } catch (...) {
    AbortProfileLaunch("Could not start game profile enrollment");
  }
}

void CompleteProfileEnrollment()
{
  const auto& result = Resolved();
  if (!result.selection.marked)
    return;
  static const bool completed = [&] {
    try {
      if (result.selection.enroll)
        WriteEnrollment(result.receipt_path, result.receipt_content);
      if (result.selection.enroll)
        DeleteFileW(result.pending_path.c_str());
      return true;
    } catch (...) {
      AbortProfileLaunch("Could not complete game profile enrollment");
    }
  }();
  (void)completed;
}

} // namespace stfc::profiles::windows

#endif
