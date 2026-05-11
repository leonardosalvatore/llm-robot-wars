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

# Python 3 — use system installation
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
message(STATUS "Python3: ${Python3_VERSION}, include: ${Python3_INCLUDE_DIRS}, libs: ${Python3_LIBRARIES}")

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
    src/colors.c
    src/lighting.c
    llm/llama_client.c
    llm/llama_bot.c
)

target_include_directories(llama-wars PRIVATE
    src
    llm
    ${Python3_INCLUDE_DIRS}
)
target_include_directories(llama-wars SYSTEM PRIVATE
    "${raygui_SOURCE_DIR}/src"
)
target_link_libraries(llama-wars PRIVATE
    raylib
    ${Python3_LIBRARIES}
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

# Copy default config if not already present in build dir
add_custom_command(TARGET llama-wars POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/llama-wars.cfg"
        "$<TARGET_FILE_DIR:llama-wars>/llama-wars.cfg"
    COMMENT "Copying default config to build directory"
)

# Copy default colors config
add_custom_command(TARGET llama-wars POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/colors.cfg"
        "$<TARGET_FILE_DIR:llama-wars>/colors.cfg"
    COMMENT "Copying colors config to build directory"
)
