cmake_minimum_required(VERSION 3.24)
project(libwt_subdirectory_consumer LANGUAGES C CXX)
set(LIBWT_BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE)
set(LIBWT_BUILD_EXAMPLES
    OFF
    CACHE BOOL "" FORCE)
add_subdirectory(../.. libwt-build)
add_executable(consumer ../Consumer.cpp)
target_link_libraries(consumer PRIVATE libwt::wt)
