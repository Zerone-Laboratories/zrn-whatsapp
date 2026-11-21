# zrn-whatsapp: one binary, three roles (the app on zrnui v2, --daemon, --notifier).
#
#   cmake --preset host-debug && cmake --build --preset host-debug        # this computer
#   cmake --preset phone-release && cmake --build --preset phone-release  # aarch64, needs the sysroot of zrn-sdk/tools/zrn
#
# zrn-sdk is taken from ZRN_SDK_DIR (default ../zrn-sdk, built in the tree as its examples do), or, with
# -DZWA_SYSTEM_SDK=ON, from an installed copy through pkg-config (zrnapp, zrnui).
cmake_minimum_required(VERSION 3.21)
project(zrn-whatsapp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 23)

option(ZWA_SYSTEM_SDK "link an installed zrn-sdk (pkg-config) instead of building ZRN_SDK_DIR" OFF)
set(ZRN_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zrn-sdk" CACHE PATH "zrn-sdk checkout")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET libsodium openssl sqlite3 zlib libcurl libsystemd libwebp libwebpdemux libjpeg libpng wayland-client)

if(ZWA_SYSTEM_SDK)
	pkg_check_modules(ZRN REQUIRED IMPORTED_TARGET zrnapp zrnui)
	set(ZRN_LIBS PkgConfig::ZRN)
	set(ZRN_INCLUDES ${ZRN_INCLUDE_DIRS})
else()
	add_subdirectory(${ZRN_SDK_DIR}/zrnapp ${CMAKE_BINARY_DIR}/zrnapp)
	set(ZRN_LIBS zrnapp::zrnapp zrnui::zrnui)
	set(ZRN_INCLUDES ${ZRN_SDK_DIR}/zrnui/include ${ZRN_SDK_DIR}/zrnapp/include)
endif()

# The SDK gaps of docs/UI-SPEC.md section 8: each one is used when the headers have it (ui/gaps.h).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${ZRN_INCLUDES} ${DEPS_INCLUDE_DIRS})
set(CMAKE_REQUIRED_DEFINITIONS)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(_h "#include <zrnui/controls.h>\n#include <zrnui/element.h>\n#include <zrnui/host.h>\n#include <zrnapp.h>\n")
macro(zwa_gap name body)
	check_cxx_source_compiles("${_h}int main(){ ${body}; return 0; }" ZWA_HAVE_${name})
	if(ZWA_HAVE_${name})
		list(APPEND ZWA_GAP_DEFS ZWA_HAVE_${name})
	endif()
endmacro()
zwa_gap(HOLDING "Zelement_on_holding(0, nullptr, nullptr)")
zwa_gap(THEME_CHANGED "Zroot_on_theme_changed(0, nullptr, nullptr)")
zwa_gap(CONTINUUM "Zframe_set_continuum_element(0, 0); Zpage_set_continuum_element(0, 0); (void)Z_TRANSITION_CONTINUUM")
zwa_gap(SLIDE_UP "(void)Z_TRANSITION_SLIDE_UP")
zwa_gap(NAVIGATE_WITH "Zframe_navigate_with_transition(0, nullptr, nullptr, Z_TRANSITION_NONE)")
zwa_gap(CACHE_MODE "Zelement_set_cache_mode(0, Z_CACHE_BITMAP)")
zwa_gap(INPUT_PANE "Zwindow_on_input_pane([](float, void*) {}, nullptr)")
zwa_gap(ANCHOR "Zscroll_viewer_set_anchor(0, 0)")
zwa_gap(SWIPE "Zelement_on_swipe(0, nullptr, nullptr)")
unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
message(STATUS "zrn-whatsapp: SDK gaps available: ${ZWA_GAP_DEFS}")

add_executable(zrn-whatsapp
	ui/main.cpp ui/io.cpp ui/look.cpp ui/media.cpp ui/page_main.cpp ui/page_chat.cpp ui/page_other.cpp ui/page_files.cpp ui/voice.cpp
	ui/entry_engine.cpp ui/entry_notify.cpp notify/policy.cpp notify/json.cpp
	engine/src/crypto.cpp engine/src/node.cpp engine/src/noise.cpp engine/src/sigproto.cpp engine/src/appstate.cpp
	engine/src/ws.cpp engine/src/store.cpp engine/src/ipc.cpp engine/src/fake.cpp engine/src/client.cpp engine/src/wamsg.cpp)
target_include_directories(zrn-whatsapp PRIVATE third_party engine/src notify ui)
target_compile_definitions(zrn-whatsapp PRIVATE ${ZWA_GAP_DEFS})
target_compile_options(zrn-whatsapp PRIVATE -Wno-deprecated-declarations)
find_package(Threads REQUIRED)
target_link_libraries(zrn-whatsapp PRIVATE ${ZRN_LIBS} PkgConfig::DEPS Threads::Threads)

include(GNUInstallDirs)
install(TARGETS zrn-whatsapp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES icon.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME zrn-whatsapp.svg)
