cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" STROJANKA_VERSION_RAW)
string(STRIP "${STROJANKA_VERSION_RAW}" STROJANKA_VERSION)
project(Strojanka VERSION ${STROJANKA_VERSION} LANGUAGES C CXX)
# file(READ) above does not register VERSION as a reconfigure dependency on its own, so an
# incremental build could keep embedding a stale version after a plain VERSION bump.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(STROJANKA_BUILD_TESTS "Build the unit tests" ON)

if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive- /Zc:__cplusplus /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  # Release keeps full optimization but also emits a .pdb, so a crash-handler .dmp from a
  # shipped binary (see common/crash_handler.*) can later be matched to function/file/line.
  # /OPT:REF,ICF restores the dead-code/COMDAT-folding stripping that /DEBUG otherwise disables.
  add_compile_options($<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF,ICF>)
endif()

configure_file(src/core/version.h.in "${CMAKE_BINARY_DIR}/generated/core/version.h" @ONLY)

find_package(dpp CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)

# Plain standard-library code shared by everything, including the dependency-free installer.
set(STROJANKA_COMMON_SOURCES
  src/common/strings.cpp
  src/common/config.cpp
  src/common/payload.cpp
  src/common/platform.cpp
  src/common/instance_lock.cpp
  src/common/crash_handler.cpp
  src/common/html.cpp
  src/common/update_backup.cpp
)

# Turns a file into a byte-array header (used for the HTML interfaces of the installer and server).
function(strojanka_embed input output name)
  add_custom_command(
    OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/${input} -DOUTPUT=${output} -DNAME=${name}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS ${input} cmake/embed_file.cmake
    COMMENT "Embedding ${input}"
    VERBATIM
  )
endfunction()

