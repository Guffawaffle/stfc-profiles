set_project("stfc-profiles")
set_version("0.3.0")
set_languages("c++23")
set_runtimes("MT")
add_rules("mode.debug", "mode.release")

-- Retained as an explicit integration-evidence input; the adapter no longer
-- imports host headers, linked game assemblies, or host feature configuration.
option("community_mod_root")
    set_showmenu(true)
    set_description("Explicit consumer source root recorded by integration verification")
    set_default("")
option_end()
add_repositories("stfc-profiles-packages " .. path.join(os.scriptdir(), "xmake-packages"))
add_requires("spdlog", "spud v0.2.0-8")
includes("xmake/library.lua")

for _, entry in ipairs({
    {"identity-tests", "tests/identity_test.cc"},
    {"consumer-smoke", "tests/consumer_smoke.cc"},
    {"prefs-store-tests", "tests/prefs_store_test.cc"},
    {"catalog-tests", "tests/catalog_test.cc"},
    {"stfc-profiles", "cli/main.cc"}
}) do
    target(entry[1])
        set_kind("binary")
        add_deps("stfc-profiles-core")
        add_files(entry[2])
        set_exceptions("cxx")
        if is_plat("windows") then
            add_syslinks("uuid")
            if entry[1] == "catalog-tests" then add_deps("stfc-profiles-native", {inherit = false}) end
            if entry[1] == "stfc-profiles" then
                add_syslinks("comctl32")
                add_ldflags("/MANIFEST:EMBED", "/MANIFESTINPUT:" .. path.join(os.scriptdir(), "cli/windows.manifest"), {force = true})
            end
        end
    target_end()
end
if is_plat("windows") then
    target("user-import-transfer-tests")
        set_kind("binary")
        add_deps("stfc-profiles", "stfc-profiles-native", {inherit = false})
        add_files("tests/user_import_transfer_test.cc")
        add_packages("nlohmann_json")
        add_defines("NOMINMAX")
        add_syslinks("bcrypt", "advapi32")
        set_exceptions("cxx")
    target_end()
    target("user-import-source-tests")
        set_kind("binary")
        add_deps("stfc-profiles-core")
        add_files("tests/user_import_source_test.cc")
        set_exceptions("cxx")
    target_end()
    target("installation-tests")
        set_kind("binary")
        add_deps("stfc-profiles-core")
        add_files("tests/installation_test.cc")
        set_exceptions("cxx")
    target_end()
end
target("stfc-profiles-native")
    set_kind("shared")
    add_deps("stfc-profiles-core")
    add_files("src/c_api.cc")
    add_defines("STFC_PROFILES_EXPORTS")
    set_exceptions("cxx")
target_end()
target("stfc-profiles-community-mod-adapter")
    set_kind("static")
    set_default(false)
    add_deps("stfc-profiles-core")
    add_files("adapters/community_mod/*.cc")
    add_packages("spdlog", "spud")
    set_exceptions("cxx")
    add_defines("NOMINMAX")
    if is_plat("windows") then add_syslinks("shell32", "ole32", "advapi32", "uuid") end
target_end()
target("stfc-profiles-runtime")
    set_kind("shared")
    set_default(false)
    add_deps("stfc-profiles-community-mod-adapter")
    add_files("bootstrap/main.cc")
    add_packages("spud", "spdlog")
    set_exceptions("cxx")
    if is_plat("windows") then
        set_filename("version.dll")
        add_files("bootstrap/version.cc", "bootstrap/version.def")
        add_syslinks("shell32", "ole32", "advapi32", "uuid", "user32")
    end
target_end()

if is_plat("windows") then
    target("runtime-loader-tests")
        set_kind("binary")
        set_default(false)
        add_deps("stfc-profiles-runtime", {inherit = false})
        add_files("tests/runtime_loader_test.cc")
        set_exceptions("cxx")
    target_end()
end
