cmake_minimum_required(VERSION 3.28)
project(gygax VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CTest)

option(GYGAX_BUILD_TESTS "Build unit and integration tests" ON)
option(GYGAX_BUILD_EXAMPLES "Build examples" ON)
option(GYGAX_BUILD_TOYS "Build the 6502/6809 assembler and CoCo emulator tools" ON)
option(GYGAX_WERROR "Treat warnings as errors" ON)
set(GYGAX_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

if(GYGAX_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=${GYGAX_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${GYGAX_SANITIZE})
endif()

if(WIN32)
    message(FATAL_ERROR "Gygax targets POSIX systems (Linux, macOS). On Windows use WSL2 or the Docker image.")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/gygax/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/gygax/version.hpp @ONLY)

add_library(gygax)

target_sources(gygax
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            src/gygax/core/base.cppm
            src/gygax/core/messaging.cppm
            src/gygax/environment/world.cppm
            src/gygax/signals/events.cppm
            src/gygax/signals/dialogue.cppm
            src/gygax/skills/registry.cppm
            src/gygax/brain/state_machine.cppm
            src/gygax/brain/orchestrator.cppm
            src/gygax/brain/agent_hierarchy.cppm
            src/gygax/brain/embodiments.cppm
            src/gygax/brain/default_brain.cppm
            src/gygax/hardware/core.cppm
            src/gygax/hardware/brains.cppm
            src/gygax/hardware/sensors.cppm
            src/gygax/hardware/actuators.cppm
            src/gygax/hardware/connectors.cppm
    PRIVATE
        src/gygax/core/base.cpp
        src/gygax/net/http.cpp
        src/gygax/net/streams.cpp
        src/gygax/inference/backend.cpp
        src/gygax/inference/atari.cpp
        src/gygax/neuro/network.cpp
        src/gygax/service/expression.cpp
        src/gygax/service/node_info.cpp
        src/gygax/service/service.cpp
        src/gygax/sim/acoustics.cpp
        src/gygax/sim/world2d.cpp
        src/gygax/bus/can.cpp
        src/gygax/bus/isotp.cpp
        src/gygax/bus/obd2.cpp
        src/gygax/bus/j1939.cpp
        src/gygax/bus/dbc.cpp
        src/gygax/bus/canopen.cpp
        src/gygax/net/link.cpp
        src/gygax/robotics/mavlink.cpp
        src/gygax/robotics/device.cpp
        src/gygax/robotics/nmea.cpp
        src/gygax/robotics/adsb.cpp
        src/gygax/robotics/rosbridge.cpp
        src/gygax/robotics/sim_vehicle.cpp
        src/gygax/math/linalg.cpp
        src/gygax/math/bigint.cpp
        src/gygax/robotics/transform.cpp
        src/gygax/robotics/urdf.cpp
        src/gygax/robotics/safety.cpp
        src/gygax/sim/vec_env.cpp
        src/gygax/neuro/events.cpp
        src/gygax/logistics/ledger.cpp
        src/gygax/service/logistics_api.cpp
        src/gygax/service/plugins.cpp
        src/gygax/service/mcp.cpp
        src/gygax/net/websocket.cpp
        src/gygax/bus/modbus.cpp
        src/gygax/bus/arinc429.cpp
        src/gygax/transport/serial.cpp
        src/gygax/transport/gpio.cpp
        src/gygax/transport/modem.cpp
        src/gygax/transport/printer.cpp
        src/gygax/transport/gcode.cpp
)

target_include_directories(gygax PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(gygax PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(gygax PRIVATE src/gygax/robotics)

function(gygax_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(GYGAX_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
        if(GYGAX_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

gygax_warnings(gygax)

find_package(Threads REQUIRED)
# Eigen 3.3+ (Ubuntu ships 3.4, Homebrew 5.x). Eigen's version file accepts only its own major
# version, so ask for any and check the minimum here; 5.x no longer sets EIGEN3_INCLUDE_DIR.
find_package(Eigen3 REQUIRED NO_MODULE)
if(Eigen3_VERSION VERSION_LESS 3.3)
  message(FATAL_ERROR "Eigen 3.3 or newer is required (found ${Eigen3_VERSION})")
endif()
get_target_property(GYGAX_EIGEN_INCLUDE Eigen3::Eigen INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(gygax SYSTEM PRIVATE ${GYGAX_EIGEN_INCLUDE})
# GMP's C++ interface. Searched for rather than named so Homebrew's /opt/homebrew prefix is found too.
find_path(GYGAX_GMPXX_INCLUDE gmpxx.h REQUIRED)
find_library(GYGAX_GMPXX_LIBRARY gmpxx REQUIRED)
find_library(GYGAX_GMP_LIBRARY gmp REQUIRED)
target_include_directories(gygax SYSTEM PRIVATE ${GYGAX_GMPXX_INCLUDE})
target_link_libraries(gygax PRIVATE ${GYGAX_GMPXX_LIBRARY} ${GYGAX_GMP_LIBRARY})
target_link_libraries(gygax PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

add_library(gygax_c SHARED src/gygax/capi/capi.cpp)
set_target_properties(gygax_c PROPERTIES OUTPUT_NAME gygax_c CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(gygax_c PRIVATE gygax)
gygax_warnings(gygax_c)

add_library(gygax_sdk INTERFACE)
add_library(gygax::sdk ALIAS gygax_sdk)
target_include_directories(gygax_sdk INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(gygax_sdk INTERFACE cxx_std_20)
set_target_properties(gygax_sdk PROPERTIES EXPORT_NAME sdk)

if(NOT CMAKE_C_COMPILER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    get_filename_component(_gygax_cxx_name "${CMAKE_CXX_COMPILER}" NAME)
    string(REPLACE "clang++" "clang" _gygax_c_name "${_gygax_cxx_name}")
    find_program(_gygax_c_compiler NAMES ${_gygax_c_name} HINTS ${CMAKE_CXX_COMPILER_LAUNCHER})
    if(_gygax_c_compiler)
        set(CMAKE_C_COMPILER ${_gygax_c_compiler})
    endif()
endif()
enable_language(C)
add_library(gygax_c_plugin MODULE examples/plugin/c_plugin.c)
set_target_properties(gygax_c_plugin PROPERTIES OUTPUT_NAME c_plugin PREFIX "" C_VISIBILITY_PRESET hidden)
target_link_libraries(gygax_c_plugin PRIVATE gygax_sdk)
target_compile_options(gygax_c_plugin PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(gygax_geo_plugin MODULE examples/plugin/geo_plugin.cpp)
set_target_properties(gygax_geo_plugin PROPERTIES OUTPUT_NAME geo_plugin PREFIX "" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(gygax_geo_plugin PRIVATE gygax_sdk)
gygax_warnings(gygax_geo_plugin)

find_package(Python3 COMPONENTS Interpreter)
set(GYGAX_PYTHON_STAGE ${CMAKE_CURRENT_BINARY_DIR}/python)
if(Python3_FOUND)
    file(GLOB GYGAX_PYTHON_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/python/gygax/*.py)
    add_custom_target(gygax_python ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GYGAX_PYTHON_STAGE}/gygax
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GYGAX_PYTHON_SOURCES} ${GYGAX_PYTHON_STAGE}/gygax
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/python/pyproject.toml ${CMAKE_CURRENT_SOURCE_DIR}/python/README.md ${GYGAX_PYTHON_STAGE}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gygax_c> ${GYGAX_PYTHON_STAGE}/gygax
        DEPENDS gygax_c ${GYGAX_PYTHON_SOURCES}
        COMMENT "Staging the Python package in ${GYGAX_PYTHON_STAGE}")
endif()

add_executable(gygax_cli tools/gygax_cli.cpp)
set_target_properties(gygax_cli PROPERTIES OUTPUT_NAME gygax)
target_link_libraries(gygax_cli PRIVATE gygax)
gygax_warnings(gygax_cli)

add_executable(gyde tools/gyde.cpp)
target_link_libraries(gyde PRIVATE gygax)
gygax_warnings(gyde)

if(GYGAX_BUILD_EXAMPLES)
    add_executable(research_swarm examples/research_swarm.cpp)
    target_link_libraries(research_swarm PRIVATE gygax)
    gygax_warnings(research_swarm)

    add_executable(wargames examples/wargames/main.cpp examples/wargames/battlefield.cpp)
    target_link_libraries(wargames PRIVATE gygax)
    target_compile_definitions(wargames PRIVATE GYGAX_WARGAMES_VIEWER="${CMAKE_CURRENT_SOURCE_DIR}/examples/wargames/viewer.html")
    gygax_warnings(wargames)

    add_executable(arrival examples/arrival/main.cpp)
    target_link_libraries(arrival PRIVATE gygax)
    gygax_warnings(arrival)

    add_executable(hardware_demo examples/hardware_demo.cpp)
    target_link_libraries(hardware_demo PRIVATE gygax)
    gygax_warnings(hardware_demo)
endif()

if(GYGAX_BUILD_TOYS)
    add_executable(gygax_asm tools/gygax_asm.cpp)
    target_include_directories(gygax_asm PRIVATE include)
    gygax_warnings(gygax_asm)

    add_executable(gygax_coco tools/gygax_coco.cpp)
    target_link_libraries(gygax_coco PRIVATE gygax)
    gygax_warnings(gygax_coco)
endif()

if(GYGAX_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(gygax_tests
        tests/unit/test_json.cpp
        tests/unit/test_http.cpp
        tests/unit/test_quantum_adapters.cpp
        tests/unit/test_inference.cpp
        tests/unit/test_runtime.cpp
        tests/unit/test_neuro.cpp
        tests/unit/test_transport.cpp
        tests/unit/test_can.cpp
        tests/unit/test_mavlink.cpp
        tests/unit/test_buses.cpp
        tests/unit/test_websocket.cpp
        tests/unit/test_devices.cpp
        tests/unit/test_sim_vehicle.cpp
        tests/unit/test_logistics.cpp
        tests/unit/test_logistics_service.cpp
        tests/unit/test_plugins.cpp
        tests/unit/test_sim.cpp
        tests/unit/test_acoustics.cpp
        tests/unit/test_embodiments.cpp
        tests/unit/test_service.cpp
        tests/unit/test_state_machine.cpp
        tests/unit/test_transform.cpp
        tests/unit/test_safety.cpp
        tests/unit/test_vec_env.cpp
        tests/unit/test_events.cpp
        tests/unit/test_math.cpp
        tests/unit/test_mcp.cpp
        tests/unit/test_atari.cpp
    )
    target_include_directories(gygax_tests PRIVATE tests/unit)
    target_link_libraries(gygax_tests PRIVATE gygax gtest gtest_main)
    target_compile_definitions(gygax_tests PRIVATE GYGAX_TEST_PLUGIN_PATH="$<TARGET_FILE:gygax_geo_plugin>" GYGAX_TEST_C_PLUGIN_PATH="$<TARGET_FILE:gygax_c_plugin>" GYGAX_TEST_FAKE_MCP="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fake_mcp_server.py")
    add_dependencies(gygax_tests gygax_geo_plugin gygax_c_plugin)
    gygax_warnings(gygax_tests)
    include(GoogleTest)
    gtest_discover_tests(gygax_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)

    add_test(NAME gygax_doctor COMMAND gygax_cli doctor)
    if(GYGAX_BUILD_EXAMPLES)
        add_test(NAME wargames_selfcheck COMMAND wargames --selfcheck)
        set_tests_properties(wargames_selfcheck PROPERTIES TIMEOUT 300)
        add_test(NAME gyde_selfcheck COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gyde-check.sh $<TARGET_FILE:gygax_cli> $<TARGET_FILE:gyde>)
        set_tests_properties(gyde_selfcheck PROPERTIES TIMEOUT 60)
        add_test(NAME arrival_selfcheck COMMAND arrival --seconds 12 --real-seconds 2 --usd ${CMAKE_CURRENT_BINARY_DIR}/arrival-test.usda)
    endif()
    if(GYGAX_BUILD_TOYS AND UNIX)
        add_test(NAME toy_assemble_and_run
            COMMAND sh -c "$<TARGET_FILE:gygax_asm> ${CMAKE_CURRENT_SOURCE_DIR}/examples/coco/hello.asm -o ${CMAKE_CURRENT_BINARY_DIR}/hello.dsk --cpu 6809 && $<TARGET_FILE:gygax_coco> ${CMAKE_CURRENT_BINARY_DIR}/hello.dsk --cpu 6809 --quiet | grep -q '1 of 4096 pixels set'")
    endif()
    if(Python3_FOUND AND NOT GYGAX_SANITIZE)
        add_test(NAME python_bindings
            COMMAND ${CMAKE_COMMAND} -E env GYGAX_BIN=$<TARGET_FILE:gygax_cli> GYGAX_PLUGIN=$<TARGET_FILE:gygax_geo_plugin> PYTHONPATH=${GYGAX_PYTHON_STAGE}
                    ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/python/tests)
        add_test(NAME python_example_stdp
            COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${GYGAX_PYTHON_STAGE}
                    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/examples/python/stdp_demo.py)
    endif()
    add_test(NAME sdk_consumer
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/sdk-check.sh ${CMAKE_BINARY_DIR} $<TARGET_FILE:gygax_cli>)
    # Build the consumer with this build's own compiler and link flags (e.g. Homebrew clang on macOS).
    set_tests_properties(sdk_consumer PROPERTIES TIMEOUT 300
        ENVIRONMENT "GYGAX_SDK_CXX=${CMAKE_CXX_COMPILER};GYGAX_SDK_LINKER_FLAGS=${CMAKE_MODULE_LINKER_FLAGS}")
endif()

install(TARGETS gygax_cli gygax_c gyde
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
if(TARGET arrival)
    install(TARGETS arrival RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET gygax_asm)
    install(TARGETS gygax_asm gygax_coco RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/gygax/sdk DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gygax FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
install(FILES include/gygax/core/json.hpp include/gygax/core/charconv.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gygax/core)
install(TARGETS gygax_sdk EXPORT GygaxTargets)
install(EXPORT GygaxTargets NAMESPACE gygax:: FILE GygaxTargets.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Gygax)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/GygaxConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/GygaxConfig.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Gygax)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/GygaxConfigVersion.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/GygaxConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/GygaxConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Gygax)
install(FILES examples/plugin/geo_plugin.cpp examples/plugin/c_plugin.c DESTINATION ${CMAKE_INSTALL_DATADIR}/gygax/examples/plugin)
install(DIRECTORY examples/plugin-consumer DESTINATION ${CMAKE_INSTALL_DATADIR}/gygax/examples)
install(FILES include/gygax/capi/gygax.h ${CMAKE_CURRENT_BINARY_DIR}/generated/gygax/version.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gygax)
install(DIRECTORY python/gygax DESTINATION ${CMAKE_INSTALL_DATADIR}/gygax/python
    PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
install(FILES deploy/gygax.service DESTINATION ${CMAKE_INSTALL_DATADIR}/gygax/deploy)
install(FILES deploy/gygax.env.example DESTINATION ${CMAKE_INSTALL_DATADIR}/gygax/deploy)
install(FILES README.md LICENSE PATENTS.md THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR} FILES_MATCHING PATTERN "*.md")

set(CPACK_PACKAGE_NAME "gygax")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "Neural Splines")
set(CPACK_PACKAGE_CONTACT "robert.sitton@neuralsplines.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Autonomous agent runtime, inference router and neuromorphic simulator")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
if(UNIX AND NOT APPLE)
    list(APPEND CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SECTION "science")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/deploy/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/deploy/debian/prerm")
endif()
include(CPack)
