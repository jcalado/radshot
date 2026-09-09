cmake_minimum_required(VERSION 3.16)
project(at168shot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch GLFW
include(FetchContent)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# Source files
set(SOURCES
    at168shot.cpp
    tinyfiledialogs.c
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_impl_glfw.cpp
    imgui/imgui_impl_opengl3.cpp
)

# Windows resource file
if(WIN32)
    list(APPEND SOURCES at168shot.rc)
endif()

add_executable(at168shot ${SOURCES})

target_include_directories(at168shot PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/imgui)
target_compile_definitions(at168shot PRIVATE RADSHOT_USE_GLFW _CRT_SECURE_NO_WARNINGS)
target_link_libraries(at168shot PRIVATE glfw)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(at168shot PRIVATE opengl32 setupapi advapi32)
    # Hide console window on Windows release builds
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set_target_properties(at168shot PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(at168shot PRIVATE OpenGL::GL)
    target_link_libraries(at168shot PRIVATE "-framework IOKit" "-framework CoreFoundation")
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(at168shot PRIVATE OpenGL::GL)
endif()

# Suppress warnings in vendor code
if(MSVC)
    target_compile_options(at168shot PRIVATE /wd4244 /wd4267 /wd4305)
else()
    target_compile_options(at168shot PRIVATE -Wno-unused-result)
endif()
