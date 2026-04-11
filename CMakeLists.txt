cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(LlamaWars C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)


# Raylib
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.0
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(raylib)

# Lua 5.4 — official repo has no CMakeLists.txt, so we build it manually.
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.7
)
cmake_policy(SET CMP0169 OLD)
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
    file(GLOB LUA_SOURCES "${lua_SOURCE_DIR}/*.c")
    list(FILTER LUA_SOURCES EXCLUDE REGEX "/lua\\.c$")
    list(FILTER LUA_SOURCES EXCLUDE REGEX "/luac\\.c$")
    list(FILTER LUA_SOURCES EXCLUDE REGEX "/onelua\\.c$")
    add_library(lua_static STATIC ${LUA_SOURCES})
    target_include_directories(lua_static PUBLIC "${lua_SOURCE_DIR}")
    if(UNIX)
        target_compile_definitions(lua_static PRIVATE LUA_USE_POSIX)
    endif()
endif()

# raygui — single-header immediate-mode GUI, compatible with raylib 5.0
FetchContent_Declare(
    raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG        4.0
)
FetchContent_MakeAvailable(raygui)

find_package(Threads REQUIRED)

add_executable(llama-wars
    src/main.c
    src/fx.c
    src/update.c
    src/scripting.c
    src/walls.c
    llm/llama_client.c
    llm/llama_bot.c
)

target_include_directories(llama-wars PRIVATE
    src
    "${raygui_SOURCE_DIR}/src"
    llm
)
target_link_libraries(llama-wars PRIVATE
    raylib
    lua_static
    Threads::Threads
)

if(UNIX)
    target_link_libraries(llama-wars PRIVATE m)
endif()

# Copy bot scripts next to the executable so they are found at runtime
add_custom_command(TARGET llama-wars POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/scripts"
        "$<TARGET_FILE_DIR:llama-wars>/scripts"
    COMMENT "Copying bot scripts to build directory"
)
