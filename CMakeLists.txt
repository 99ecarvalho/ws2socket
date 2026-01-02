cmake_minimum_required(VERSION 3.10)
project(ws2socket VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find required packages
find_package(OpenSSL REQUIRED)

# Add source files
set(SOURCES
    src/websocket.c
    src/websocket_impl.c
    src/http_server.c
    src/proxy.c
    src/server.c
    src/utils.c
    src/config.c
    src/logging.c
    src/token_auth.c
    src/conn_pool.c
    src/metrics.c
    src/ws2socket.c
)

# Create executable
add_executable(ws2socket ${SOURCES})

# Link libraries
target_link_libraries(ws2socket 
    PRIVATE 
        OpenSSL::SSL 
        OpenSSL::Crypto
        pthread
)

# Include directories
target_include_directories(ws2socket 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OPENSSL_INCLUDE_DIR}
)

# Compiler flags for better warnings
if(UNIX AND NOT APPLE)
    target_compile_options(ws2socket PRIVATE -Wall -Wextra -Wpedantic -Wstrict-prototypes)
endif()

# Install target
install(TARGETS ws2socket DESTINATION bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/docs/ws2socket.1 DESTINATION share/man/man1)

# Doxygen documentation
find_package(Doxygen)
if(DOXYGEN_FOUND)
    set(DOXYGEN_INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    set(DOXYGEN_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/docs)
    set(DOXYGEN_INDEX_FILE ${DOXYGEN_OUTPUT_DIR}/html/index.html)
    set(DOXYGEN_CONFIG_FILE ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
        ${DOXYGEN_CONFIG_FILE}
        @ONLY
    )
    
    add_custom_command(
        OUTPUT ${DOXYGEN_INDEX_FILE}
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_CONFIG_FILE}
        MAIN_DEPENDENCY ${DOXYGEN_CONFIG_FILE}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
    
    add_custom_target(docs ALL DEPENDS ${DOXYGEN_INDEX_FILE})
endif()
