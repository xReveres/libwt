cmake_minimum_required(VERSION 3.24)
project(
    libwt
    VERSION 0.1.0
    LANGUAGES C CXX)

# Linux is the initial supported platform; see docs/architecture.md.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "libwt currently supports Linux only")
endif()
option(LIBWT_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(LIBWT_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(LIBWT_BUILD_NODE "Build Node-API bindings" OFF)
option(LIBWT_USE_SYSTEM_MSQUIC "Use installed MsQuic headers and shared library" OFF)
set(LIBWT_MSQUIC_SOURCE_DIR
    ""
    CACHE PATH "Existing MsQuic checkout with OpenSSL")
include(GNUInstallDirs)
include(FetchContent)
find_package(Threads REQUIRED)

set(LIBWT_BUNDLE_MSQUIC OFF)
if(NOT TARGET msquic)
    if(LIBWT_USE_SYSTEM_MSQUIC)
        find_path(LIBWT_MSQUIC_INCLUDE_DIR msquic.h REQUIRED)
        find_library(LIBWT_MSQUIC_LIBRARY NAMES msquic REQUIRED)
        add_library(msquic SHARED IMPORTED)
        set_target_properties(
            msquic PROPERTIES IMPORTED_LOCATION "${LIBWT_MSQUIC_LIBRARY}"
                              INTERFACE_INCLUDE_DIRECTORIES "${LIBWT_MSQUIC_INCLUDE_DIR}")
    else()
        set(LIBWT_BUNDLE_MSQUIC ON)
        set(QUIC_TLS_LIB
            openssl
            CACHE STRING "MsQuic TLS provider")
        set(QUIC_BUILD_SHARED
            ON
            CACHE BOOL "Build shared MsQuic")
        set(QUIC_BUILD_TOOLS
            OFF
            CACHE BOOL "Build MsQuic tools")
        set(QUIC_BUILD_TEST
            OFF
            CACHE BOOL "Build MsQuic tests")
        set(QUIC_BUILD_PERF
            OFF
            CACHE BOOL "Build MsQuic benchmarks")
        set(QUIC_ENABLE_LOGGING
            OFF
            CACHE BOOL "Enable MsQuic tracing")
        if(LIBWT_MSQUIC_SOURCE_DIR)
            add_subdirectory("${LIBWT_MSQUIC_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/msquic")
            set(msquic_SOURCE_DIR "${LIBWT_MSQUIC_SOURCE_DIR}")
        else()
            FetchContent_Declare(
                msquic
                GIT_REPOSITORY https://github.com/microsoft/msquic.git
                GIT_TAG a01333cf7c2659cce0ff03ef3f21e1ff15bb5b83
                GIT_SUBMODULES submodules/openssl GIT_SUBMODULES_RECURSE OFF)
            FetchContent_MakeAvailable(msquic)
        endif()
    endif()
endif()
if(NOT TARGET msquic)
    message(FATAL_ERROR "MsQuic target was not provided by the selected dependency source")
endif()
add_library(wt_qpack STATIC third_party/ls-qpack/lsqpack.c third_party/xxhash/xxhash.c)
set_target_properties(wt_qpack PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(
    wt_qpack
    PUBLIC third_party/ls-qpack
    PRIVATE third_party/xxhash)
target_compile_definitions(wt_qpack PRIVATE LS_QPACK_USE_LARGE_TABLES=0)

add_library(wt SHARED src/Server.cpp src/Connection.cpp src/Http3Protocol.cpp src/Http3Utils.cpp
                      src/QpackUtils.cpp)
add_library(libwt::wt ALIAS wt)
set_target_properties(
    wt
    PROPERTIES VERSION ${PROJECT_VERSION}
               SOVERSION 0
               INSTALL_RPATH "$ORIGIN"
               POSITION_INDEPENDENT_CODE ON)
target_compile_features(wt PUBLIC cxx_std_17)
target_include_directories(wt PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                     "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
if(msquic_SOURCE_DIR)
    target_include_directories(wt SYSTEM PRIVATE "${msquic_SOURCE_DIR}/src/inc")
endif()
target_link_libraries(wt PRIVATE msquic wt_qpack Threads::Threads)
target_compile_options(wt PRIVATE -Wall -Wextra -Wpedantic)

if(LIBWT_BUILD_TESTS)
    enable_testing()
    add_executable(wt_protocol_tests tests/ProtocolTests.cpp)
    target_include_directories(wt_protocol_tests PRIVATE src)
    target_link_libraries(wt_protocol_tests PRIVATE libwt::wt)
    add_test(NAME protocol COMMAND wt_protocol_tests)
    add_executable(wt_lifecycle_tests tests/LifecycleTests.cpp)
    target_link_libraries(wt_lifecycle_tests PRIVATE libwt::wt)
    add_test(NAME lifecycle COMMAND wt_lifecycle_tests)
    add_executable(wt_statistics_server tests/StatisticsServer.cpp)
    target_link_libraries(wt_statistics_server PRIVATE libwt::wt)
    add_executable(wt_publication_tests tests/PublicationFailureTests.cpp)
    target_include_directories(wt_publication_tests PRIVATE src)
    if(msquic_SOURCE_DIR)
        target_include_directories(wt_publication_tests SYSTEM
                                   PRIVATE "${msquic_SOURCE_DIR}/src/inc")
    endif()
    target_link_libraries(wt_publication_tests PRIVATE libwt::wt msquic)
    add_test(NAME publication_failure COMMAND wt_publication_tests)
endif()
if(LIBWT_BUILD_EXAMPLES OR LIBWT_BUILD_TESTS)
    add_executable(wt_echo examples/echo.cpp)
    target_link_libraries(wt_echo PRIVATE libwt::wt)
endif()
if(LIBWT_BUILD_NODE)
    add_subdirectory(bindings/node)
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/libwtConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/libwtConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libwt")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libwtConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
install(
    TARGETS wt
    EXPORT libwtTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(LIBWT_BUNDLE_MSQUIC)
    install(FILES "$<TARGET_FILE:msquic>" DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES "${msquic_SOURCE_DIR}/LICENSE" "${msquic_SOURCE_DIR}/THIRD-PARTY-NOTICES"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/libwt/msquic)
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    EXPORT libwtTargets
    NAMESPACE libwt::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libwt)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libwtConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/libwtConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libwt)
install(
    FILES third_party/README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/libwt
    RENAME THIRD_PARTY.md)
