cmake_minimum_required(VERSION 3.16)
project(RenderCore)
set(CMAKE_CXX_STANDARD 20)

# Pour éviter les warnings MSVC
if (MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Récupération des fichiers du moteur
file(GLOB_RECURSE ENGINE_SOURCES
    "engine/src/*.cpp"
    "engine/src/*.h"
)
list(APPEND ENGINE_SOURCES
    libs/tinyfiledialogs/tinyfiledialogs.c
)

# ImGui - SEULEMENT les fichiers nécessaires
set(IMGUI_SOURCES
    libs/imgui/imgui.cpp
    libs/imgui/imgui_demo.cpp
    libs/imgui/imgui_draw.cpp
    libs/imgui/imgui_tables.cpp
    libs/imgui/imgui_widgets.cpp
    libs/imgui/backends/imgui_impl_sdl2.cpp
    libs/imgui/backends/imgui_impl_opengl3.cpp
)

# ImGuizmo
file(GLOB IMGUIZMO_SOURCES
    "libs/ImGuizmo/ImGuizmo.cpp"
    "libs/ImGuizmo/ImGuizmo.h"
)

# GLAD
set(GLAD_SOURCES libs/glad/src/glad.c)

# Include directories (recherche intelligente)
set(INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/src
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/glad/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ImGuizmo
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/SDL2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/lua/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/librw
    ${CMAKE_CURRENT_SOURCE_DIR}/libs
)

# Chercher nlohmann/json
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/nlohmann/json.hpp")
    list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/libs)
    message(STATUS "Found nlohmann/json at libs/nlohmann/json.hpp")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/json/include")
    list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/libs/json/include)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/json/single_include")
    list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/libs/json/single_include)
else()
    message(WARNING "nlohmann/json not found!")
endif()

# Chercher dirent
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/dirent/include")
    list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/libs/dirent/include)
else()
    message(WARNING "dirent.h not found! ImGuiFileDialog may fail to compile")
endif()

include_directories(${INCLUDE_DIRS})

# Link directories
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    link_directories(
        libs/SDL2/lib/x64
        libs/lua/lib
    )
else()
    link_directories(
        libs/SDL2/lib/x86
        libs/lua/lib
    )
endif()

# Assimp
set(ASSIMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libs/assimp")
set(ASSIMP_INCLUDE_DIR "${ASSIMP_ROOT}/install/include")
set(ASSIMP_LIB_DIR "${ASSIMP_ROOT}/install/lib")
set(ASSIMP_DLL "${ASSIMP_ROOT}/install/bin/assimp-vc143-mt.dll")

include_directories(${ASSIMP_INCLUDE_DIR})
link_directories(${ASSIMP_LIB_DIR})

add_subdirectory(libs/librw)

# ===================================
# EXÉCUTABLE PRINCIPAL (ÉDITEUR)
# ===================================
add_executable(SpicyGamesEngine
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
    ${IMGUIZMO_SOURCES}
    ${GLAD_SOURCES}
)

# OpenGL
find_package(OpenGL REQUIRED)
target_link_libraries(SpicyGamesEngine 
    ${OPENGL_LIBRARIES}
    SDL2
    SDL2main
    assimp
    librw
)

# Lua
if(EXISTS "${CMAKE_SOURCE_DIR}/libs/lua/lib")
    target_link_libraries(SpicyGamesEngine lua)
endif()

# ===================================
# RUNTIME PLAYER (STANDALONE)
# ===================================
add_executable(RenderCore_Runtime 
    runtime_player.cpp
    ${GLAD_SOURCES}
)

# Lier SDL2 pour le runtime
target_link_libraries(RenderCore_Runtime
    ${OPENGL_LIBRARIES}
    SDL2
    SDL2main
    assimp
    librw
)

# Lua pour runtime
if(EXISTS "${CMAKE_SOURCE_DIR}/libs/lua/lib")
    target_link_libraries(RenderCore_Runtime lua)
endif()

# Copier DLL Assimp pour les deux cibles
foreach(TARGET SpicyGamesEngine RenderCore_Runtime)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ASSIMP_DLL}"
        $<TARGET_FILE_DIR:${TARGET}>
    )
endforeach()

# Définitions Windows
if(WIN32)
    target_compile_definitions(SpicyGamesEngine PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_compile_definitions(RenderCore_Runtime PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

# Configuration Debug/Release
foreach(TARGET SpicyGamesEngine RenderCore_Runtime)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(${TARGET} PRIVATE DEBUG)
    else()
        target_compile_definitions(${TARGET} PRIVATE NDEBUG)
    endif()
endforeach()