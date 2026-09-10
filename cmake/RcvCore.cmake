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

find_package(Threads REQUIRED)

get_filename_component(RCV_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# src/data/wavemanager.cpp includes util/version.h, which is generated. The root
# CMakeLists does this too; the tests tree never did, so generate it here where
# both callers get it. PROJECT_VERSION_* is only set when the *root* project()
# call ran, which is not the case for the tests tree - parse the version out of
# the root CMakeLists so both paths emit a valid header.
if(NOT PROJECT_VERSION_MAJOR)
    file(READ "${RCV_REPO_ROOT}/CMakeLists.txt" _rcv_root_cmake)
    string(REGEX MATCH "VERSION[ \t\r\n]+([0-9]+)\\.([0-9]+)\\.([0-9]+)" _m "${_rcv_root_cmake}")
    set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
endif()
configure_file("${RCV_REPO_ROOT}/src/util/version.h.in" "${CMAKE_BINARY_DIR}/src/util/version.h" @ONLY)

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
    ${RCV_REPO_ROOT}/src/util/jsonrequest.cpp)
list(APPEND RCV_CORE_SOURCE_FILES ${RCV_REPO_ROOT}/src/wave/token.cpp)
list(APPEND RCV_CORE_SOURCE_FILES ${RCV_REPO_ROOT}/src/util/memtracker.cpp)

# Neither is reachable from rcv_core: appconfig is GUI settings persistence,
# and marker_colors only produces QColors for the wave views.
list(REMOVE_ITEM RCV_CORE_SOURCE_FILES ${RCV_REPO_ROOT}/src/config/appconfig.cpp)
list(REMOVE_ITEM RCV_CORE_SOURCE_FILES ${RCV_REPO_ROOT}/src/data/marker_colors.cpp)
# annotation publishes QColor-backed overlay categories for the code views.
list(REMOVE_ITEM RCV_CORE_SOURCE_FILES ${RCV_REPO_ROOT}/src/analysis/annotation.cpp)

# The glob picks up both applyToAsm implementations. Exactly one belongs in
# rcv_core: the no-op stub for CLI builds. The widget-backed version lives in
# src/code/hidden_latency_asm.cpp and is compiled into the GUI target instead.
list(REMOVE_ITEM RCV_CORE_SOURCE_FILES ${RCV_REPO_ROOT}/src/analysis/hidden_latency_stub.cpp)
if(NOT RCV_BUILD_GUI)
    list(APPEND RCV_CORE_SOURCE_FILES ${RCV_REPO_ROOT}/src/analysis/hidden_latency_stub.cpp)
endif()

add_library(rcv_core STATIC ${RCV_CORE_SOURCE_FILES})
# The root CMakeLists sets CMAKE_AUTOMOC globally for the GUI. rcv_core has no
# Q_OBJECT classes left, and leaving it on makes CMake demand Qt6::moc even in
# a Qt-free build.
set_target_properties(rcv_core PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(rcv_core PUBLIC ${RCV_REPO_ROOT}/src ${CMAKE_BINARY_DIR}/src)
# rcv_core is Qt-free. Everything Qt-shaped in the data layer is either a
# QWARNING (a plain std::cout macro from util/memtracker.h) or guarded behind
# RCV_BUILD_GUI. Do not add a Qt link here.
target_link_libraries(rcv_core PUBLIC Threads::Threads)
if(RCV_HAS_TRACE_DECODER)
    target_link_libraries(rcv_core PUBLIC rocprof-trace-decoder::rocprof-trace-decoder-static)
endif()
