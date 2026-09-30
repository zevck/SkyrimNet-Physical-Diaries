cmake_minimum_required(VERSION 3.21)

# Force static linking for all vcpkg dependencies
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "" FORCE)

project(
    SkyrimNetPhysicalDiaries
    VERSION 2.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(GNUInstallDirs)

# Output directly to mod folder for MO2
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Install prefix" FORCE)

# ---- Add CommonLibSSE-NG (lib/commonlibsse-ng submodule) ----
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/commonlibsse-ng ${CMAKE_CURRENT_BINARY_DIR}/CommonLibSSE-NG EXCLUDE_FROM_ALL)
# Expose add_commonlibsse_plugin() and friends
include(lib/commonlibsse-ng/cmake/CommonLibSSE.cmake)

# ---- Dependencies ----
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)

# ---- Sources ----
set(sources
    src/main.cpp
    src/ActorLookup.cpp
    src/BookCreation.cpp
    src/BookEditor.cpp
    src/BookManager.cpp
    src/BookText.cpp
    src/BookTextHook.cpp
    src/Database.cpp
    src/DiaryDB.cpp
    src/DiaryTheftHandler.cpp
    src/DynamicForms.cpp
    src/InterPluginAPI.cpp
    src/Localization.cpp
    src/PapyrusAPI.cpp
    src/SaveFolder.cpp
    src/Serialization.cpp
    src/TimelineGate.cpp
    src/VolumeSync.cpp
    src/WritingMode.cpp
)

# ---- Generate Plugin.h ----
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Plugin.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/Plugin.h
    @ONLY
)

# ---- Plugin Target ----
add_commonlibsse_plugin(${PROJECT_NAME}
    NAME    "SkyrimNetPhysicalDiaries"
    AUTHOR  "Zevick"
    USE_ADDRESS_LIBRARY
    SOURCES ${sources}
)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_precompile_headers(
    ${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h
)

target_compile_options(
    ${PROJECT_NAME}
    PRIVATE
        /utf-8
)

target_compile_definitions(
    ${PROJECT_NAME}
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
        minhook::minhook
)

# ---- Installation ----
install(
    FILES
        $<TARGET_FILE:${PROJECT_NAME}>
    DESTINATION
        "SKSE/Plugins"
)

install(
    FILES
        $<TARGET_PDB_FILE:${PROJECT_NAME}>
    DESTINATION
        "SKSE/Plugins"
    OPTIONAL
)
