-- include subprojects
includes("lib/commonlibsf")

-- set project constants
set_project("SpeedSync")
set_version("0.3")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- define targets
target("SpeedSync")
    add_rules("commonlibsf.plugin", {
        name = "SpeedSync",
        author = "litbeep",
        description = "SpeedSync SFSE plugin for Starfield",
        email = "litbeep@proton.me"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