# --- core: business logic, storage, analysis (no Discord) -------------------------------
add_library(strojanka_core STATIC
  ${STROJANKA_COMMON_SOURCES}
  src/core/log.cpp
  src/core/database.cpp
  src/core/migrations.cpp
  src/core/repository.cpp
  src/core/portfolio.cpp
  src/core/messages.cpp
  src/core/commands.cpp
  src/core/monitor_service.cpp
  src/core/screener.cpp
  src/core/run_state.cpp
  src/core/admin_service.cpp
  src/core/server_console.cpp
  src/core/arena/engine.cpp
  src/core/arena/arena_service.cpp
  src/core/ml/gbdt.cpp
  src/core/ml/pythia.cpp
  src/core/politicians/disclosure_source.cpp
  src/core/politicians/senate_efd.cpp
  src/core/politicians/house_clerk.cpp
  src/core/politicians/politician_service.cpp
  src/core/archive.cpp
  src/core/pdf_text.cpp
  src/core/pdf_crypto.cpp
  src/core/http.cpp
  src/core/news/sentiment.cpp
  src/core/news/news_provider.cpp
  src/core/analysis/indicators.cpp
  src/core/analysis/zigzag.cpp
  src/core/analysis/analyzer.cpp
  src/core/analysis/calibrator.cpp
  src/core/analysis/classifier.cpp
  src/core/analysis/walk_forward.cpp
  src/core/market/yahoo_provider.cpp
)
target_include_directories(strojanka_core PUBLIC src "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(strojanka_core PUBLIC unofficial::sqlite3::sqlite3 nlohmann_json::nlohmann_json CURL::libcurl ZLIB::ZLIB)
if(WIN32)
  target_link_libraries(strojanka_core PUBLIC dbghelp)
endif()

# Windows resource file (icon + version info) for an executable.
set(STROJANKA_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/icon/strojanka.ico")
function(strojanka_add_resources target name description)
  if(WIN32)
    set(RC_NAME "${name}")
    set(RC_DESCRIPTION "${description}")
    configure_file(src/common/app.rc.in "${CMAKE_BINARY_DIR}/generated/${target}.rc" @ONLY)
    target_sources(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated/${target}.rc")
  endif()
endfunction()

# --- bot: Discord front-end and BotHost, shared by the headless bot and the server window ----
add_library(strojanka_bot STATIC src/bot/discord_bot.cpp src/bot/tls_trust.cpp src/bot/bot_host.cpp src/bot/session_watch.cpp)
target_link_libraries(strojanka_bot PUBLIC strojanka_core dpp::dpp)
if(WIN32)
  target_link_libraries(strojanka_bot PUBLIC crypt32 user32)
endif()

# Headless bot and command-line tools.
add_executable(strojanka src/bot/main.cpp)
target_link_libraries(strojanka PRIVATE strojanka_bot)
strojanka_add_resources(strojanka strojanka "Strojanka stock alert bot")

# --- server window: the bot with a dashboard, live log and command line ----------------------
set(STROJANKA_SERVER_UI_HEADER "${CMAKE_BINARY_DIR}/generated/server/server_ui.h")
strojanka_embed(src/server/ui/server.html "${STROJANKA_SERVER_UI_HEADER}" kServerHtml)
add_executable(StrojankaServer src/server/main.cpp "${STROJANKA_SERVER_UI_HEADER}")
target_include_directories(StrojankaServer PRIVATE third_party/webview)
target_link_libraries(StrojankaServer PRIVATE strojanka_bot)
strojanka_add_resources(StrojankaServer StrojankaServer "Strojanka Server")
if(WIN32)
  target_include_directories(StrojankaServer PRIVATE third_party/webview2)
  set_property(TARGET StrojankaServer PROPERTY WIN32_EXECUTABLE TRUE)
  target_link_options(StrojankaServer PRIVATE /ENTRY:mainCRTStartup)
  target_link_libraries(StrojankaServer PRIVATE ole32 shell32 shlwapi user32 version advapi32)
elseif(APPLE)
  set_source_files_properties(assets/icon/strojanka.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_sources(StrojankaServer PRIVATE assets/icon/strojanka.icns)
  set_target_properties(StrojankaServer PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "Strojanka Server"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.strojanka.server"
    MACOSX_BUNDLE_ICON_FILE strojanka.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
  target_link_libraries(StrojankaServer PRIVATE "-framework Cocoa" "-framework WebKit" dl)
endif()

# --- installer (static runtime, no DLLs; window via the system web engine) ------------------
set(STROJANKA_UI_HEADER "${CMAKE_BINARY_DIR}/generated/installer/installer_ui.h")
strojanka_embed(src/installer/ui/installer.html "${STROJANKA_UI_HEADER}" kInstallerHtml)
add_executable(StrojankaInstaller
  src/installer/main.cpp
  src/installer/installer_core.cpp
  src/installer/platform_install.cpp
  src/installer/console_ui.cpp
  src/installer/gui.cpp
  "${STROJANKA_UI_HEADER}"
  ${STROJANKA_COMMON_SOURCES}
)
target_include_directories(StrojankaInstaller PRIVATE src "${CMAKE_BINARY_DIR}/generated" third_party/webview)
# nlohmann_json is header-only, so the installer still ships as a single file.
target_link_libraries(StrojankaInstaller PRIVATE nlohmann_json::nlohmann_json)
strojanka_add_resources(StrojankaInstaller StrojankaInstaller "Strojanka Setup")
if(WIN32)
  target_include_directories(StrojankaInstaller PRIVATE third_party/webview2)
  set_property(TARGET StrojankaInstaller PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET StrojankaInstaller PROPERTY WIN32_EXECUTABLE TRUE)
  target_link_options(StrojankaInstaller PRIVATE /ENTRY:mainCRTStartup)
  target_link_libraries(StrojankaInstaller PRIVATE ole32 shell32 shlwapi uuid advapi32 user32 version dbghelp)
elseif(APPLE)
  set_source_files_properties(assets/icon/strojanka.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_sources(StrojankaInstaller PRIVATE assets/icon/strojanka.icns)
  set_target_properties(StrojankaInstaller PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "Strojanka Setup"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.strojanka.setup"
    MACOSX_BUNDLE_ICON_FILE strojanka.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
  target_link_libraries(StrojankaInstaller PRIVATE "-framework Cocoa" "-framework WebKit" dl)
endif()

# --- packer (build tool) ------------------------------------------------------------------
add_executable(strojanka_pack src/packer/main.cpp ${STROJANKA_COMMON_SOURCES})
target_include_directories(strojanka_pack PRIVATE src)
target_link_libraries(strojanka_pack PRIVATE nlohmann_json::nlohmann_json)  # header-only, for common/update_backup
if(WIN32)
  target_link_libraries(strojanka_pack PRIVATE dbghelp)
endif()

# --- tests --------------------------------------------------------------------------------
if(STROJANKA_BUILD_TESTS)
  enable_testing()
  add_executable(strojanka_tests tests/test_main.cpp)
  target_link_libraries(strojanka_tests PRIVATE strojanka_core)
  target_compile_definitions(strojanka_tests PRIVATE STROJANKA_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME strojanka_tests COMMAND strojanka_tests)
endif()

# --- install layout (what ends up inside the installer) ------------------------------------
install(TARGETS strojanka RUNTIME DESTINATION .)
install(TARGETS StrojankaServer RUNTIME DESTINATION . BUNDLE DESTINATION .)
if(WIN32)
  # vcpkg copies dpp, openssl, zlib, curl, sqlite... next to the exe at build time.
  install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/" DESTINATION . FILES_MATCHING PATTERN "*.dll")
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  include(InstallRequiredSystemLibraries)
  install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)
endif()
install(FILES VERSION CHANGELOG.md DESTINATION .)
install(DIRECTORY documentation/ DESTINATION docs)

# --- dist: stage the install layout and pack it into dist/StrojankaInstaller ----------------
set(STROJANKA_STAGE_DIR "${CMAKE_BINARY_DIR}/stage")
set(STROJANKA_DIST_DIR "${CMAKE_SOURCE_DIR}/dist")
if(APPLE)
  # macOS: dist/StrojankaInstaller.app, with the payload appended to the bundle's executable.
  set(STROJANKA_APP "${STROJANKA_DIST_DIR}/StrojankaInstaller.app")
  add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${STROJANKA_STAGE_DIR}" "${STROJANKA_APP}"
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${STROJANKA_STAGE_DIR}" --config $<CONFIG>
    COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:StrojankaInstaller> "${STROJANKA_APP}"
    COMMAND $<TARGET_FILE:strojanka_pack> $<TARGET_FILE:StrojankaInstaller> "${STROJANKA_STAGE_DIR}"
            "${STROJANKA_APP}/Contents/MacOS/StrojankaInstaller"
    DEPENDS strojanka StrojankaServer StrojankaInstaller strojanka_pack
    COMMENT "Packing ${STROJANKA_APP}"
    VERBATIM
  )
else()
  set(STROJANKA_INSTALLER_NAME "StrojankaInstaller$<$<BOOL:${WIN32}>:.exe>")
  add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${STROJANKA_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${STROJANKA_STAGE_DIR}" --config $<CONFIG>
    COMMAND $<TARGET_FILE:strojanka_pack> $<TARGET_FILE:StrojankaInstaller> "${STROJANKA_STAGE_DIR}"
            "${STROJANKA_DIST_DIR}/${STROJANKA_INSTALLER_NAME}"
    DEPENDS strojanka StrojankaServer StrojankaInstaller strojanka_pack
    COMMENT "Packing the installer into ${STROJANKA_DIST_DIR}"
    VERBATIM
  )
endif()
