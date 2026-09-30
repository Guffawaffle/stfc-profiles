-- Embeddable dependency: includes("<pinned-source>/xmake/library.lua").
-- This file declares only the reusable library; it does not import the tests,
-- project options, package repositories or the standalone product bootstrap.
target("stfc-profiles-core")
    set_kind("static")
    set_languages("c++23")
    set_exceptions("cxx")
    add_files("../src/component.cc")
    add_headerfiles("../include/(stfc_profiles/**.h)")
    add_includedirs("../include", {public = true})
    if is_plat("windows") then
        add_files("../src/windows/*.cc")
        add_defines("NOMINMAX", {public = true})
        add_syslinks("crypt32", "shell32", "ole32", "advapi32", {public = true})
    end
target_end()
