cmake_minimum_required(VERSION 3.20)
project(SSNERecomp LANGUAGES C CXX)
set(SSNE_VERSION "1.2.8.1")  # release version (the launcher's self-update compares it with the latest GitHub release)

set(SSNE_UPDATE_REPOSITORY "Master290600/SSNE-Native-Multiplayer" CACHE STRING "GitHub owner/repository for launcher releases")
if(NOT SSNE_UPDATE_REPOSITORY MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
  message(FATAL_ERROR "SSNE_UPDATE_REPOSITORY must be a GitHub owner/repository")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MINGW)
  add_link_options(-static)
endif()

option(SSNE_BUILD_LAUNCHER "Build the SDL2/ImGui launcher" ON)
option(SSNE_BUILD_PORT     "Build the PC port executable"   ON)
# Tests run the freshly built translator during the build, which can't work when cross-compiling (e.g. Windows
# binaries from Linux), so they default to off then.
if(CMAKE_CROSSCOMPILING)
  option(SSNE_BUILD_TESTS  "Build tests"                    OFF)
else()
  option(SSNE_BUILD_TESTS  "Build tests"                    ON)
endif()
# Directory produced by `SSNE-RECOOP-Native` (see docs/BUILDING.md). If empty, the port builds without
# game code and only shows a status screen.
set(SSNE_RECOMPILED_DIR "" CACHE PATH "Directory holding recompiled game code (funcs_*.cpp)")

if(MSVC)
  add_compile_options(/W3 /utf-8 /bigobj)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wno-unused-label -Wno-unused-variable)
endif()

# ---------------------------------------------------------------- core disc library
add_library(gc STATIC
  libgc/src/sha1.cpp libgc/src/disc.cpp libgc/src/dol.cpp libgc/src/verify.cpp
  libgc/src/blob.cpp libgc/src/decomp_inflate.cpp libgc/src/decomp_zstd.cpp libgc/src/decomp_lzma.cpp
  libgc/src/decomp_bzip2.cpp libgc/src/sha256.cpp libgc/src/zip.cpp
  libgc/src/custom_levels.cpp launcher/src/launcher_json.cpp)
target_include_directories(gc PUBLIC libgc/include)

add_executable(gctool tools/gctool.cpp)
target_link_libraries(gctool PRIVATE gc)

# ---------------------------------------------------------------- recompiler (offline tool)
add_executable(ssne-recomp
  recomp/src/main.cpp recomp/src/ppc_decode.cpp recomp/src/analysis.cpp recomp/src/codegen.cpp)
target_include_directories(ssne-recomp PRIVATE recomp/src)
target_link_libraries(ssne-recomp PRIVATE gc)

add_executable(ssne-symbols recomp/src/symbols_main.cpp recomp/src/symbols.cpp recomp/src/analysis.cpp
  recomp/src/ppc_decode.cpp)
