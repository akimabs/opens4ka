cmake_minimum_required(VERSION 3.22)
project(OpenS4kaXboxRuntime LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SDK "${REPO}/sdk")                          # XboxXBE-Recomp's runtime, vendored
set(NATIVE "${CMAKE_CURRENT_LIST_DIR}/host")    # window, Vulkan, input, audio
set(GAME_SRC "${CMAKE_CURRENT_LIST_DIR}/game")  # D3D 8, DirectSound, settings, and each game's own

# The host is shared by every game; each brings its recompiled image (generated/<game>, from assets/<game>.xbe and
# config/<game>.toml) and runtime/game/games/<game>/. Build each into its own directory: runtime/build/<game>.
set(OPENS4KA_GAME "mk" CACHE STRING "Game runtime to build: mk, digimon or nfs-mw")
set_property(CACHE OPENS4KA_GAME PROPERTY STRINGS mk digimon nfs-mw)
if(OPENS4KA_GAME STREQUAL "digimon")
    set(RUNTIME_TARGET OpenS4kaDigimon)
    set(GAME_DEFINE OPENS4KA_DIGIMON)
elseif(OPENS4KA_GAME STREQUAL "nfs-mw")
    set(RUNTIME_TARGET OpenS4kaNFSMW)
    set(GAME_DEFINE OPENS4KA_NFSMW)
elseif(OPENS4KA_GAME STREQUAL "mk")
    set(RUNTIME_TARGET OpenS4kaXbox)
    set(GAME_DEFINE OPENS4KA_MORTAL_KOMBAT)
else()
    message(FATAL_ERROR "OPENS4KA_GAME must be mk, digimon or nfs-mw")
endif()
set(GAME "${GAME_SRC}/games/${OPENS4KA_GAME}")
set(GEN "${REPO}/generated/${OPENS4KA_GAME}")
set(XBE "${REPO}/assets/${OPENS4KA_GAME}.xbe")
set(GAME_CONFIG "${REPO}/config/${OPENS4KA_GAME}.toml")

if(NOT EXISTS "${GEN}/x86_func_mapping.cpp" OR NOT EXISTS "${XBE}" OR NOT EXISTS "${GAME_CONFIG}")
    message(FATAL_ERROR "${OPENS4KA_GAME}: generated/${OPENS4KA_GAME}, assets/${OPENS4KA_GAME}.xbe or config are missing "
                        "(scripts/recompile.sh ${OPENS4KA_GAME})")
endif()

file(STRINGS "${GAME_CONFIG}" GAME_PARTITIONS REGEX "^partitions *=" )
string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
math(EXPR LAST_PARTITION "${GAME_PARTITIONS} - 1")
set(GEN_SOURCES "${GEN}/x86_func_mapping.cpp")
foreach(i RANGE ${LAST_PARTITION})
    list(APPEND GEN_SOURCES "${GEN}/x86_recomp.${i}.cpp")
endforeach()

file(SHA1 "${XBE}" XBE_SHA1)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp"
     CONTENT "extern const char kImageSha1[] = \"${XBE_SHA1}\";\n")
set(BUILD_COMMIT "macos")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/build_info.h"
     CONTENT "#define BUILD_COMMIT \"${BUILD_COMMIT}\"\n")

include(FetchContent)
# third_party/<name> when it is there (a checkout of the same tag), else FetchContent downloads it
foreach(dep glslang tomlplusplus imgui)
    string(TOUPPER ${dep} DEP)
    if(EXISTS "${REPO}/third_party/${dep}")
        set(FETCHCONTENT_SOURCE_DIR_${DEP} "${REPO}/third_party/${dep}" CACHE PATH "" FORCE)
    endif()
endforeach()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
if(EXISTS "${REPO}/third_party/SDL/CMakeLists.txt")
    add_subdirectory("${REPO}/third_party/SDL" "${CMAKE_CURRENT_BINARY_DIR}/SDL" EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE
                         EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3)
endif()

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 16.0.0 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glslang tomlplusplus imgui)

find_package(Vulkan REQUIRED)

file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS "${SDK}/runtime/*/*.cpp")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX
     "/runtime/app/|/core/input\\.cpp$|/gpu/(d3d12|render_thread)\\.cpp$|/audio/xaudio2\\.cpp$|/xdk/(dsound|d3d8)\\.cpp$")
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS
     "${GAME_SRC}/common/*.cpp"
     "${GAME_SRC}/d3d/*.cpp"
     "${GAME_SRC}/sound/*.cpp"
     "${GAME}/*.cpp")
file(GLOB NATIVE_SOURCES CONFIGURE_DEPENDS "${NATIVE}/*.cpp")
list(APPEND NATIVE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/main.cpp" "${CMAKE_CURRENT_LIST_DIR}/update.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/macos_window.mm")

set(IMGUI_SOURCES
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")

add_executable(${RUNTIME_TARGET} ${RUNTIME_SOURCES} ${GAME_SOURCES} ${NATIVE_SOURCES} ${GEN_SOURCES}
            ${IMGUI_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp")

target_include_directories(${RUNTIME_TARGET} PRIVATE
    "${NATIVE}/compat"
    "${SDK}/harness/compat"
    "${NATIVE}"
    "${GAME_SRC}"
    "${SDK}/runtime"
    "${GEN}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${glslang_SOURCE_DIR}")
target_include_directories(${RUNTIME_TARGET} SYSTEM PRIVATE "${imgui_SOURCE_DIR}")
target_compile_definitions(${RUNTIME_TARGET} PRIVATE OPENS4KA_MACOS ${GAME_DEFINE})
target_compile_options(${RUNTIME_TARGET} PRIVATE -fno-strict-aliasing -fsigned-char)
set(HOST_SOURCES ${RUNTIME_SOURCES} ${GAME_SOURCES} ${NATIVE_SOURCES})
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "-include;${NATIVE}/compat/crt.h")
# the host's verbose log (draw traces, frame stats) without recompiling the generated code
option(OPENS4KA_VERBOSE_LOG "Verbose host log" OFF)
if(OPENS4KA_VERBOSE_LOG)
    set_property(SOURCE ${HOST_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS XBE_VERBOSE_LOG)
endif()
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-w;-fno-strict-aliasing;-fsigned-char")
set_source_files_properties("${NATIVE}/vk_shader.cpp" PROPERTIES COMPILE_DEFINITIONS ENABLE_HLSL)

target_link_libraries(${RUNTIME_TARGET} PRIVATE
    SDL3::SDL3-static
    tomlplusplus::tomlplusplus
    glslang
    glslang-default-resource-limits
    Vulkan::Vulkan)
if(APPLE)
    target_link_libraries(${RUNTIME_TARGET} PRIVATE "-framework AppKit")
endif()
set_target_properties(${RUNTIME_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
    OUTPUT_NAME "${RUNTIME_TARGET}")
