// Exercise real loader attach/detach notifications using the product DLL, in a
// non-game host. No profile catalog or game initialization is requested.
#include <Windows.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
constexpr SIZE_T reserve=64*1024;
struct ThreadState { SIZE_T actualReserve=0; };
DWORD WINAPI SmallStackThread(void* opaque)
{
  auto& state=*static_cast<ThreadState*>(opaque);
  char probe=0;
  MEMORY_BASIC_INFORMATION memory{};
  ULONG_PTR low=0,high=0;
  GetCurrentThreadStackLimits(&low,&high);
  if (!VirtualQuery(&probe,&memory,sizeof(memory))) return 1;
  const auto base=reinterpret_cast<ULONG_PTR>(memory.AllocationBase);
  if (high<=base) return 2;
  state.actualReserve=high-base;
  return state.actualReserve<=reserve ? 0 : 3;
}
void Check(bool condition,const char* message)
{
  if (!condition) throw std::runtime_error(message);
}
} // namespace
int wmain(int count,wchar_t** arguments)
{
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  try {
    Check(count==2,"Pass the exact profile runtime DLL path");
    std::vector<wchar_t> executable(32768);
    auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    Check(length>0&&length<executable.size(),"Could not identify the fixture host");
    const auto host=std::filesystem::path(std::wstring(executable.data(),length)).filename().wstring();
    Check(CompareStringOrdinal(host.c_str(),-1,L"prime.exe",-1,TRUE)!=CSTR_EQUAL,"The fixture must not be a game host");
    const auto path=std::filesystem::canonical(arguments[1]);
    auto module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Check(module!=nullptr,"The product runtime could not be loaded");
    struct Unload { HMODULE module; ~Unload(){FreeLibrary(module);} } unload{module};
    length=GetModuleFileNameW(module,executable.data(),static_cast<DWORD>(executable.size()));
    Check(length>0&&length<executable.size(),"Could not identify the loaded runtime");
    Check(std::filesystem::equivalent(path,std::filesystem::path(std::wstring(executable.data(),length))),"The loader selected another DLL");
    const auto contract=reinterpret_cast<const unsigned int*>(GetProcAddress(module,"STFCProfilesExplicitLaunchContractV1"));
    Check(contract&&*contract==1,"The loaded DLL lacks the concrete profile launch contract");
    SIZE_T actualReserve=0;
    constexpr unsigned iterations=64;
    for (unsigned i=0;i<iterations;++i) {
      ThreadState state;
      auto thread=CreateThread(nullptr,reserve,SmallStackThread,&state,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
      Check(thread!=nullptr,"Could not create a small-stack thread");
      const auto waited=WaitForSingleObject(thread,10000);
      if (waited!=WAIT_OBJECT_0) {
        // Do not unwind the DLL or ThreadState while a worker may still use them.
        std::cerr<<"A small-stack thread did not finish\n"<<std::flush;
        TerminateProcess(GetCurrentProcess(),1);
        std::abort();
      }
      DWORD exitCode=1;
      const auto exited=GetExitCodeThread(thread,&exitCode);
      CloseHandle(thread);
      Check(exited&&exitCode==0,"A small-stack loader/thread notification failed");
      actualReserve=state.actualReserve;
    }
    std::cout<<"Product loader notifications passed: "<<iterations<<" threads; reserved stack "<<actualReserve<<" bytes\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
  }
}
