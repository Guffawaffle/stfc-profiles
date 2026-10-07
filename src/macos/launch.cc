#include "launch.h"
#include "stfc_profiles/catalog.h"
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <libkern/OSByteOrder.h>
#include <spawn.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>
extern char** environ;

namespace stfc::profiles::detail {
namespace {
template<class T> T Read(const std::vector<char>& bytes, std::size_t offset)
{
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw CatalogError("runtime_unavailable", "The profile runtime has a truncated Mach-O image.");
  T value; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
constexpr cpu_type_t Architecture()
{
#if __arm64__
  return CPU_TYPE_ARM64;
#else
  return CPU_TYPE_X86_64;
#endif
}
struct CFReleaseOwner {
  CFTypeRef value = nullptr;
  ~CFReleaseOwner() { if (value) CFRelease(value); }
};
}
bool HasMacLaunchContract(const std::filesystem::path& library)
{
  std::ifstream input(library, std::ios::binary | std::ios::ate);
  if (!input) return false;
  const auto length = input.tellg();
  if (length < 0 || length > 256 * 1024 * 1024) return false;
  std::vector<char> bytes(static_cast<std::size_t>(length));
  input.seekg(0); input.read(bytes.data(), bytes.size());
  if (!input) return false;
  try {
    std::size_t begin = 0, size = bytes.size();
    const auto magic = Read<std::uint32_t>(bytes, 0);
    if (magic == FAT_CIGAM || magic == FAT_CIGAM_64) {
      const auto header = Read<fat_header>(bytes, 0);
      const auto count = OSSwapBigToHostInt32(header.nfat_arch);
      if (count > 64) return false;
      bool found = false;
      for (std::uint32_t i = 0; i < count; ++i) {
        if (magic == FAT_CIGAM) {
          const auto arch = Read<fat_arch>(bytes, sizeof(header) + i * sizeof(fat_arch));
          if (OSSwapBigToHostInt32(arch.cputype) != Architecture()) continue;
          begin = OSSwapBigToHostInt32(arch.offset); size = OSSwapBigToHostInt32(arch.size);
        } else {
          const auto arch = Read<fat_arch_64>(bytes, sizeof(header) + i * sizeof(fat_arch_64));
          if (OSSwapBigToHostInt32(arch.cputype) != Architecture()) continue;
          begin = OSSwapBigToHostInt64(arch.offset); size = OSSwapBigToHostInt64(arch.size);
        }
        found = true; break;
      }
      if (!found || begin > bytes.size() || size > bytes.size() - begin) return false;
    }
    const auto header = Read<mach_header_64>(bytes, begin);
    if (header.magic != MH_MAGIC_64 || header.cputype != Architecture() || header.filetype != MH_DYLIB
        || size < sizeof(header) || header.sizeofcmds > size - sizeof(header)) return false;
    auto offset = begin + sizeof(header);
    const auto end = offset + header.sizeofcmds;
    for (std::uint32_t i = 0; i < header.ncmds; ++i) {
      if (offset > end || sizeof(load_command) > end - offset) return false;
      const auto command = Read<load_command>(bytes, offset);
      if (command.cmdsize < sizeof(command) || command.cmdsize > end - offset) return false;
      if (command.cmd == LC_SEGMENT_64) {
        if (command.cmdsize < sizeof(segment_command_64)) return false;
        const auto segment = Read<segment_command_64>(bytes, offset);
        if (segment.nsects > (command.cmdsize - sizeof(segment)) / sizeof(section_64)) return false;
        for (std::uint32_t n = 0; n < segment.nsects; ++n) {
          const auto section = Read<section_64>(bytes, offset + sizeof(segment) + n * sizeof(section_64));
          if (std::strncmp(section.sectname, "__stfc_profile", 16) != 0) continue;
          if (section.size != sizeof(std::uint32_t) || section.offset > size
              || section.size > size - section.offset) return false;
          return Read<std::uint32_t>(bytes, begin + section.offset) == 1;
        }
      }
      offset += command.cmdsize;
    }
  } catch (const CatalogError&) { return false; }
  return false;
}
void CheckMacLoaderEntitlements(const std::filesystem::path& executable)
{
  const auto path = executable.string();
  CFReleaseOwner url{CFURLCreateFromFileSystemRepresentation(nullptr,
      reinterpret_cast<const UInt8*>(path.data()), path.size(), false)};
  SecStaticCodeRef code = nullptr;
  if (!url.value || SecStaticCodeCreateWithPath(static_cast<CFURLRef>(url.value), kSecCSDefaultFlags, &code) != errSecSuccess)
    throw CatalogError("loader_entitlements", "The selected game's code signature could not be inspected.");
  CFReleaseOwner retained{code};
  CFDictionaryRef information = nullptr;
  if (SecCodeCopySigningInformation(code, kSecCSSigningInformation, &information) != errSecSuccess)
    throw CatalogError("loader_entitlements", "The selected game's signing information is unavailable.");
  CFReleaseOwner info{information};
  auto entitlements = static_cast<CFDictionaryRef>(CFDictionaryGetValue(information, kSecCodeInfoEntitlementsDict));
  if (!entitlements || CFGetTypeID(entitlements) != CFDictionaryGetTypeID())
    throw CatalogError("loader_entitlements", "The selected game does not allow profile runtime injection.");
  for (auto key : {CFSTR("com.apple.security.cs.allow-dyld-environment-variables"),
                  CFSTR("com.apple.security.cs.disable-library-validation")}) {
    if (CFDictionaryGetValue(entitlements, key) != kCFBooleanTrue)
      throw CatalogError("loader_entitlements", "The selected game needs loader entitlements. Open it with the community-mod launcher first.");
  }
}
pid_t SpawnMacProfileSuspended(const std::filesystem::path& executable,
                              const std::filesystem::path& library, std::string_view id,
                              const std::filesystem::path& log)
{
  // A named launch has one injection owner. Do not inherit another runtime or
  // loader search path, and never ask LaunchServices to reuse a running game.
  std::vector<std::string> environment;
  for (auto item = environ; item && *item; ++item)
    if (!std::string_view(*item).starts_with("DYLD_")) environment.emplace_back(*item);
  environment.emplace_back("DYLD_INSERT_LIBRARIES=" + library.string());
  std::vector<char*> env;
  for (auto& item : environment) env.push_back(item.data()); env.push_back(nullptr);
  std::vector<std::string> arguments{executable.string(), "-stfc-profile", std::string(id), "-logFile", log.string()};
  std::vector<char*> argv;
  for (auto& item : arguments) argv.push_back(item.data()); argv.push_back(nullptr);
  posix_spawnattr_t attributes;
  if (posix_spawnattr_init(&attributes) != 0)
    throw CatalogError("launch_failed", "Could not initialize the Mac profile launch.");
  struct Destroy { posix_spawnattr_t* value; ~Destroy() { posix_spawnattr_destroy(value); } } destroy{&attributes};
  if (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_START_SUSPENDED | POSIX_SPAWN_CLOEXEC_DEFAULT) != 0)
    throw CatalogError("launch_failed", "Could not suspend the Mac game for profile admission.");
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0)
    throw CatalogError("launch_failed", "Could not initialize Mac launch streams.");
  struct CloseActions { posix_spawn_file_actions_t* value; ~CloseActions() { posix_spawn_file_actions_destroy(value); } } close{&actions};
  // The game must not retain the coordinator's JSON pipes after it reports ready.
  if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0
      || posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) != 0
      || posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0
      || posix_spawn_file_actions_addchdir_np(&actions, executable.parent_path().c_str()) != 0)
    throw CatalogError("launch_failed", "Could not prepare Mac launch streams and working directory.");
  pid_t child = 0;
  if (posix_spawn(&child, executable.c_str(), &actions, &attributes, argv.data(), env.data()) != 0)
    throw CatalogError("launch_failed", "macOS could not start the selected game executable.");
  return child;
}
}
