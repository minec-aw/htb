cmake_minimum_required(VERSION 3.27)

project(hyprtouchbar
    DESCRIPTION "Modern touch-friendly titlebars for Hyprland"
    VERSION 1.2.0
)

set(CMAKE_CXX_STANDARD 23)

set(SRC
    main.cpp
    barDeco.cpp
    BarPassElement.cpp
    CSDManager.cpp
    AppIcon.cpp
)

add_library(hyprtouchbar SHARED ${SRC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pixman-1
    wayland-server
    xkbcommon
    librsvg-2.0
)
target_link_libraries(hyprtouchbar PRIVATE rt PkgConfig::deps)

install(TARGETS hyprtouchbar)
