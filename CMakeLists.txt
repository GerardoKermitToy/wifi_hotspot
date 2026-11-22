cmake_minimum_required(VERSION 4.2)
project(WiFiStudio VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "WiFi Studio requires Windows 11 and the Visual Studio C++ toolchain.")
endif()
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND
   CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION VERSION_LESS "10.0.26100.0")
    message(FATAL_ERROR "Install Windows SDK 10.0.26100.0 or newer (Windows 11 24H2 APIs).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # No C++ modules; skip unnecessary MSVC dependency scans.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /guard:cf)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
add_link_options(/DYNAMICBASE /NXCOMPAT /guard:cf)

add_library(hotspot_domain STATIC src/domain.cpp src/localization.cpp)
target_include_directories(hotspot_domain PUBLIC include)
target_link_libraries(hotspot_domain PUBLIC bcrypt)

add_library(hotspot_service STATIC src/hotspot_service.cpp)
target_include_directories(hotspot_service PUBLIC include)
target_link_libraries(hotspot_service PUBLIC hotspot_domain windowsapp wlanapi iphlpapi ole32 runtimeobject)

add_executable(WiFiStudio WIN32 src/main.cpp resources/app.rc)
target_link_libraries(WiFiStudio PRIVATE hotspot_service comctl32 dwmapi uxtheme shell32)
# The manifest is supplied by app.rc (DPI, common controls, Windows 11 compatibility).
target_link_options(WiFiStudio PRIVATE /MANIFEST:NO)

add_executable(wifi-hotspot-cli src/cli.cpp)
target_link_libraries(wifi-hotspot-cli PRIVATE hotspot_service)

include(CTest)
if(BUILD_TESTING)
    add_executable(hotspot_tests tests/domain_tests.cpp)
    target_link_libraries(hotspot_tests PRIVATE hotspot_domain)
    add_test(NAME hotspot.validation COMMAND hotspot_tests)
endif()

install(TARGETS WiFiStudio wifi-hotspot-cli RUNTIME DESTINATION .)
install(FILES README.md README.ru.md DESTINATION .)
install(FILES docs/TESTING.md docs/WPA3.md DESTINATION docs)
