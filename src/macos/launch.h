#pragma once
#include <filesystem>
#include <string_view>
#include <sys/types.h>

namespace stfc::profiles::detail {
// File inspection never loads the runtime into the coordinator.
bool HasMacLaunchContract(const std::filesystem::path& library);
void CheckMacLoaderEntitlements(const std::filesystem::path& executable);
pid_t SpawnMacProfileSuspended(const std::filesystem::path& executable,
                              const std::filesystem::path& library, std::string_view id,
                              const std::filesystem::path& log);
}
