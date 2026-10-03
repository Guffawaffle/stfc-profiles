-- Embeddable shared owner; both distributions and the native Bridge API link
-- this implementation. Package requirements are static transitive dependencies.
add_requires("nlohmann_json 3.12.0")
-- Resolve the profile-owned archive recipe for independent consumer builds.
add_repositories("stfc-profiles-packages " .. path.join(os.scriptdir(), "../xmake-packages"))
add_requires("libarchive 3.8.9", {configs = {xz = true, zlib = true, openssl3 = false,
    bzip2 = false, lz4 = false, lzo = false, zstd = false}})
target("stfc-profiles-core")
    set_kind("static")
    set_languages("c++23")
    set_exceptions("cxx")
    add_files("../src/component.cc", "../src/catalog.cc", "../src/prefs_store.cc", "../src/installation.cc")
    add_headerfiles("../include/(stfc_profiles/**.h)")
    add_includedirs("../include", {public = true})
    add_packages("nlohmann_json", {public = true})
    add_packages("libarchive", {public = true})
    if is_plat("windows") then
        add_files("../src/windows/*.cc")
        add_defines("NOMINMAX", {public = true})
        add_syslinks("crypt32", "bcrypt", "shell32", "ole32", "advapi32", "winhttp", {public = true})
    elseif is_plat("macosx") then
        add_files("../src/macos/*.cc")
        add_frameworks("Security", "CoreFoundation", {public = true})
        add_syslinks("proc", {public = true})
    end
    on_load(function(target)
        if not target:is_plat("windows", "macosx") then
            raise("STFC Profiles supports Windows and macOS native hosts")
        end
    end)
target_end()
