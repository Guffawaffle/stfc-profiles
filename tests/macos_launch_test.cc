#include "../src/macos/launch.h"
#include "stfc_profiles/catalog.h"
#include "stfc_profiles/prefs_store.h"
#include "stfc_profiles/session.h"
#include <nlohmann/json.hpp>
#include <libproc.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace stfc::profiles;
void Check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
Json Call(Json request) {
  request["apiVersion"] = 2;
  return Json::parse(ExecuteCatalogRequest(request.dump()));
}
int main(int argc, char** argv)
{
  try {
    if (argc == 5 && std::string_view(argv[1]) == "-stfc-profile" && std::string_view(argv[3]) == "-logFile") {
      Check(std::getenv("DYLD_INSERT_LIBRARIES") && !std::getenv("DYLD_LIBRARY_PATH"), "injection environment mismatch");
      Check(fs::current_path() == fs::canonical(argv[0]).parent_path(), "incorrect launch cwd");
      if (std::string_view(argv[2]) == "00000000000000000000000000000000") return 0;
      SessionLease lease(DefaultCatalogRoot(), argv[2]);
      ProfilePrefsStore prefs(DefaultCatalogRoot(), argv[2], ProfileOpenMode::New, lease);
      prefs.SetString(u"synthetic", u"no account data"); prefs.FinishNewProfile(); lease.MarkReady();
      const auto stop = fs::path(argv[4]).string() + ".stop";
      for (int i = 0; i < 300 && !fs::exists(stop); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      return 0;
    }
    Check(argc == 2, "pass the synthetic runtime library");
    const auto library = fs::canonical(argv[1]);
    Check(detail::HasMacLaunchContract(library), "native runtime contract missing");
    Check(!detail::HasMacLaunchContract(fs::canonical(argv[0])), "executable was accepted as a dylib");
    char pattern[] = "/tmp/stfc-profiles-launch-XXXXXX";
    Check(mkdtemp(pattern), "fixture directory unavailable");
    const fs::path fixture = fs::canonical(pattern);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{fixture};
    const auto truncated = fixture / "broken.dylib";
    std::ofstream(truncated) << "not Mach-O";
    Check(!detail::HasMacLaunchContract(truncated), "malformed runtime accepted");
    const auto marker = fixture / "injected.txt";
    setenv("STFC_PROFILES_TEST_INJECTION", marker.c_str(), 1);
    setenv("DYLD_LIBRARY_PATH", "/synthetic/untrusted", 1);
    const auto child = detail::SpawnMacProfileSuspended(fs::canonical(argv[0]), library,
        "00000000000000000000000000000000", fixture / "Player.log");
    struct Child { pid_t pid; ~Child() { if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); } } } owned{child};
    char image[PROC_PIDPATHINFO_MAXSIZE]{};
    Check(proc_pidpath(child, image, sizeof(image)) > 0 && fs::path(image) == fs::canonical(argv[0]),
          "suspended process identity unavailable");
    Check(!fs::exists(marker), "runtime initialized before pending publication");
    Check(kill(child, SIGCONT) == 0, "resume failed");
    int status = 0; Check(waitpid(child, &status, 0) == child, "child wait failed"); owned.pid = 0;
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && fs::exists(marker), "native injection/arguments failed");
    // Catalog end-to-end uses only ephemeral CI runner state and its synthetic Keychain.
    if (std::getenv("STFC_PROFILES_TEST_KEYCHAIN_DIRECTORY")) {
      const auto game = fixture / "game"; fs::create_directory(game);
      fs::copy_file(fs::canonical(argv[0]), game / "Star Trek Fleet Command");
      detail::CheckMacLoaderEntitlements(game / "Star Trek Fleet Command");
      std::vector<Json> profiles;
      struct Profiles {
        std::vector<Json>& items;
        ~Profiles() {
          for (const auto& profile : items) {
            try {
              const auto id = profile.at("id").get<std::string>();
              std::ofstream(profile.at("logPath").get<std::string>() + ".stop") << "stop";
              std::this_thread::sleep_for(std::chrono::milliseconds(300));
              const auto paths = Call({{"operation", "paths"}, {"id", id}});
              const auto archived = Call({{"operation", "archive"}, {"id", id}, {"expectedRevision", paths.at("profile").at("revision")}});
              if (archived.value("ok", false)) Call({{"operation", "delete"}, {"id", id}, {"archived", true},
                  {"permanent", true}, {"expectedRevision", archived.at("profile").at("revision")}});
            } catch (...) {}
          }
        }
      } settle{profiles};
      for (int i = 0; i < 2; ++i) {
        const auto created = Call({{"operation", "create"}, {"name", "Synthetic Mac launch"}});
        Check(created.value("ok", false), created.dump()); profiles.push_back(created.at("profile"));
        const auto id = profiles.back().at("id");
        Json request{{"operation", "launch"}, {"id", id}, {"gameDirectory", game.string()}, {"runtimeLibrary", library.string()}};
        const auto launched = Call(request);
        Check(launched.value("ok", false) && launched.at("readiness") == "ready", launched.dump());
        Check(!Call(request).value("ok", false), "duplicate live profile launched");
      }
      Check(Call({{"operation", "sessions"}}).at("sessions").size() == 2, "independent sessions missing");
    }
    std::cout << "macOS native launch tests passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
