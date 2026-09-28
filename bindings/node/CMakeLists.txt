find_program(LIBWT_NODE_EXECUTABLE node REQUIRED)
execute_process(
    COMMAND "${LIBWT_NODE_EXECUTABLE}" -p
            "require('path').resolve(process.execPath, '../../include/node')"
    OUTPUT_VARIABLE LIBWT_NODE_PREFIX_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE LIBWT_NODE_QUERY_RESULT)
if(NOT LIBWT_NODE_QUERY_RESULT EQUAL 0)
    message(FATAL_ERROR "Unable to query Node.js header directory")
endif()
find_path(LIBWT_NODE_INCLUDE_DIR node_api.h HINTS "${LIBWT_NODE_PREFIX_INCLUDE}" /usr/include/node
                                                  /usr/local/include/node)
if(NOT LIBWT_NODE_INCLUDE_DIR)
    message(FATAL_ERROR "Node-API headers not found; install Node.js development headers")
endif()
add_library(wt_node MODULE addon.cpp)
target_include_directories(wt_node PRIVATE "${LIBWT_NODE_INCLUDE_DIR}")
target_link_libraries(wt_node PRIVATE libwt::wt)
target_compile_features(wt_node PRIVATE cxx_std_17)
target_compile_definitions(wt_node PRIVATE NAPI_VERSION=8)
set_target_properties(
    wt_node
    PROPERTIES PREFIX ""
               SUFFIX ".node"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(wt_node PRIVATE -Wall -Wextra -Wpedantic)
