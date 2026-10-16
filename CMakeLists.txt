cmake_minimum_required(VERSION 3.20)

project(netpulse
    VERSION 1.0.0
    DESCRIPTION "A ping implementation for Linux and Windows"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NETPULSE_BUILD_TESTS "Build the unit tests" ON)
option(NETPULSE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(netpulse_warnings INTERFACE)
if(MSVC)
    target_compile_options(netpulse_warnings INTERFACE /W4 /permissive- /utf-8)
    if(NETPULSE_WARNINGS_AS_ERRORS)
        target_compile_options(netpulse_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(netpulse_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(NETPULSE_WARNINGS_AS_ERRORS)
        target_compile_options(netpulse_warnings INTERFACE -Werror)
    endif()
endif()

add_library(netpulse_core STATIC
    src/cli.cpp
    src/icmp.cpp
    src/ip_address.cpp
    src/report.cpp
    src/resolver.cpp
    src/statistics.cpp
    src/platform/socket_address.cpp)
add_library(netpulse::core ALIAS netpulse_core)
target_include_directories(netpulse_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(netpulse_core PRIVATE netpulse_warnings)

if(WIN32)
    target_sources(netpulse_core PRIVATE src/windows/icmp_client_windows.cpp)
    target_compile_definitions(netpulse_core PUBLIC
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(netpulse_core PUBLIC iphlpapi ws2_32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(netpulse_core PRIVATE src/linux/icmp_client_linux.cpp)
else()
    message(FATAL_ERROR "netpulse supports Linux and Windows; ${CMAKE_SYSTEM_NAME} has no ICMP backend yet")
endif()

add_executable(netpulse app/main.cpp)
target_compile_definitions(netpulse PRIVATE NETPULSE_VERSION="${PROJECT_VERSION}")
target_link_libraries(netpulse PRIVATE netpulse::core netpulse_warnings)
if(MINGW)
    target_link_options(netpulse PRIVATE -static)
endif()

if(NETPULSE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS netpulse RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" NETPULSE_PLATFORM)
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-${NETPULSE_PLATFORM}")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