target_include_directories(ssne-symbols PRIVATE recomp/src)
target_link_libraries(ssne-symbols PRIVATE gc)
set_target_properties(ssne-symbols PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------- runtime (linked into the port)
add_library(ssne-runtime STATIC runtime/src/memory.cpp runtime/src/runtime.cpp runtime/src/hle.cpp
  runtime/src/hw.cpp runtime/src/custom_level_fst.cpp runtime/src/custom_level_mount.cpp runtime/src/audio.cpp runtime/src/exi.cpp runtime/src/scheduler.cpp runtime/src/gx/gx_fifo.cpp runtime/src/gx/gx_vertex.cpp
  runtime/src/gx/gx_texture.cpp runtime/src/gx/gx_crosshair.cpp runtime/src/gx/gx_raster.cpp runtime/src/gx/gx_gpu_record.cpp runtime/src/gx/gx_gpu.cpp
  runtime/src/gx/gx_gpu_capture.cpp runtime/src/gx/gx_prompts.cpp runtime/src/savestate.cpp
  runtime/src/thread_util.cpp runtime/src/si.cpp runtime/src/mods.cpp runtime/src/ammo_rules.cpp
  runtime/src/native_party.cpp runtime/src/native_entity.cpp runtime/src/native_effect.cpp runtime/src/replica_simulation.cpp
  runtime/src/native_results.cpp runtime/src/native_cutscene.cpp runtime/src/native_music.cpp runtime/src/input_glyphs.cpp
  runtime/src/native_survival.cpp runtime/src/native_spectator.cpp runtime/src/native_session_hud.cpp
  runtime/src/native_boss_hud.cpp)
add_library(ssne-native-collision STATIC runtime/src/native_collision_module.cpp)
target_include_directories(ssne-native-collision PUBLIC runtime/include)
target_link_libraries(ssne-runtime PUBLIC ssne-native-collision)
find_package(Threads REQUIRED)
target_link_libraries(ssne-runtime PUBLIC Threads::Threads)
target_include_directories(ssne-runtime PUBLIC runtime/include)
target_link_libraries(ssne-runtime PUBLIC gc)
add_executable(custom_level_fst_test EXCLUDE_FROM_ALL tests/custom_level_fst_test.cpp runtime/src/custom_level_fst.cpp)
target_include_directories(custom_level_fst_test PRIVATE runtime/include libgc/include)

# Profile-guided optimization of the runtime (clang). Only the runtime is instrumented / optimized: it is shipped
# prebuilt, while the game code is compiled on the player's PC. tools/make_pgo_profile.sh trains
# data/pgo/runtime.proftext; the release scripts convert it for their compiler (tools/pgo_build_profile.sh).
#   SSNE_PGO_GENERATE=ON       instrumented runtime; running the game writes .profraw files (LLVM_PROFILE_FILE)
#   SSNE_PGO_PROFILE=<file>    runtime optimized with a .profdata matching this compiler
option(SSNE_PGO_GENERATE "Instrument the runtime for profile-guided optimization (clang)" OFF)
set(SSNE_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata to optimize the runtime with (clang)")
if((SSNE_PGO_GENERATE OR SSNE_PGO_PROFILE) AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "SSNE_PGO_GENERATE / SSNE_PGO_PROFILE need clang; building without profile-guided optimization")
elseif(SSNE_PGO_GENERATE)
  target_compile_options(ssne-runtime PRIVATE -fprofile-instr-generate)
  target_link_options(ssne-runtime INTERFACE -fprofile-instr-generate)
elseif(SSNE_PGO_PROFILE)
  target_compile_options(ssne-runtime PRIVATE "-fprofile-instr-use=${SSNE_PGO_PROFILE}"
                         -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin)
endif()

# GPU renderer regression/benchmark tool: replays frames captured with SSNE_GPU_CAPTURE (headless EGL, Linux).
if(UNIX AND NOT APPLE)
  add_executable(gpu-replay tools/gpu_replay.cpp port/src/no_game_code.cpp)
  target_link_libraries(gpu-replay PRIVATE ssne-runtime ${CMAKE_DL_LIBS})
  set_target_properties(gpu-replay PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Authoritative cooperative session/state core; the native engine adapter is in progress.
add_library(ssne-coop STATIC port/src/coop/session_authority.cpp port/src/coop/world_state.cpp
  port/src/coop/survival.cpp port/src/coop/spectator.cpp port/src/coop/custom_map_transfer.cpp)
if(WIN32)
  target_sources(ssne-coop PRIVATE port/src/coop/lan_discovery.cpp)
endif()
target_include_directories(ssne-coop PUBLIC port/src/coop runtime/include)
target_link_libraries(ssne-coop PUBLIC Threads::Threads gc)
if(WIN32)
  target_sources(ssne-coop PRIVATE port/src/coop/authoritative_session.cpp)
  target_link_libraries(ssne-coop PUBLIC ws2_32 bcrypt iphlpapi)
  include(port/src/coop/invite_network.cmake)
  add_executable(custom_map_transfer_test EXCLUDE_FROM_ALL tests/custom_map_transfer_test.cpp)
  target_link_libraries(custom_map_transfer_test PRIVATE ssne-coop)
endif()


# ---------------------------------------------------------------- third-party (fetched)
if(SSNE_BUILD_LAUNCHER OR SSNE_BUILD_PORT OR SSNE_BUILD_TESTS)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  # Linux: 2.30.9 no longer builds against current PipeWire headers; 2.32.10 does. Windows keeps the pinned release.
  if(WIN32)
    set(SSNE_SDL2_TAG release-2.30.9)
  else()
    set(SSNE_SDL2_TAG release-2.32.10)
  endif()
  FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
                            GIT_TAG ${SSNE_SDL2_TAG} GIT_SHALLOW TRUE)
  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git
                             GIT_TAG v1.91.5 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL2 imgui)
  find_package(OpenGL REQUIRED)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC SDL2::SDL2-static OpenGL::GL)
