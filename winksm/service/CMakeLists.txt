# WinKSM User-Mode Service - CMakeLists.txt

cmake_minimum_required(VERSION 3.20)
project(winksm_service LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# Source files
set(SERVICE_SOURCES
    main.cpp
)

# Create the service executable
add_executable(winksm_service ${SERVICE_SOURCES})

target_include_directories(winksm_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_link_libraries(winksm_service PRIVATE
    ws2_32
    advapi32
    kernel32
)

# Output settings
set_target_properties(winksm_service PROPERTIES
    OUTPUT_NAME "WinKSM_Service"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Copy web files to output
add_custom_command(TARGET winksm_service POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:winksm_service>/web"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../web/index.html"
        "$<TARGET_FILE_DIR:winksm_service>/web/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../web/style.css"
        "$<TARGET_FILE_DIR:winksm_service>/web/style.css"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../web/app.js"
        "$<TARGET_FILE_DIR:winksm_service>/web/app.js"
    COMMENT "Copying web files to output directory"
)
