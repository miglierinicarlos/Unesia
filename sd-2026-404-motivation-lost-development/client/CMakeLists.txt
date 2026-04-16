set(CLIENT_SOURCES
    src/eop_client.c
    src/eop_envelope.c)

add_library(eop_client_static STATIC ${CLIENT_SOURCES})
add_library(eop_client_shared SHARED ${CLIENT_SOURCES})

target_link_libraries(eop_client_static PUBLIC pthread)
target_link_libraries(eop_client_shared PUBLIC pthread)

target_include_directories(eop_client_shared PUBLIC include)
target_include_directories(eop_client_static PUBLIC include)

target_include_directories(eop_client_shared PRIVATE src)
target_include_directories(eop_client_static PRIVATE src)

set_target_properties(eop_client_shared PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    SOVERSION 1
    OUTPUT_NAME eop_client
)

set_target_properties(eop_client_static PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME eop_client
)

add_executable(connect_basic_example
    examples/connect_basic.c
)
target_link_libraries(connect_basic_example PRIVATE eop_client_static)

if(EXODUS_ENABLE_COVERAGE)
    target_compile_options(eop_client_static PRIVATE -O0 -g --coverage)
    target_link_options(eop_client_static PUBLIC --coverage)
  endif()
