cmake_minimum_required(VERSION 3.22)

# Glyder: Gygax in VR. One portable C++ core (kit, protocol, drop-in, prescription optics, the
# experience itself), one Vulkan renderer, and thin platform layers: a Meta Quest app (OpenXR,
# NativeActivity, no Java), a desktop window, and headless capture.
#
# Inside the Gygax build only the dependency-free core and its tests are built by default; the
# renderer, desktop viewer and Quest app are opt-in, so nothing here can break a normal build.
# Standalone: `cmake --preset quest` (from this directory) builds the APK.

# The version is Gygax's: one release, one number.
if(NOT DEFINED GYGAX_VERSION_FULL)
    file(READ ${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt _gygax_root)
    string(REGEX MATCH "project\\(gygax VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" _ ${_gygax_root})
    set(GLYDER_VERSION ${CMAKE_MATCH_1})
    string(REGEX MATCH "set\\(GYGAX_RELEASE_NAME \"([^\"]*)\"\\)" _ ${_gygax_root})
    set(GLYDER_RELEASE_NAME ${CMAKE_MATCH_1})
    set(GLYDER_VERSION_FULL ${GLYDER_VERSION})
else()
    set(GLYDER_VERSION ${PROJECT_VERSION})
    set(GLYDER_VERSION_FULL ${GYGAX_VERSION_FULL})
    set(GLYDER_RELEASE_NAME ${GYGAX_RELEASE_NAME})
endif()

project(glyder VERSION ${GLYDER_VERSION} LANGUAGES C CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

get_directory_property(_glyder_has_parent PARENT_DIRECTORY)
option(GLYDER_BUILD_TESTS "Build glyder's unit tests" ON)
option(GLYDER_BUILD_VIEWER "Build the Vulkan renderer, desktop viewer and headless tools" OFF)
option(GLYDER_WERROR "Treat warnings as errors" ON)

function(glyder_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(GLYDER_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

configure_file(${CMAKE_CURRENT_LIST_DIR}/include/glyder/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/glyder/version.hpp @ONLY)

# ---------------------------------------------------------------------------------------------
# Core: no dependencies beyond the C++ standard library and POSIX sockets.
add_library(glyder_core STATIC
    src/core/math.cpp
    src/core/kit.cpp
    src/core/draw.cpp
    src/core/image.cpp
    src/core/crypto.cpp
    src/core/wire.cpp
    src/core/net.cpp
    src/core/handling.cpp
    src/core/host.cpp
    src/core/client.cpp
    src/core/rx.cpp
    src/core/rxserver.cpp
    src/core/demo.cpp
    src/core/app.cpp
)
target_include_directories(glyder_core PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/generated)
glyder_warnings(glyder_core)
find_package(Threads REQUIRED)
target_link_libraries(glyder_core PUBLIC Threads::Threads)

if(GLYDER_BUILD_TESTS AND NOT ANDROID)
    if(NOT TARGET gtest_main)
        include(FetchContent)
        FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
    add_executable(glyder_tests tests/test_core.cpp tests/test_session.cpp tests/test_experience.cpp)
    target_link_libraries(glyder_tests PRIVATE glyder_core gtest gtest_main)
    get_target_property(_glyder_gtest_inc gtest INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(glyder_tests SYSTEM PRIVATE ${_glyder_gtest_inc})
    glyder_warnings(glyder_tests)
    include(GoogleTest)
    gtest_discover_tests(glyder_tests TEST_PREFIX "glyder." DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()

# ---------------------------------------------------------------------------------------------
# Renderer and desktop tools (opt-in; needs Vulkan headers and loader).
if(GLYDER_BUILD_VIEWER AND NOT ANDROID)
    find_package(Vulkan REQUIRED)
    add_library(glyder_render STATIC src/render/render.cpp)
    target_link_libraries(glyder_render PUBLIC glyder_core Vulkan::Vulkan)
    glyder_warnings(glyder_render)
    target_compile_options(glyder_render PRIVATE -Wno-missing-field-initializers)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
    add_executable(glyder_desktop platform/desktop/main.cpp)
    set_target_properties(glyder_desktop PROPERTIES OUTPUT_NAME glyder)
    target_link_libraries(glyder_desktop PRIVATE glyder_render PkgConfig::XCB)
    glyder_warnings(glyder_desktop)
    target_compile_options(glyder_desktop PRIVATE -Wno-missing-field-initializers)

    add_executable(glyder_gallery tools/gallery.cpp)
    target_link_libraries(glyder_gallery PRIVATE glyder_render)
    glyder_warnings(glyder_gallery)
    target_compile_options(glyder_gallery PRIVATE -Wno-missing-field-initializers)
endif()

# ---------------------------------------------------------------------------------------------
# Meta Quest: an APK built straight from the NDK and the SDK build tools, no Gradle.
if(ANDROID)
    # The Khronos OpenXR loader (Apache-2.0) from its official Android archive, pinned by hash.
    set(GLYDER_OPENXR_VERSION 1.1.63)
    set(GLYDER_OPENXR_SHA256 622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73)
    set(_aar ${CMAKE_BINARY_DIR}/openxr_loader_for_android-${GLYDER_OPENXR_VERSION}.aar)
    if(NOT EXISTS ${_aar})
        file(DOWNLOAD
            https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/${GLYDER_OPENXR_VERSION}/openxr_loader_for_android-${GLYDER_OPENXR_VERSION}.aar
            ${_aar} EXPECTED_HASH SHA256=${GLYDER_OPENXR_SHA256} TLS_VERIFY ON)
    endif()
    set(_oxr ${CMAKE_BINARY_DIR}/openxr-${GLYDER_OPENXR_VERSION})
    if(NOT EXISTS ${_oxr}/jni/${ANDROID_ABI}/libopenxr_loader.so)
        file(ARCHIVE_EXTRACT INPUT ${_aar} DESTINATION ${_oxr})
    endif()
    add_library(glyder_openxr SHARED IMPORTED)
    set_target_properties(glyder_openxr PROPERTIES
        IMPORTED_LOCATION ${_oxr}/jni/${ANDROID_ABI}/libopenxr_loader.so
        IMPORTED_NO_SONAME OFF
        INTERFACE_INCLUDE_DIRECTORIES ${_oxr}/prefab/modules/headers/include)

    add_library(glyder_render STATIC src/render/render.cpp)
    target_link_libraries(glyder_render PUBLIC glyder_core vulkan)
    glyder_warnings(glyder_render)
    target_compile_options(glyder_render PRIVATE -Wno-missing-field-initializers)

    add_library(glyder_native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    target_include_directories(glyder_native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

    add_library(glyder SHARED platform/quest/main.cpp)
    target_link_libraries(glyder PRIVATE glyder_render glyder_native_app_glue glyder_openxr android log vulkan)
    glyder_warnings(glyder)
    target_compile_options(glyder PRIVATE -Wno-missing-field-initializers)
    # Keep the NativeActivity entry point; 16 KB pages for current Android.
    target_link_options(glyder PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)

    # Packaging.
    if(DEFINED ENV{ANDROID_HOME})
        set(_sdk $ENV{ANDROID_HOME})
    elseif(DEFINED ENV{ANDROID_SDK_ROOT})
        set(_sdk $ENV{ANDROID_SDK_ROOT})
    else()
        get_filename_component(_sdk ${ANDROID_NDK}/../.. ABSOLUTE)
    endif()
    file(GLOB _bt LIST_DIRECTORIES true ${_sdk}/build-tools/*)
    list(SORT _bt COMPARE NATURAL ORDER DESCENDING)
    list(GET _bt 0 _bt)
    set(GLYDER_ANDROID_JAR ${_sdk}/platforms/android-34/android.jar CACHE FILEPATH "android.jar to link against")
    find_program(GLYDER_AAPT2 aapt2 HINTS ${_bt} REQUIRED)
    find_program(GLYDER_ZIPALIGN zipalign HINTS ${_bt} REQUIRED)
    find_program(GLYDER_APKSIGNER apksigner HINTS ${_bt} REQUIRED)
    find_program(GLYDER_ZIP zip REQUIRED)
    find_program(GLYDER_KEYTOOL keytool REQUIRED)
    set(GLYDER_KEYSTORE ${CMAKE_BINARY_DIR}/glyder-debug.keystore CACHE FILEPATH "Keystore that signs the APK")
    set(GLYDER_KEYSTORE_PASS "android" CACHE STRING "Its password (use pass:env:VAR style via GLYDER_KEYSTORE_PASS_ARG for CI)")
    set(GLYDER_KEYSTORE_PASS_ARG "pass:${GLYDER_KEYSTORE_PASS}" CACHE STRING "apksigner --ks-pass value")
    math(EXPR GLYDER_VERSION_CODE "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")

    set(_stage ${CMAKE_BINARY_DIR}/apk)
    set(_apk ${CMAKE_BINARY_DIR}/glyder-${GLYDER_VERSION_FULL}-quest.apk)
    file(GLOB_RECURSE _res CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/platform/quest/res/*)
    add_custom_command(
        OUTPUT ${_apk}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_stage}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_stage}/lib/${ANDROID_ABI}
        COMMAND ${GLYDER_AAPT2} compile --dir ${CMAKE_CURRENT_LIST_DIR}/platform/quest/res -o ${_stage}/res.zip
        COMMAND ${GLYDER_AAPT2} link -o ${_stage}/unsigned.apk --manifest ${CMAKE_CURRENT_LIST_DIR}/platform/quest/AndroidManifest.xml
                -I ${GLYDER_ANDROID_JAR} ${_stage}/res.zip --min-sdk-version 29 --target-sdk-version 34
                --version-code ${GLYDER_VERSION_CODE} --version-name ${GLYDER_VERSION_FULL}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:glyder> ${_oxr}/jni/${ANDROID_ABI}/libopenxr_loader.so ${_stage}/lib/${ANDROID_ABI}/
        COMMAND ${CMAKE_STRIP} --strip-unneeded ${_stage}/lib/${ANDROID_ABI}/libglyder.so
        COMMAND ${CMAKE_COMMAND} -E chdir ${_stage} ${GLYDER_ZIP} -q -r unsigned.apk lib
        COMMAND ${GLYDER_ZIPALIGN} -f -P 16 4 ${_stage}/unsigned.apk ${_stage}/aligned.apk
        COMMAND ${CMAKE_COMMAND} -DKEYTOOL=${GLYDER_KEYTOOL} -DKEYSTORE=${GLYDER_KEYSTORE} -DPASS=${GLYDER_KEYSTORE_PASS}
                -P ${CMAKE_CURRENT_LIST_DIR}/platform/quest/keystore.cmake
        COMMAND ${GLYDER_APKSIGNER} sign --ks ${GLYDER_KEYSTORE} --ks-pass ${GLYDER_KEYSTORE_PASS_ARG} --out ${_apk} ${_stage}/aligned.apk
        COMMAND ${GLYDER_APKSIGNER} verify ${_apk}
        DEPENDS glyder ${CMAKE_CURRENT_LIST_DIR}/platform/quest/AndroidManifest.xml ${_res}
        COMMENT "Packaging and signing glyder-${GLYDER_VERSION_FULL}-quest.apk"
        VERBATIM)
    add_custom_target(glyder_apk ALL DEPENDS ${_apk})
endif()
