set_project("stfc-profiles")
set_version("0.1.0")
set_languages("c++23")
set_runtimes("MT")
add_rules("mode.debug", "mode.release")

option("community_mod_root")
    set_showmenu(true)
    set_description("Explicit host source root for legacy adapter compilation; not a release dependency")
    set_default("")
option_end()

includes("xmake/library.lua")

target("legacy-contract-tests")
    set_kind("binary")
    add_deps("stfc-profiles-core")
    add_files("tests/legacy_contract_test.cc")
    set_exceptions("cxx")
target_end()

target("consumer-smoke")
    set_kind("binary")
    add_deps("stfc-profiles-core")
    add_files("tests/consumer_smoke.cc")
    set_exceptions("cxx")
target_end()

if is_plat("windows") then
    target("prefs-store-tests")
        set_kind("binary")
        add_deps("stfc-profiles-core")
        add_files("tests/prefs_store_test.cc")
        set_exceptions("cxx")
    target_end()
end

local host = get_config("community_mod_root")
if host and #host > 0 then
    if not os.isdir(path.join(host, "mods/src/il2cpp")) then
        raise("Explicit community-mod host headers are missing")
    end
    add_repositories("stfc-community-mod-repo " .. path.join(host, "xmake-packages"))
    add_requires("eastl", "spdlog", "spud v0.2.0-8")
    target("stfc-profiles-community-mod-adapter")
        set_kind("static")
        set_default(false)
        add_deps("stfc-profiles-core")
        add_files("adapters/community_mod/profile_isolation.cc", "integration/community_mod/legacy_compat.cc")
        add_includedirs(path.join(host, "mods/src"), path.join(host, "third_party/libil2cpp"))
        add_packages("eastl", "spdlog", "spud")
        set_exceptions("cxx")
        add_defines("NOMINMAX")
    target_end()
end
