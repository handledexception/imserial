cmake_minimum_required(VERSION 3.24)
project(imserial VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- SDL3 (static, only the subsystems we need) -----------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG ON CACHE BOOL "" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL3)

# --- cimgui + Dear ImGui (built as a C++ static lib; app code is pure C) ----
FetchContent_Declare(cimgui
  GIT_REPOSITORY https://github.com/cimgui/cimgui.git
  GIT_TAG 1.92.9b
  GIT_SHALLOW TRUE
  GIT_SUBMODULES imgui
  SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(cimgui)

find_package(Freetype QUIET)
if(NOT Freetype_FOUND)
  set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
  set(SKIP_INSTALL_ALL ON) # keep FreeType's headers/libs out of our packages
  FetchContent_Declare(freetype
    URL https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(freetype)
  add_library(Freetype::Freetype ALIAS freetype)
endif()

find_package(OpenGL REQUIRED)

include(cmake/libserialport.cmake)

set(CIMGUI_DIR ${cimgui_SOURCE_DIR})
set(IMGUI_DIR ${cimgui_SOURCE_DIR}/imgui)
add_library(cimgui STATIC
  ${CIMGUI_DIR}/cimgui.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
  ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp)
target_include_directories(cimgui PUBLIC ${CIMGUI_DIR} ${CIMGUI_DIR}/generator/output ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(cimgui PUBLIC
  IMGUI_ENABLE_FREETYPE
  IMGUI_ENABLE_STB_TRUETYPE
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1
  IMGUI_USE_WCHAR32
  CIMGUI_USE_SDL3
  CIMGUI_USE_OPENGL3
  "IMGUI_IMPL_API=extern \"C\"")
target_link_libraries(cimgui PUBLIC SDL3::SDL3 Freetype::Freetype OpenGL::GL)
if(MSVC)
  target_compile_options(cimgui PRIVATE /w)
else()
  target_compile_options(cimgui PRIVATE -w)
endif()

# --- Embedded assets ---------------------------------------------------------
set(ASSET_FILES
  Inter-Regular.ttf Inter-SemiBold.ttf IBMPlexMono-Regular.ttf IBMPlexMono-Bold.ttf
  JetBrainsMono-Regular.ttf JetBrainsMono-Bold.ttf codicon.ttf)
set(ASSET_SOURCES)
foreach(f ${ASSET_FILES})
  string(MAKE_C_IDENTIFIER "asset_${f}" sym)
  set(out ${CMAKE_BINARY_DIR}/assets/${sym}.c)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/assets/fonts/${f} -DOUT=${out} -DSYM=${sym} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/fonts/${f} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding ${f}")
  list(APPEND ASSET_SOURCES ${out})
endforeach()

# --- Application -------------------------------------------------------------
set(APP_SOURCES
  src/main.c
  src/app/app.c
  src/app/settings.c
  src/app/fontlist.c
  src/app/log.c
  src/ui/layout.c
  src/serial/serial_sp.c
  src/serial/session.c
  src/term/termbuf.c
  src/ui/theme.c
  src/ui/widgets.c
  src/ui/titlebar.c
  src/ui/sidebar.c
  src/ui/tabs.c
  src/ui/termview.c
  src/ui/sendbar.c
  src/ui/statusbar.c
  src/ui/quickpick.c
  src/ui/preferences.c
  src/ui/appicon.c
  third_party/cjson/cJSON.c)

add_executable(imserial WIN32 ${APP_SOURCES} ${ASSET_SOURCES})
if(WIN32)
  target_sources(imserial PRIVATE packaging/windows/imserial.rc) # app icon
endif()
target_include_directories(imserial PRIVATE src third_party)
target_link_libraries(imserial PRIVATE cimgui serialport SDL3::SDL3 OpenGL::GL)
target_compile_definitions(imserial PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS IMSERIAL_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(imserial PRIVATE /W3 /utf-8)
else()
  target_compile_options(imserial PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_libraries(imserial PRIVATE m pthread)
endif()

# --- Tests -------------------------------------------------------------------
enable_testing()
add_executable(test_termbuf tests/test_termbuf.c src/term/termbuf.c)
target_include_directories(test_termbuf PRIVATE src)
add_test(NAME termbuf COMMAND test_termbuf)
add_executable(test_log tests/test_log.c src/app/log.c)
target_include_directories(test_log PRIVATE src)
target_link_libraries(test_log PRIVATE SDL3::SDL3)
add_test(NAME log COMMAND test_log)
add_executable(test_fontlist tests/test_fontlist.c src/app/fontlist.c)
target_include_directories(test_fontlist PRIVATE src)
target_link_libraries(test_fontlist PRIVATE SDL3::SDL3 Freetype::Freetype)
add_test(NAME fontlist COMMAND test_fontlist)
add_executable(test_reconnect tests/test_reconnect.c tests/fake_serial.c src/serial/session.c)
target_include_directories(test_reconnect PRIVATE src tests)
target_link_libraries(test_reconnect PRIVATE SDL3::SDL3)
add_test(NAME reconnect COMMAND test_reconnect)
add_executable(test_session tests/test_session.c src/serial/session.c src/serial/serial_sp.c)
target_include_directories(test_session PRIVATE src)
target_link_libraries(test_session PRIVATE serialport SDL3::SDL3)

# --- Install + packaging (cpack) ---------------------------------------------
# Windows ships as a flat zip (imserial.exe at the top level); Linux installs
# under the usual bin/ and share/ layout, which CPack roots at /usr for the
# .deb and .rpm.
if(WIN32)
  set(IMSERIAL_BINDIR .)
else()
  set(IMSERIAL_BINDIR bin)
endif()
install(TARGETS imserial RUNTIME DESTINATION ${IMSERIAL_BINDIR})
if(UNIX AND NOT APPLE)
  install(FILES packaging/imserial.desktop DESTINATION share/applications)
  install(FILES assets/icon/imserial.svg DESTINATION share/icons/hicolor/scalable/apps)
  install(FILES assets/icon/imserial-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME imserial.png)
endif()

set(CPACK_PACKAGE_NAME imserial)
set(CPACK_PACKAGE_VENDOR "Paul Hindt")
set(CPACK_PACKAGE_CONTACT "Paul Hindt <phindt@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/handledexception/imserial")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast serial terminal with automatic reconnect")
set(CPACK_PACKAGE_DESCRIPTION
  "A fast, minimal serial terminal with a VS Code-style UI. Each tab follows
its device by USB identity and reconnects automatically when it comes back.")
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES ON)
set(CPACK_SOURCE_GENERATOR "")
if(WIN32)
  set(CPACK_GENERATOR ZIP)
elseif(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "TGZ;DEB;RPM")
  # .deb: let dpkg-shlibdeps compute Depends from the linked libraries.
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION electronics)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # .rpm: rpmbuild computes Requires itself. Don't claim ownership of
  # directories that belong to the filesystem / hicolor-icon-theme packages
  # (this list replaces CPack's default, so the base directories are repeated).
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
  set(CPACK_RPM_PACKAGE_LICENSE "MIT")
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr /usr/bin /usr/share
    /usr/share/applications
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/scalable
    /usr/share/icons/hicolor/scalable/apps
    /usr/share/icons/hicolor/256x256
    /usr/share/icons/hicolor/256x256/apps)
endif()
include(CPack)
