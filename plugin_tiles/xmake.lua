includes("../lib/commonlibsse-ng")

set_project("GTSLandMap")
set_version("0.1.1")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.release", "mode.releasedbg")

target("GTSLandMap")
    add_rules("commonlibsse-ng.plugin", {
        name = "GTSLandMap",
        author = "Tom",
        description = "GTS Land Map: flat map camera, view, panning, night sheets and tiles in view"
    })
    add_files("src/**.cpp")
    -- the WebP decoder: official libwebp 1.6.0 Windows package (https://storage.googleapis.com/downloads.webmproject.org/releases/webp/index.html). xmake's own is 1.3.0, which has CVE-2023-4863.
    -- the package is built against the static C runtime and this DLL the shared one
    local webp = "../lib/libwebp-1.6.0-windows-x64"
    add_includedirs(webp .. "/include")
    add_linkdirs(webp .. "/lib")
    add_links("libwebp")
    add_shflags("/NODEFAULTLIB:LIBCMT", { force = true })
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
