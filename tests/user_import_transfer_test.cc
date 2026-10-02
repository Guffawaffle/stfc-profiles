// Native entrypoint fixture only: random local pipes and an invalid synthetic SID.
// It never invokes UAC, enumerates accounts, or reads a real PlayerPrefs key.
#include <Windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
constexpr DWORD CaseTimeout = 12000;
constexpr DWORD MaxResponse = 16384;
void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE v) : value(v) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Child {
  Handle process, thread;
  DWORD pid = 0;
  ~Child() {
    if (process.value != INVALID_HANDLE_VALUE && process.value &&
        WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
      // Only the exact child created by this fixture can be terminated.
      TerminateProcess(process.value, 191);
      WaitForSingleObject(process.value, 2000);
    }
  }
};
struct Deadline {
  ULONGLONG end = GetTickCount64() + CaseTimeout;
  DWORD Remaining() const {
    const auto now = GetTickCount64();
    return now < end ? static_cast<DWORD>(end - now) : 0;
  }
};
struct Operation {
  HANDLE pipe;
  Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  OVERLAPPED state{};
  bool pending = false;
  explicit Operation(HANDLE handle) : pipe(handle) {
    Check(event.value != nullptr, "could not create fixture I/O event");
    state.hEvent = event.value;
  }
  ~Operation() {
    if (pending) {
      DWORD transferred = 0;
      CancelIoEx(pipe, &state);
      // Keep OVERLAPPED storage alive until cancellation has completed.
      GetOverlappedResult(pipe, &state, &transferred, TRUE);
    }
  }
  DWORD Finish(const Deadline& deadline, HANDLE process = nullptr) {
    HANDLE waits[]{event.value, process};
    const auto wait = WaitForMultipleObjects(process ? 2 : 1, waits, FALSE, deadline.Remaining());
    Check(wait == WAIT_OBJECT_0, "helper connection or transfer exceeded its deadline or child exited early");
    DWORD transferred = 0;
    const auto ok = GetOverlappedResult(pipe, &state, &transferred, FALSE);
    pending = false;
    Check(ok != FALSE, "helper transfer failed");
    return transferred;
  }
};
std::wstring Nonce() {
  std::array<unsigned char, 16> bytes{};
  Check(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                       BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "could not create fixture nonce");
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring result;
  for (auto byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}
std::wstring TokenSid() {
  Handle token;
  Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE,
        "could not identify fixture token");
  DWORD length = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &length);
  Check(length != 0, "could not size fixture token");
  std::vector<unsigned char> data(length);
  Check(GetTokenInformation(token.value, TokenUser, data.data(), length, &length) != FALSE,
        "could not read fixture token");
  LPWSTR text = nullptr;
  Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text) != FALSE,
        "could not format fixture token SID");
  std::unique_ptr<void, decltype(&LocalFree)> owned(text, LocalFree);
  return text;
}
std::wstring Quote(std::wstring_view argument) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (const auto c : argument) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    result += c;
  }
  result.append(slashes * 2, L'\\');
  result += L'\"';
  return result;
}
void Transfer(HANDLE pipe, void* data, DWORD length, bool write,
              const Deadline& deadline, HANDLE process) {
  auto bytes = static_cast<unsigned char*>(data);
  while (length) {
    Operation operation(pipe);
    DWORD count = 0;
    const auto ok = write ? WriteFile(pipe, bytes, length, &count, &operation.state)
                          : ReadFile(pipe, bytes, length, &count, &operation.state);
    if (!ok) {
      Check(GetLastError() == ERROR_IO_PENDING, "helper closed the fixture transfer unexpectedly");
      operation.pending = true;
      count = operation.Finish(deadline, process);
    }
    Check(count > 0 && count <= length, "helper returned an invalid transfer size");
    bytes += count;
    length -= count;
  }
}
void Send(HANDLE pipe, const Json& value, const Deadline& deadline, HANDLE process) {
  auto bytes = Json::to_cbor(value);
  auto length = static_cast<DWORD>(bytes.size());
  Transfer(pipe, &length, sizeof(length), true, deadline, process);
  Transfer(pipe, bytes.data(), length, true, deadline, process);
}
Json Receive(HANDLE pipe, const Deadline& deadline, HANDLE process) {
  DWORD length = 0;
  Transfer(pipe, &length, sizeof(length), false, deadline, process);
  Check(length && length <= MaxResponse, "helper returned an oversized or empty fixture response");
  std::vector<std::uint8_t> bytes(length);
  Transfer(pipe, bytes.data(), length, false, deadline, process);
  return Json::from_cbor(bytes);
}
DWORD ExitCode(Child& child, const Deadline& deadline) {
  Check(WaitForSingleObject(child.process.value, deadline.Remaining()) == WAIT_OBJECT_0,
        "helper did not exit within its deadline");
  DWORD code = STILL_ACTIVE;
  Check(GetExitCodeProcess(child.process.value, &code) != FALSE && code != STILL_ACTIVE,
        "helper exit code is unavailable");
  return code;
}
void CheckDisconnected(HANDLE pipe, const Deadline& deadline) {
  Operation operation(pipe);
  unsigned char byte = 0;
  DWORD count = 0;
  auto ok = ReadFile(pipe, &byte, 1, &count, &operation.state);
  DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  if (!ok && error == ERROR_IO_PENDING) {
    operation.pending = true;
    Check(WaitForSingleObject(operation.event.value, deadline.Remaining()) == WAIT_OBJECT_0,
          "wrong-parent helper kept its pipe open");
    ok = GetOverlappedResult(pipe, &operation.state, &count, FALSE);
    error = ok ? ERROR_SUCCESS : GetLastError();
    operation.pending = false;
  }
  Check((ok && count == 0) || (!ok && (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
                                     error == ERROR_PIPE_NOT_CONNECTED)),
        "wrong-parent helper emitted data instead of disconnecting before the request");
}
std::wstring SystemRundll32() {
  std::array<wchar_t, 32768> system{};
  const auto count = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
  Check(count && count < system.size(), "native Windows helper path is unavailable");
  return (std::filesystem::path(std::wstring(system.data(), count)) / L"rundll32.exe").native();
}
void RunCase(const std::wstring& cli, const std::wstring& native, bool dll, bool wrong_parent) {
  Deadline deadline;
  const auto nonce = Nonce();
  const auto name = L"\\\\.\\pipe\\STFCProfilesImport-" + nonce;
  const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + TokenSid() + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
          &descriptor, nullptr) != FALSE, "could not protect fixture pipe");
  std::unique_ptr<void, decltype(&LocalFree)> owned(descriptor, LocalFree);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  Handle pipe{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
      FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
      PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security)};
  Check(pipe.value != INVALID_HANDLE_VALUE, "could not create fixture pipe");
  Child child;
  Operation connection(pipe.value);
  bool connected = ConnectNamedPipe(pipe.value, &connection.state) != FALSE;
  auto error = connected ? ERROR_SUCCESS : GetLastError();
  connected = connected || error == ERROR_PIPE_CONNECTED;
  Check(connected || error == ERROR_IO_PENDING, "could not prepare fixture connection");
  connection.pending = !connected;
  // A wrong PID must be rejected before a request is received or accounts are read.
  const auto parent = wrong_parent ? DWORD{1} : GetCurrentProcessId();
  Check(parent != GetCurrentProcessId() || !wrong_parent, "fixture wrong parent PID is not distinct");
  const auto executable = dll ? SystemRundll32() : cli;
  auto command = Quote(executable) + L" " +
      (dll ? Quote(native) + L",STFCProfilesUserImport " : L"--internal-user-import ") +
      nonce + L" " + std::to_wstring(parent);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
        "could not launch compiled helper entrypoint");
  child.process.value = process.hProcess;
  child.thread.value = process.hThread;
  child.pid = process.dwProcessId;
  if (!connected) connection.Finish(deadline, child.process.value);
  ULONG client = 0;
  Check(GetNamedPipeClientProcessId(pipe.value, &client) != FALSE && client == child.pid,
        "fixture connection did not belong to its launched child");
  if (wrong_parent) {
    const auto code = ExitCode(child, deadline);
    Check(code != 0 && (!dll || code == 190), "wrong-parent helper did not reject its invocation");
    CheckDisconnected(pipe.value, deadline);
  } else {
    Send(pipe.value, {{"sourceUserSid", "NOT-A-WINDOWS-SID"}}, deadline, child.process.value);
    const auto response = Receive(pipe.value, deadline, child.process.value);
    Check(response.is_object() && response.contains("ok") && response.at("ok") == false &&
          response.value("code", "") == "source_user_missing" && !response.contains("values"),
          "compiled helper did not return the deterministic invalid-SID error");
    Check(ExitCode(child, deadline) == 0, "compiled helper failed after its error response");
  }
  Check(DisconnectNamedPipe(pipe.value) != FALSE, "could not disconnect completed fixture pipe");
  std::cout << (dll ? "native unsuffixed W callback" : "CLI private entrypoint")
            << (wrong_parent ? ": wrong parent rejected before request\n"
                             : ": invalid synthetic SID returned source_user_missing\n");
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    Check(argc == 3, "usage: user-import-transfer-tests <stfc-profiles.exe> <stfc-profiles-native.dll>");
    const auto cli = std::filesystem::absolute(argv[1]).native();
    const auto native = std::filesystem::absolute(argv[2]).native();
    Handle cli_image{CreateFileW(cli.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
    Handle native_image{CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
    Check(cli_image.value != INVALID_HANDLE_VALUE && native_image.value != INVALID_HANDLE_VALUE,
          "compiled CLI and native DLL inputs must exist and remain fixed during the fixture");
    RunCase(cli, native, false, false);
    RunCase(cli, native, true, false);
    RunCase(cli, native, false, true);
    RunCase(cli, native, true, true);
    std::cout << "user import transfer fixtures PASS (4 compiled entrypoint cases)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "user import transfer fixtures FAIL: " << error.what() << '\n';
    return 1;
  }
}
