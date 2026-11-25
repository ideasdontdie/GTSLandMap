includes("../lib/commonlibsse-ng")

set_project("GTSLandMap")
set_version("0.1.2")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.release", "mode.releasedbg")

target("GTSLandMap")
    add_rules("commonlibsse-ng.plugin", {
        name = "GTSLandMap",
        author = "Tom",
        description = "GTS Land Map: flat map camera, view, panning and night sheets"
    })
    add_files("src/**.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