endif()

if(SSNE_BUILD_LAUNCHER)
  add_executable(ssne-launcher WIN32
    launcher/src/main.cpp launcher/src/launcher_app.cpp launcher/src/platform.cpp launcher/src/config.cpp
    launcher/src/theme.cpp launcher/src/game_builder.cpp launcher/src/updater.cpp
    launcher/src/launcher_json.cpp launcher/src/runtime_kit.cpp launcher/src/translation_patch.cpp
    launcher/src/map_document.cpp launcher/src/map_structure.cpp launcher/src/map_import.cpp launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/map_logic.cpp launcher/src/map_editor_logic.cpp launcher/src/map_editor_links.cpp launcher/src/map_editor_custom.cpp libgc/src/custom_levels.cpp port/src/crosshair_image.cpp port/src/png_write.cpp launcher/src/map_editor.cpp launcher/src/map_editor_structure.cpp launcher/src/map_editor_3d.cpp launcher/src/map_scene.cpp launcher/src/map_gl.cpp launcher/src/map_appearance.cpp launcher/src/map_model_pose.cpp)
  target_link_libraries(ssne-launcher PRIVATE gc imgui SDL2::SDL2main)
  add_executable(custom_levels_test EXCLUDE_FROM_ALL tests/custom_levels_test.cpp libgc/src/custom_levels.cpp launcher/src/launcher_json.cpp)
  target_include_directories(custom_levels_test PRIVATE libgc/include)
  target_link_libraries(custom_levels_test PRIVATE gc)
  add_executable(custom_level_session_test EXCLUDE_FROM_ALL tests/custom_level_session_test.cpp port/src/coop/custom_levels_runtime.cpp libgc/src/custom_levels.cpp launcher/src/launcher_json.cpp)
  target_include_directories(custom_level_session_test PRIVATE libgc/include port/src/coop runtime/include)
  target_link_libraries(custom_level_session_test PRIVATE gc ssne-coop)
  add_executable(map_links_test EXCLUDE_FROM_ALL tests/map_links_test.cpp launcher/src/map_logic.cpp launcher/src/map_document.cpp launcher/src/map_structure.cpp launcher/src/map_import.cpp launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/launcher_json.cpp)
  target_include_directories(map_links_test PRIVATE launcher/src)
  target_link_libraries(map_links_test PRIVATE gc)
  add_executable(map_logic_test EXCLUDE_FROM_ALL tests/map_logic_test.cpp launcher/src/map_logic.cpp launcher/src/map_document.cpp launcher/src/map_structure.cpp launcher/src/map_import.cpp launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/launcher_json.cpp)
  target_include_directories(map_logic_test PRIVATE launcher/src)
  target_link_libraries(map_logic_test PRIVATE gc)
  add_executable(map_waves_test EXCLUDE_FROM_ALL tests/map_waves_test.cpp launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/map_import.cpp launcher/src/map_structure.cpp launcher/src/map_document.cpp launcher/src/launcher_json.cpp)
  target_include_directories(map_waves_test PRIVATE launcher/src)
  target_link_libraries(map_waves_test PRIVATE gc)
  add_executable(map_document_test EXCLUDE_FROM_ALL tests/map_document_test.cpp launcher/src/map_document.cpp launcher/src/map_structure.cpp launcher/src/map_import.cpp launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/launcher_json.cpp)
  target_include_directories(map_document_test PRIVATE launcher/src)
  target_link_libraries(map_document_test PRIVATE gc)
  add_executable(map_import_test EXCLUDE_FROM_ALL tests/map_import_test.cpp launcher/src/map_import.cpp launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/map_structure.cpp launcher/src/map_document.cpp launcher/src/map_scene.cpp launcher/src/map_appearance.cpp launcher/src/map_model_pose.cpp launcher/src/launcher_json.cpp)
  target_include_directories(map_import_test PRIVATE launcher/src)
  target_link_libraries(map_import_test PRIVATE gc)
  add_executable(map_structure_test EXCLUDE_FROM_ALL tests/map_structure_test.cpp launcher/src/map_document.cpp launcher/src/map_structure.cpp launcher/src/map_import.cpp launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/launcher_json.cpp)
  target_include_directories(map_structure_test PRIVATE launcher/src)
  target_link_libraries(map_structure_test PRIVATE gc)
  add_executable(map_scene_test EXCLUDE_FROM_ALL tests/map_scene_test.cpp launcher/src/map_scene.cpp launcher/src/map_document.cpp launcher/src/map_structure.cpp launcher/src/map_import.cpp launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/launcher_json.cpp)
  target_include_directories(map_scene_test PRIVATE launcher/src)
  target_link_libraries(map_scene_test PRIVATE gc)
  add_executable(map_appearance_test EXCLUDE_FROM_ALL tests/map_appearance_test.cpp launcher/src/map_appearance.cpp launcher/src/map_model_pose.cpp launcher/src/map_scene.cpp launcher/src/map_document.cpp launcher/src/map_structure.cpp launcher/src/map_import.cpp launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/launcher_json.cpp)
  target_include_directories(map_appearance_test PRIVATE launcher/src)
  target_link_libraries(map_appearance_test PRIVATE gc)
  # Fonts (SIL OFL, see launcher/fonts/*-OFL.txt) are compiled into the launcher.
  set(EMBED_DIR ${CMAKE_BINARY_DIR}/embed)
  foreach(font EricaOne-Regular Outfit-Regular Outfit-Bold)
    string(REPLACE "-" "_" sym "font_${font}")
    add_custom_command(OUTPUT ${EMBED_DIR}/${sym}.h
      COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/launcher/fonts/${font}.ttf -DOUT=${EMBED_DIR}/${sym}.h
              -DNAME=${sym} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
      DEPENDS ${CMAKE_SOURCE_DIR}/launcher/fonts/${font}.ttf ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)
    target_sources(ssne-launcher PRIVATE ${EMBED_DIR}/${sym}.h)
  endforeach()
  target_include_directories(ssne-launcher PRIVATE ${EMBED_DIR})
  target_compile_definitions(ssne-launcher PRIVATE IMGUI_DEFINE_MATH_OPERATORS SSNE_VERSION="${SSNE_VERSION}" SSNE_UPDATE_REPOSITORY="${SSNE_UPDATE_REPOSITORY}")
  set_target_properties(ssne-launcher PROPERTIES OUTPUT_NAME "SSNE-RECOOP-Native")
  if(WIN32)
    enable_language(RC)
    add_library(ssne-launcher-resource OBJECT launcher/src/launcher.rc)
    set_property(TARGET ssne-launcher-resource PROPERTY COMPILE_OPTIONS "")
    string(REPLACE "." "," SSNE_FILEVERSION "${SSNE_VERSION}")
    target_compile_definitions(ssne-launcher-resource PRIVATE SSNE_FILEVERSION=${SSNE_FILEVERSION} SSNE_RESOURCE_VERSION=${SSNE_VERSION})
    target_include_directories(ssne-launcher-resource PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/port/resources")
    set_property(SOURCE launcher/src/launcher.rc PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/port/resources/game.ico")
    target_sources(ssne-launcher PRIVATE $<TARGET_OBJECTS:ssne-launcher-resource>)
    target_link_libraries(ssne-launcher PRIVATE comdlg32 shell32 winhttp)
  endif()
endif()

if(SSNE_BUILD_PORT)
  set(PORT_SOURCES port/src/coop/custom_map_session.cpp libgc/src/custom_levels.cpp launcher/src/launcher_json.cpp port/src/coop/custom_levels_runtime.cpp port/src/coop/custom_level_menu.cpp port/src/chat_overlay.cpp port/src/tactical_ping.cpp port/src/crosshair_image.cpp port/src/crosshair_renderer.cpp port/src/native_crosshair.cpp port/src/native_menu_carousel.cpp port/src/native_menu_mouse.cpp port/src/main.cpp port/src/settings.cpp port/src/settings_menu.cpp port/src/mod_menu.cpp port/src/png_write.cpp port/src/netplay.cpp
    port/src/coop/editor_logic_debug.cpp port/src/coop/native_lobby.cpp port/src/coop/native_frontend.cpp port/src/coop/native_world.cpp port/src/coop/native_configuration.cpp port/src/coop/native_replica.cpp
    port/src/coop/native_lifecycle.cpp port/src/coop/native_bootstrap.cpp port/src/coop/native_session.cpp)
  if(SSNE_RECOMPILED_DIR)
    file(GLOB RECOMPILED_SOURCES CONFIGURE_DEPENDS ${SSNE_RECOMPILED_DIR}/*.cpp)
    list(APPEND PORT_SOURCES ${RECOMPILED_SOURCES})
    if(NOT MSVC)
      # Generated functions are huge; -O2 keeps build times sane with nearly the same speed as -O3.
      set_source_files_properties(${RECOMPILED_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-w")
    endif()
  else()
    list(APPEND PORT_SOURCES port/src/no_game_code.cpp)
  endif()
  add_executable(ssne-port WIN32 ${PORT_SOURCES})
  if(WIN32)
    enable_language(RC)
    add_library(ssne-game-icon OBJECT port/resources/game.rc)
    # The resource compiler does not take the C/C++ compiler's warning flags.
    set_property(TARGET ssne-game-icon PROPERTY COMPILE_OPTIONS "")
    target_include_directories(ssne-game-icon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/port/resources")
    set_property(SOURCE port/resources/game.rc PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/port/resources/game.ico")
    target_sources(ssne-port PRIVATE $<TARGET_OBJECTS:ssne-game-icon>)
  endif()
  if(SSNE_RECOMPILED_DIR)
    target_include_directories(ssne-port PRIVATE ${SSNE_RECOMPILED_DIR})
    # Headless runner for bring-up (shares the recompiled sources).
    add_executable(ssne-headless tools/headless.cpp ${RECOMPILED_SOURCES})
    target_include_directories(ssne-headless PRIVATE ${SSNE_RECOMPILED_DIR})
    target_link_libraries(ssne-headless PRIVATE ssne-runtime)
    set_target_properties(ssne-headless PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  endif()
  target_link_libraries(ssne-port PRIVATE ssne-runtime ssne-coop imgui SDL2::SDL2main)
  set_target_properties(ssne-port PROPERTIES OUTPUT_NAME "SSNE-Native-Multiplayer-Game")
  if(WIN32)
    add_custom_command(TARGET ssne-port POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ssne-port> $<TARGET_FILE_DIR:ssne-port>/SSNE-Native-Multiplayer.exe)
    # Keep the historical build target as an alias for the native game.
    add_custom_target(ssne-multiplayer-launcher DEPENDS ssne-port)
  endif()
  if(WIN32)
    target_link_libraries(ssne-port PRIVATE winmm ws2_32)
  endif()
endif()

# Launcher expects the port and data files next to it.
foreach(t ssne-launcher ssne-port)
  if(TARGET ${t})
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  endif()
endforeach()
set_target_properties(gctool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set_target_properties(ssne-recomp PROPERTIES
  OUTPUT_NAME "SSNE-RECOOP-Native"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/build-kit")
configure_file(data/known_dumps.txt ${CMAKE_BINARY_DIR}/bin/known_dumps.txt COPYONLY)

if(SSNE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Professional editor modules
foreach(t map_document_test map_structure_test map_import_test map_waves_test map_scene_test map_appearance_test map_logic_test map_links_test)
  if(TARGET ${t})
    target_sources(${t} PRIVATE launcher/src/map_logic_author.cpp)
  endif()
endforeach()
if(TARGET ssne-launcher)
  target_sources(ssne-launcher PRIVATE launcher/src/map_gizmo.cpp launcher/src/map_help.cpp launcher/src/map_editor_help.cpp launcher/src/map_editor_graph.cpp launcher/src/map_logic_author.cpp launcher/src/map_editor_author.cpp launcher/src/map_logic_icons.cpp launcher/src/map_logic_icon_catalog.cpp launcher/src/map_model_pick.cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libgc/src/editor_effect.cpp")
 target_sources(gc PRIVATE libgc/src/editor_effect.cpp)
endif()
add_executable(map_help_test tests/map_help_test.cpp launcher/src/map_help.cpp)
target_include_directories(map_help_test PRIVATE launcher/src)
add_executable(map_gizmo_test tests/map_gizmo_test.cpp launcher/src/map_gizmo.cpp launcher/src/map_scene.cpp)
target_include_directories(map_gizmo_test PRIVATE launcher/src libgc/include)
target_link_libraries(map_gizmo_test PRIVATE gc)
target_sources(ssne-runtime PRIVATE runtime/src/editor_logic_effects.cpp runtime/src/editor_logic_use.cpp runtime/src/editor_scene_collision.cpp)
if(TARGET ssne-port)
 target_sources(ssne-port PRIVATE port/src/coop/editor_collision_probe.cpp port/src/coop/native_particles.cpp)
endif()
foreach(t map_document_test map_structure_test map_import_test map_waves_test map_scene_test map_appearance_test)
 if(TARGET ${t})
  target_sources(${t} PRIVATE launcher/src/map_logic.cpp)
 endif()
endforeach()
foreach(t map_appearance_test map_import_test)
 if(TARGET ${t})
  target_sources(${t} PRIVATE launcher/src/map_model_pick.cpp)
 endif()
endforeach()

if(SSNE_BUILD_TESTS)
  add_library(map_professional_checks OBJECT
    launcher/src/map_document.cpp launcher/src/map_structure.cpp launcher/src/map_import.cpp
    launcher/src/map_waves.cpp launcher/src/map_links.cpp launcher/src/map_logic.cpp
    launcher/src/map_logic_author.cpp launcher/src/map_scene.cpp launcher/src/map_appearance.cpp
    launcher/src/map_model_pose.cpp launcher/src/map_model_pick.cpp launcher/src/launcher_json.cpp)
  target_include_directories(map_professional_checks PRIVATE launcher/src)
  target_link_libraries(map_professional_checks PRIVATE gc)
  foreach(kind transform logic_author logic_templates model_pick)
    add_executable(map_${kind}_test EXCLUDE_FROM_ALL tests/map_${kind}_test.cpp $<TARGET_OBJECTS:map_professional_checks>)
    target_include_directories(map_${kind}_test PRIVATE launcher/src)
    target_link_libraries(map_${kind}_test PRIVATE gc)
  endforeach()
  target_sources(map_transform_test PRIVATE launcher/src/map_gizmo.cpp)
  add_executable(map_logic_native_fixture EXCLUDE_FROM_ALL tests/map_logic_native_fixture.cpp $<TARGET_OBJECTS:map_professional_checks>)
  target_include_directories(map_logic_native_fixture PRIVATE launcher/src)
  target_link_libraries(map_logic_native_fixture PRIVATE gc)
  add_executable(editor_effect_test tests/editor_effect_test.cpp)
  target_link_libraries(editor_effect_test PRIVATE gc)
  add_executable(editor_logic_effects_test tests/editor_logic_effects_test.cpp runtime/src/editor_logic_effects.cpp)
  target_include_directories(editor_logic_effects_test PRIVATE runtime/include runtime/src)
  target_link_libraries(editor_logic_effects_test PRIVATE gc)
  add_executable(native_particles_test tests/native_particles_test.cpp port/src/coop/native_particles.cpp runtime/src/native_effect.cpp)
  target_include_directories(native_particles_test PRIVATE port/src/coop runtime/include runtime/src)
  target_compile_options(native_particles_test PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(native_particles_test PRIVATE -Wl,--gc-sections)
  add_executable(editor_logic_use_test tests/editor_logic_use_test.cpp runtime/src/editor_logic_use.cpp)
  target_include_directories(editor_logic_use_test PRIVATE runtime/include libgc/include)
  add_executable(editor_scene_collision_test tests/editor_scene_collision_test.cpp runtime/src/editor_scene_collision.cpp)
  target_include_directories(editor_scene_collision_test PRIVATE runtime/include)
  if(MINGW)
    target_link_options(editor_logic_use_test PRIVATE -static)
    target_link_options(editor_scene_collision_test PRIVATE -static)
  endif()
  add_executable(map_logic_icons_test tests/map_logic_icons_test.cpp launcher/src/map_logic_icon_catalog.cpp launcher/src/map_logic_icons.cpp)
  target_include_directories(map_logic_icons_test PRIVATE launcher/src)
  target_compile_definitions(map_logic_icons_test PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
  target_link_libraries(map_logic_icons_test PRIVATE imgui)
  add_test(NAME map_help COMMAND map_help_test)
  add_test(NAME map_gizmo COMMAND map_gizmo_test)
  add_test(NAME editor_effect_codec COMMAND editor_effect_test)
  add_test(NAME editor_logic_effects COMMAND editor_logic_effects_test)
  add_test(NAME native_particles COMMAND native_particles_test)
  add_test(NAME editor_logic_use COMMAND editor_logic_use_test)
  add_test(NAME editor_scene_collision COMMAND editor_scene_collision_test)
  add_test(NAME map_logic_icons COMMAND map_logic_icons_test)
endif()
