#include "stfc_profiles/community_mod_adapter.h"
#include "stfc_profiles/catalog.h"
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if _WIN32
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#else
#include <dlfcn.h>
#include <poll.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace stfc::profiles::community_mod {
namespace {
bool SafeUrl(std::u16string_view url)
{
  return url.starts_with(u"https://") && url.size() <= 16384
      && url.find_first_of(u"\"\r\n\t ") == std::u16string_view::npos
      && url.find(u'\0') == std::u16string_view::npos;
}
#if _WIN32
struct Handle {
  HANDLE value = nullptr;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
std::wstring Quote(std::wstring_view value)
{
  std::wstring result=L"\"";
  std::size_t backslashes=0;
  for (wchar_t ch:value) {
    if (ch==L'\\') { ++backslashes; continue; }
    if (ch==L'\"') result.append(backslashes*2+1,L'\\');
    else result.append(backslashes,L'\\');
    backslashes=0; result.push_back(ch);
  }
  result.append(backslashes*2,L'\\'); result.push_back(L'\"');
  return result;
}
std::wstring Folder(REFKNOWNFOLDERID id)
{
  PWSTR value=nullptr;
  if (FAILED(SHGetKnownFolderPath(id,0,nullptr,&value)) || !value) {
    CoTaskMemFree(value); return {};
  }
  std::wstring result(value); CoTaskMemFree(value); return result;
}
std::filesystem::path Edge()
{
  for (auto key:{HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER}) {
    DWORD size=0;
    const auto status=RegGetValueW(key,L"SOFTWARE\\Policies\\Microsoft\\Edge",L"UserDataDir",
                                   RRF_RT_ANY,nullptr,nullptr,&size);
    if (status!=ERROR_FILE_NOT_FOUND && status!=ERROR_PATH_NOT_FOUND)
      throw std::runtime_error("Edge policy overrides isolated browser storage");
  }
  for (const auto* id:{&FOLDERID_ProgramFilesX86,&FOLDERID_ProgramFiles,&FOLDERID_LocalAppData}) {
    auto folder=Folder(*id);
    if (folder.empty()) continue;
    auto file=std::filesystem::path(folder)/L"Microsoft"/L"Edge"/L"Application"/L"msedge.exe";
    if (std::filesystem::is_regular_file(file)) return file;
  }
  throw std::runtime_error("Microsoft Edge is unavailable for isolated sign-in");
}
void BrowserGuardian(std::wstring_view arguments)
{
  int count=0;
  const auto command=L"guardian "+std::wstring(arguments);
  LPWSTR* parts=CommandLineToArgvW(command.c_str(),&count);
  if (!parts) throw std::runtime_error("invalid browser guardian arguments");
  struct Free { LPWSTR* p; ~Free(){ LocalFree(p); } } free{parts};
  if (count!=9 || std::wstring_view(parts[1])!=L"--root" || std::wstring_view(parts[3])!=L"--profile"
      || std::wstring_view(parts[5])!=L"--ready-event" || std::wstring_view(parts[7])!=L"--url")
    throw std::runtime_error("invalid browser guardian request");
  std::string id;
  for (wchar_t ch:std::wstring_view(parts[4])) { if (ch>127) throw std::runtime_error("invalid profile ID"); id+=char(ch); }
  const std::u16string_view url(reinterpret_cast<const char16_t*>(parts[8]),wcslen(parts[8]));
  if (!SafeUrl(url) || !std::wstring_view(parts[6]).starts_with(L"Local\\STFCProfilesBrowser-"))
    throw std::runtime_error("invalid browser guardian handoff");
  BrowserLease lease(std::filesystem::path(parts[2]),id);
  const auto directory=lease.Directory()/L"browser";
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory)))
    throw std::runtime_error("isolated browser directory is a link");
  std::filesystem::create_directory(directory);
  const auto attributes=GetFileAttributesW(directory.c_str());
  if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("isolated browser directory is unavailable or redirected");
  const auto browser=Edge();
  Handle job(CreateJobObjectW(nullptr,nullptr));
  Handle completion(CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  JOBOBJECT_ASSOCIATE_COMPLETION_PORT port{job.value,completion.value};
  if (!job.value || !completion.value
      || !SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))
      || !SetInformationJobObject(job.value,JobObjectAssociateCompletionPortInformation,&port,sizeof(port)))
    throw std::runtime_error("could not protect isolated browser lifetime");
  auto browser_command=Quote(browser.wstring())+L" --user-data-dir="+Quote(directory.wstring())
                      +L" --no-first-run --new-window "+Quote(parts[8]);
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(browser.c_str(),browser_command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,
                       nullptr,nullptr,&startup,&process))
    throw std::runtime_error("could not start isolated sign-in browser");
  Handle browser_process(process.hProcess),browser_thread(process.hThread);
  if (!AssignProcessToJobObject(job.value,process.hProcess)) {
    TerminateProcess(process.hProcess,190); WaitForSingleObject(process.hProcess,10000);
    throw std::runtime_error("could not retain isolated browser process tree");
  }
  if (ResumeThread(process.hThread)==DWORD(-1))
    throw std::runtime_error("could not resume isolated browser");
  // A real guardian owns both the stable browser lease and the protected child
  // tree before acknowledging the handoff. No metadata claims a logged-in user.
  Handle ready(OpenEventW(EVENT_MODIFY_STATE,FALSE,parts[6]));
  if (!ready.value || !SetEvent(ready.value))
    throw std::runtime_error("isolated browser handoff acknowledgment failed");
  while (true) {
    DWORD message=0; ULONG_PTR key=0; LPOVERLAPPED overlapped=nullptr;
    if (!GetQueuedCompletionStatus(completion.value,&message,&key,&overlapped,INFINITE))
      throw std::runtime_error("isolated browser lifetime observation failed");
    if (message==JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) break;
  }
  // Child processes have exited before releasing BrowserLease. A second URL
  // may route to the original guardian's browser; its lease still blocks moves.
}
#endif
} // namespace

