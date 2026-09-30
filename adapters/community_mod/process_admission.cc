#include "stfc_profiles/community_mod_adapter.h"

#if _WIN32
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace stfc::profiles::community_mod {
namespace {

using CreateMutexAFunction = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
CreateMutexAFunction original_create_mutex = nullptr;
std::string admitted_profile_id;

HANDLE WINAPI ProfileCreateMutexA(LPSECURITY_ATTRIBUTES attributes, BOOL initial_owner, LPCSTR name)
{
  if (!name || std::string_view(name).find("-SingleInstanceMutex-") == std::string_view::npos)
    return original_create_mutex(attributes, initial_owner, name);
  const auto previous_error = GetLastError();
  try {
    // Only UnityPlayer's imported single-instance mutex call is redirected.
    // The account writer lease remains the independent lifetime admission gate.
    const std::string scoped = std::string(name) + "-STFCProfiles-" + admitted_profile_id;
    SetLastError(previous_error);
    return original_create_mutex(attributes, initial_owner, scoped.c_str());
  } catch (...) {
    // Never continue with Unity's installation-wide mutex after a named request.
    TerminateProcess(GetCurrentProcess(), 190);
    std::abort();
  }
}

} // namespace

void InstallProcessAdmission(std::string_view id)
{
  if (original_create_mutex)
    throw std::runtime_error("named process admission was already installed");
  if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string_view::npos)
    throw std::runtime_error("named process admission requires an immutable profile ID");

  auto* module = reinterpret_cast<std::byte*>(GetModuleHandleW(L"UnityPlayer.dll"));
  if (!module)
    throw std::runtime_error("UnityPlayer is unavailable for named process admission");
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
    throw std::runtime_error("UnityPlayer has an invalid DOS header");
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    throw std::runtime_error("UnityPlayer has an unsupported image header");
  const auto image_size = nt->OptionalHeader.SizeOfImage;
  const auto within_image = [image_size](std::size_t offset, std::size_t count) {
    return offset < image_size && count <= image_size - offset;
  };
  const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!within_image(imports.VirtualAddress, imports.Size) || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR))
    throw std::runtime_error("UnityPlayer has no valid import table");

  void** mutex_slot = nullptr;
  for (std::size_t offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imports.Size;
       offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
    const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(module + imports.VirtualAddress + offset);
    if (!descriptor->Name)
      break;
    if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk)
      continue;
    for (std::size_t index = 0;; ++index) {
      const auto names_offset = std::size_t(descriptor->OriginalFirstThunk) + index * sizeof(IMAGE_THUNK_DATA);
      const auto functions_offset = std::size_t(descriptor->FirstThunk) + index * sizeof(IMAGE_THUNK_DATA);
      if (!within_image(names_offset, sizeof(IMAGE_THUNK_DATA))
          || !within_image(functions_offset, sizeof(IMAGE_THUNK_DATA)))
        throw std::runtime_error("UnityPlayer has an invalid import thunk");
      const auto* name_thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(module + names_offset);
      if (!name_thunk->u1.AddressOfData)
        break;
      if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal))
        continue;
      const auto name_offset = std::size_t(name_thunk->u1.AddressOfData) + offsetof(IMAGE_IMPORT_BY_NAME, Name);
      constexpr char expected[] = "CreateMutexA";
      if (!within_image(name_offset, sizeof(expected)))
        throw std::runtime_error("UnityPlayer has an invalid import name");
      if (std::memcmp(module + name_offset, expected, sizeof(expected)) != 0)
        continue;
      if (mutex_slot)
        throw std::runtime_error("UnityPlayer has ambiguous mutex imports");
      mutex_slot = reinterpret_cast<void**>(module + functions_offset);
    }
  }
  if (!mutex_slot || !*mutex_slot)
    throw std::runtime_error("UnityPlayer's process admission import is unavailable");

  admitted_profile_id.assign(id);
  const auto original = reinterpret_cast<CreateMutexAFunction>(*mutex_slot);
  DWORD previous_protection{};
  if (!VirtualProtect(mutex_slot, sizeof(void*), PAGE_READWRITE, &previous_protection))
    throw std::runtime_error("could not protect UnityPlayer's process admission import");
  original_create_mutex = original;
  InterlockedExchangePointer(mutex_slot, reinterpret_cast<void*>(&ProfileCreateMutexA));
  DWORD ignored{};
  if (!VirtualProtect(mutex_slot, sizeof(void*), previous_protection, &ignored))
    throw std::runtime_error("could not restore UnityPlayer's import protection");
}

} // namespace stfc::profiles::community_mod
#elif __APPLE__
namespace stfc::profiles::community_mod {
void InstallProcessAdmission(std::string_view)
{
  // The coordinator starts the bundle's native executable directly. LaunchServices
  // reuse is avoided by that launch contract; no Windows mutex exists on macOS.
}
} // namespace stfc::profiles::community_mod
#endif
