# Defines the rcv_core static library: the headless data/analysis layer shared
# by the GUI application and rcv-cli.
#
# Included by both the root CMakeLists.txt and tests/CMakeLists.txt. This is an
# include(), not an add_subdirectory(), on purpose: both of those files call
# add_compile_options(-include .../src/util/diagnostic_log.h) with paths
# relative to themselves, and nesting one inside the other makes CMake emit the
# -include flag once but both header paths, so the second is parsed as an extra
# input file and every compile dies with "cannot specify '-o' with '-c' ...
# with multiple files". include() runs in the caller's directory scope and
# avoids that entirely.

include_guard(GLOBAL)

get_filename_component(RCV_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

option(RCV_BUILD_GUI "Build the Qt Widgets GUI application" OFF)
option(RCV_BUILD_CLI "Build the rcv-cli command-line tool" ON)
if(RCV_BUILD_GUI)
    add_compile_definitions(RCV_BUILD_GUI)
endif()

file(
    GLOB
    RCV_CORE_SOURCE_FILES
    ${RCV_REPO_ROOT}/src/data/*.cpp
    ${RCV_REPO_ROOT}/src/data/waitcnt/*.cpp
    ${RCV_REPO_ROOT}/src/analysis/*.cpp
    ${RCV_REPO_ROOT}/src/code/codeload.cpp
    ${RCV_REPO_ROOT}/src/config/*.cpp
    ${RCV_REPO_ROOT}/src/wave/othersimd.cpp
    ${RCV_REPO_ROOT}/src/util/custom_layouts.cpp
    ${RCV_REPO_ROOT}/src/util/jsonrequest.cpp)

# The glob picks up both applyToAsm implementations. Exactly one belongs in
# rcv_core: the no-op stub for CLI builds. The widget-backed version lives in
# src/code/hidden_latency_asm.cpp and is compiled into the GUI target instead.
list(REMOVE_ITEM RCV_CORE_SOURCE_FILES ${RCV_REPO_ROOT}/src/analysis/hidden_latency_stub.cpp)
if(NOT RCV_BUILD_GUI)
    list(APPEND RCV_CORE_SOURCE_FILES ${RCV_REPO_ROOT}/src/analysis/hidden_latency_stub.cpp)
endif()

add_library(rcv_core STATIC ${RCV_CORE_SOURCE_FILES})
target_include_directories(rcv_core PUBLIC ${RCV_REPO_ROOT}/src ${CMAKE_BINARY_DIR}/src)
# Qt6::Gui is required by src/config/config.cpp (QPalette/QColor) and is safe
# headless: no display is needed unless a QGuiApplication is constructed.
target_link_libraries(rcv_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network)
if(RCV_HAS_TRACE_DECODER)
    target_link_libraries(rcv_core PUBLIC rocprof-trace-decoder::rocprof-trace-decoder-static)
endif()