bool LaunchIsolatedBrowser(const std::filesystem::path& root, std::string_view id,
                           std::u16string_view url)
{
  if (!SafeUrl(url)) return false;
#if _WIN32
  HMODULE module=nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
       reinterpret_cast<LPCWSTR>(&LaunchIsolatedBrowser),&module)) return false;
  std::vector<wchar_t> path(32768),system(32768);
  const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
  const auto system_length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
  if (!length || length==path.size() || !system_length || system_length>=system.size()) return false;
  const auto runner=std::filesystem::path(std::wstring(system.data(),system_length))/L"rundll32.exe";
  GUID guid{}; wchar_t unique[40]{};
  if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid,unique,40)) return false;
  const auto event_name=L"Local\\STFCProfilesBrowser-"+std::wstring(unique);
  Handle ready(CreateEventW(nullptr,TRUE,FALSE,event_name.c_str()));
  if (!ready.value || GetLastError()==ERROR_ALREADY_EXISTS) return false;
  std::wstring wide_id(id.begin(),id.end());
  std::wstring address(reinterpret_cast<const wchar_t*>(url.data()),url.size());
  auto command=Quote(runner.wstring())+L" "+Quote(std::wstring(path.data(),length))
               +L",STFCProfilesBrowserLaunchW --root "+Quote(root.wstring())+L" --profile "+Quote(wide_id)
               +L" --ready-event "+Quote(event_name)+L" --url "+Quote(address);
  STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
  if (!CreateProcessW(runner.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                       nullptr,nullptr,&startup,&process)) return false;
  Handle guardian(process.hProcess),thread(process.hThread);
  HANDLE events[]{ready.value,guardian.value};
  return WaitForMultipleObjects(2,events,FALSE,15000)==WAIT_OBJECT_0;
#else
  // The macOS helper lives alongside either runtime product. It is an explicit
  // package dependency; absent helper fails instead of using a shared browser.
  Dl_info image{};
  if (!dladdr(reinterpret_cast<const void*>(&LaunchIsolatedBrowser),&image) || !image.dli_fname) return false;
  const auto helper=std::filesystem::path(image.dli_fname).parent_path()/"stfc-profiles";
  if (!std::filesystem::is_regular_file(helper)) return false;
  std::string address;
  for (char16_t ch:url) {
    if (ch>127) return false; // OIDC URLs must already use percent-encoded ASCII.
    address.push_back(static_cast<char>(ch));
  }
  int signal_pipe[2];
  if (pipe(signal_pipe)!=0) return false;
  fcntl(signal_pipe[0],F_SETFD,FD_CLOEXEC);
  const auto root_text=root.string(),id_text=std::string(id),descriptor=std::to_string(signal_pipe[1]);
  std::vector<std::string> arguments{helper.string(),"--internal-browser","--root",root_text,"--profile",id_text,
                                      "--ready-fd",descriptor,"--url",address};
  std::vector<char*> argv;
  for (auto& arg:arguments) argv.push_back(arg.data());
  argv.push_back(nullptr);
  pid_t child=0;
  const auto status=posix_spawn(&child,helper.c_str(),nullptr,nullptr,argv.data(),environ);
  close(signal_pipe[1]);
  if (status!=0) { close(signal_pipe[0]); return false; }
  struct pollfd descriptor_poll{signal_pipe[0],POLLIN,0};
  const bool ready=poll(&descriptor_poll,1,15000)>0;
  char response=0;
  const bool accepted=ready && read(signal_pipe[0],&response,1)==1 && response=='R';
  close(signal_pipe[0]);
  return accepted;
#endif
}
} // namespace stfc::profiles::community_mod

#if _WIN32
extern "C" __declspec(dllexport) void CALLBACK STFCProfilesBrowserLaunchW(HWND,HINSTANCE,LPWSTR command,int)
{
  try { stfc::profiles::community_mod::BrowserGuardian(command ? command : L""); }
  catch (...) { ExitProcess(190); }
}
#endif
