-- Deliberately outside the library project to verify an ordinary consumer.
set_project("stfc-profile-consumer")
set_languages("c++23")
set_runtimes("MT")
add_rules("mode.debug", "mode.release")
includes("../../xmake/library.lua")

target("example-consumer")
    set_kind("binary")
    add_deps("stfc-profiles-core")
    add_files("../../tests/consumer_smoke.cc")
    set_exceptions("cxx")
