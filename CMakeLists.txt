cmake_minimum_required(VERSION 3.20)
project(tmnt2003_netplay C CXX)

# tmnt.exe is a 32-bit process: everything that goes into it (and the client,
# for symmetry) must be x86.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure with -A Win32: tmnt.exe is a 32-bit process")
endif()
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/minhook/src/hook.c)
  message(FATAL_ERROR "third_party is empty: git submodule update --init --recursive")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so players do not need the VC++ redistributable. The policy
# default makes third-party CMake projects with old minimum versions obey it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# --- third_party: MinHook (x86 hooks) ----------------------------------------
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# --- third_party: ENet (UDP with reliable/unreliable channels) ---------------
add_library(enet STATIC
  third_party/enet/callbacks.c third_party/enet/compress.c third_party/enet/host.c
  third_party/enet/list.c third_party/enet/packet.c third_party/enet/peer.c
  third_party/enet/protocol.c third_party/enet/win32.c)
target_include_directories(enet PUBLIC third_party/enet/include)
target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
target_compile_options(enet PRIVATE /wd5287)  # upstream mixes enum types in flags
target_link_libraries(enet PUBLIC ws2_32 winmm)

# --- third_party: Dear ImGui (in-game menu, client window) -------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE)

# --- third_party: SDL2 (client gamepads: XInput, DualShock/DualSense, ...) --
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_FORCE_STATIC_VCRT ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
foreach(sub AUDIO RENDER SENSOR LOCALE POWER FILESYSTEM)
  set(SDL_${sub} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt OPENGL OPENGLES VULKAN RENDER_D3D WASAPI DIRECTX_AUDIO)
  set(SDL_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

# --- our code ------------------------------------------------------------------
# _CRT_SECURE_NO_WARNINGS is defined empty: enet/win32.h defines it that way too.
set(OUR_DEFS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS=
             _WINSOCK_DEPRECATED_NO_WARNINGS UNICODE _UNICODE)
set(OUR_OPTS /W4 /permissive- /utf-8)

# common: protocol, config, log, hashing, sockets, transports
add_library(tmntnet_common STATIC
  src/common/config.cpp
  src/common/log.cpp
  src/common/net.cpp
  src/common/transport_enet.cpp
  src/common/transport_steam.cpp
  src/common/steam_boot.cpp
  src/common/steam_mock.cpp
  src/common/sha256.cpp
  src/common/keynames.cpp
  src/common/joincode.cpp
  src/common/screenshot.cpp)
target_include_directories(tmntnet_common PUBLIC src)
target_compile_definitions(tmntnet_common PUBLIC ${OUR_DEFS})
target_compile_options(tmntnet_common PUBLIC ${OUR_OPTS})
target_link_libraries(tmntnet_common PUBLIC enet ws2_32 bcrypt iphlpapi windowscodecs ole32)

# --- optional: Steam transport (Steamworks SDK; never part of the repo) ---------
#   cmake -B build -A Win32 -DSTEAMWORKS_SDK_DIR=C:/sdk/steamworks_sdk
# Without the SDK everything still builds, ENet (LAN / IP) only.
option(TMNTNET_WITH_STEAM "Steam P2P transport (needs STEAMWORKS_SDK_DIR)" ON)
set(STEAMWORKS_SDK_DIR "" CACHE PATH "Steamworks SDK root (contains public/steam and redistributable_bin)")
set(STEAM_API_DLL ${STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.dll)
set(TMNTNET_STEAM OFF)
if(TMNTNET_WITH_STEAM)
  if(EXISTS ${STEAMWORKS_SDK_DIR}/public/steam/isteamnetworkingmessages.h AND
     EXISTS ${STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.lib AND EXISTS ${STEAM_API_DLL})
    set(TMNTNET_STEAM ON)
    message(STATUS "Steam transport: ON (${STEAMWORKS_SDK_DIR})")
  else()
    message(WARNING "Steam transport: OFF, building ENet-only. Set -DSTEAMWORKS_SDK_DIR=<Steamworks SDK folder> "
                    "(public/steam, redistributable_bin/steam_api.lib/.dll) to enable it.")
  endif()
endif()
if(TMNTNET_STEAM)
  # The 32-bit import library and DLL live directly in redistributable_bin.
  add_library(steam_api SHARED IMPORTED)
  set_target_properties(steam_api PROPERTIES
    IMPORTED_IMPLIB ${STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.lib
    IMPORTED_LOCATION ${STEAM_API_DLL}
    INTERFACE_INCLUDE_DIRECTORIES ${STEAMWORKS_SDK_DIR}/public)  # SYSTEM: no /W4 noise from the SDK
  target_sources(tmntnet_common PRIVATE src/common/steam_real.cpp src/common/lobby_steam.cpp)
  target_compile_definitions(tmntnet_common PUBLIC TMNTNET_STEAM=1)
  # SteamAPI_InitEx (SDK 1.58+) reports why init failed; older SDKs only have SteamAPI_Init.
  file(STRINGS ${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h STEAM_INITEX REGEX "SteamAPI_InitEx")
  if(STEAM_INITEX)
    target_compile_definitions(tmntnet_common PRIVATE TMNTNET_STEAM_INITEX=1)
  endif()
  target_link_libraries(tmntnet_common PUBLIC steam_api delayimp)
else()
  target_sources(tmntnet_common PRIVATE src/common/steam_stub.cpp)
endif()
get_filename_component(MSVC_BIN_DIR ${CMAKE_LINKER} DIRECTORY)
find_program(DUMPBIN dumpbin HINTS ${MSVC_BIN_DIR})

# tmntnet.asi: loaded into tmnt.exe by Ultimate ASI Loader (or src/loader shim)
add_library(tmntnet MODULE
  src/mod/dllmain.cpp
  src/mod/dinput_hook.cpp
  src/mod/p2_binds.cpp
  src/mod/net_host.cpp
  src/mod/patch.cpp
  src/mod/widescreen.cpp
  src/mod/csel.cpp
  src/mod/overlay.cpp
  src/mod/display.cpp
  src/mod/ambient.cpp
  src/mod/audio.cpp
  src/mod/binds.cpp
  src/mod/settings_ui.cpp
  src/mod/game_options.cpp
  src/mod/autosave.cpp
  src/mod/game_text.cpp
  src/mod/native_menu.cpp
  src/mod/net_pages.cpp
  src/mod/mod_pages.cpp
  src/mod/option_pages.cpp
  src/mod/keyconf.cpp
  src/mod/lockstep.cpp
  src/mod/net_peer.cpp
  src/mod/ls_guest.cpp
  src/mod/map_turtles.cpp
  src/mod/nametags.cpp
  src/mod/four_players.cpp
  src/mod/four_hud.cpp
  src/mod/revive.cpp
  src/mod/csel_art.cpp
  src/mod/voice_sync.cpp
  src/mod/quality.cpp
  src/mod/load_guard.cpp
  src/mod/host_menus.cpp
  src/mod/net_pause.cpp
  src/mod/level_marks.cpp
  src/mod/costume.cpp
  src/mod/own_camera.cpp
  src/mod/story_menu.cpp
  src/mod/ui.cpp
  src/mod/main_menu.cpp)
target_link_libraries(tmntnet PRIVATE tmntnet_common minhook imgui dxguid windowscodecs ole32)
set_target_properties(tmntnet PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "tmntnet")

# dinput8.dll: fallback loader shim, only for setups where ASI Loader conflicts
add_library(dinput8_shim MODULE
  src/loader/dinput8_shim.cpp
  src/loader/dinput8.def)
target_compile_definitions(dinput8_shim PRIVATE ${OUR_DEFS})
set_target_properties(dinput8_shim PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")

# tmntnet-client.exe: remote P2
# A window app (no console flashing up) that keeps main(): --nogui attaches to
# the parent console for scripts and tests.
add_executable(tmntnet_client WIN32 src/client/main.cpp src/client/core.cpp src/client/gui.cpp)
target_link_libraries(tmntnet_client PRIVATE tmntnet_common imgui SDL2::SDL2-static winmm d3d9)
target_link_options(tmntnet_client PRIVATE /ENTRY:mainCRTStartup)
set_target_properties(tmntnet_client PROPERTIES OUTPUT_NAME "tmntnet-client")

# Delay-loaded: without steam_api.dll (or with Steam off) the mod and the
# client still start and fall back to ENet.
if(TMNTNET_STEAM)
  target_link_options(tmntnet PRIVATE /DELAYLOAD:steam_api.dll)
  target_link_options(tmntnet_client PRIVATE /DELAYLOAD:steam_api.dll)
endif()

# --- tests: stand-in tmnt.exe that loads the real mod (no game needed) -------
# build/tests/<config>/tmnt.exe + tmntnet.asi, client/ = test client.
# Run tests/run_integration.ps1.
add_executable(fake_game tests/fake_game.cpp)
target_compile_definitions(fake_game PRIVATE ${OUR_DEFS})
target_link_libraries(fake_game PRIVATE dinput8 dxguid)
set_target_properties(fake_game PROPERTIES OUTPUT_NAME "tmnt"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_dependencies(fake_game tmntnet tmntnet_client)
add_custom_command(TARGET fake_game POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tmntnet> $<TARGET_FILE_DIR:fake_game>/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/tests/fake_game.ini $<TARGET_FILE_DIR:fake_game>/tmntnet.ini
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:fake_game>/client
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tmntnet_client> $<TARGET_FILE_DIR:fake_game>/client/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/tests/client_test.ini $<TARGET_FILE_DIR:fake_game>/client/tmntnet.ini)

# Off-screen render of the in-game screens to PNG: ui_preview.exe <folder>
add_executable(ui_preview tests/ui_preview.cpp src/mod/ui.cpp src/mod/patch.cpp)
target_link_libraries(ui_preview PRIVATE tmntnet_common minhook imgui d3d9)
set_target_properties(ui_preview PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

# --- dist: everything a player needs in build/dist/<config> -----------------
# Ultimate ASI Loader is built separately (tools/build_asi_loader.ps1) and
# shipped as winmm.dll: tmnt.exe imports winmm by name at startup. Not as
# dsound.dll: the game imports DirectSoundCreate by ordinal 1, which the loader
# assigns to another function.
set(UAL_DLL ${CMAKE_SOURCE_DIR}/third_party/ultimate-asi-loader/bin/Win32/Release/dinput8.dll)
foreach(t tmntnet tmntnet_client)
  add_custom_command(TARGET ${t} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/$<CONFIG>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${t}> ${CMAKE_BINARY_DIR}/dist/$<CONFIG>/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/tmntnet.ini ${CMAKE_BINARY_DIR}/dist/$<CONFIG>/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/README.md ${CMAKE_BINARY_DIR}/dist/$<CONFIG>/)
endforeach()
add_custom_command(TARGET tmntnet POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DSRC=${UAL_DLL} -DDST=${CMAKE_BINARY_DIR}/dist/$<CONFIG>/winmm.dll
          -P ${CMAKE_SOURCE_DIR}/tools/copy_if_exists.cmake)
if(TMNTNET_STEAM)
  # steam_api.dll goes next to tmnt.exe / tmntnet-client.exe; it must be the
  # 32-bit one (dumpbin: 14C machine).
  foreach(t tmntnet tmntnet_client)
    add_custom_command(TARGET ${t} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${STEAM_API_DLL} ${CMAKE_BINARY_DIR}/dist/$<CONFIG>/
      COMMAND ${CMAKE_COMMAND} -DDUMPBIN=${DUMPBIN} -DFILE=${CMAKE_BINARY_DIR}/dist/$<CONFIG>/steam_api.dll
              -P ${CMAKE_SOURCE_DIR}/tools/check_x86.cmake)
  endforeach()
endif()
# --- tmntnet-setup.exe: one-file installer/uninstaller with everything inside --
if(EXISTS ${UAL_DLL})
  set(PAYLOAD_DIR ${CMAKE_BINARY_DIR}/payload)
  set(PAYLOAD_FILES ${PAYLOAD_DIR}/winmm.dll ${PAYLOAD_DIR}/tmntnet.asi ${PAYLOAD_DIR}/tmntnet.ini
                    ${PAYLOAD_DIR}/tmntnet-client.exe ${PAYLOAD_DIR}/README.md)
  set(PAYLOAD_STEAM_RC "")
  set(PAYLOAD_STEAM_COPY "")
  if(TMNTNET_STEAM)
    list(APPEND PAYLOAD_FILES ${PAYLOAD_DIR}/steam_api.dll)
    set(PAYLOAD_STEAM_RC "RES_STEAM RCDATA \"${PAYLOAD_DIR}/steam_api.dll\"")
    set(PAYLOAD_STEAM_COPY COMMAND ${CMAKE_COMMAND} -E copy_if_different ${STEAM_API_DLL} ${PAYLOAD_DIR}/)
  endif()
  add_custom_target(setup_payload
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PAYLOAD_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UAL_DLL} ${PAYLOAD_DIR}/winmm.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tmntnet> ${PAYLOAD_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tmntnet_client> ${PAYLOAD_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/tmntnet.ini ${PAYLOAD_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/README.md ${PAYLOAD_DIR}/
    ${PAYLOAD_STEAM_COPY}
    BYPRODUCTS ${PAYLOAD_FILES})
  add_dependencies(setup_payload tmntnet tmntnet_client)
  configure_file(src/setup/payload.rc.in ${CMAKE_BINARY_DIR}/payload.rc @ONLY)
  set_source_files_properties(${CMAKE_BINARY_DIR}/payload.rc PROPERTIES OBJECT_DEPENDS "${PAYLOAD_FILES}")

  add_executable(tmntnet_setup WIN32 src/setup/setup.cpp ${CMAKE_BINARY_DIR}/payload.rc)
  target_include_directories(tmntnet_setup PRIVATE src)
  target_compile_definitions(tmntnet_setup PRIVATE ${OUR_DEFS})
  target_compile_options(tmntnet_setup PRIVATE ${OUR_OPTS})
  target_link_libraries(tmntnet_setup PRIVATE comctl32 ole32 shell32 advapi32)
  target_link_options(tmntnet_setup PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
  set_target_properties(tmntnet_setup PROPERTIES OUTPUT_NAME "tmntnet-setup")
  add_dependencies(tmntnet_setup setup_payload)
  add_custom_command(TARGET tmntnet_setup POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tmntnet_setup> ${CMAKE_BINARY_DIR}/dist/$<CONFIG>/)
else()
  message(WARNING "Ultimate ASI Loader not built: tmntnet-setup.exe is skipped (tools/build_asi_loader.ps1)")
endif()

add_custom_command(TARGET dinput8_shim POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/$<CONFIG>/alt-loader
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:dinput8_shim> ${CMAKE_BINARY_DIR}/dist/$<CONFIG>/alt-loader/)
