# WinKSM - Top-level CMakeLists.txt
# Builds both the kernel driver and user-mode service

cmake_minimum_required(VERSION 3.20)
project(winksm LANGUAGES C CXX)

# Add subdirectories
add_subdirectory(driver)
add_subdirectory(service)

# Install targets
install(TARGETS winksm_service
    RUNTIME DESTINATION bin
)

install(DIRECTORY web/
    DESTINATION bin/web
    FILES_MATCHING PATTERN "*.html" PATTERN "*.css" PATTERN "*.js"
)

# Copy web files to source directory for development
add_custom_target(copy_web ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/web"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
        "${CMAKE_BINARY_DIR}/bin/web/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/style.css"
        "${CMAKE_BINARY_DIR}/bin/web/style.css"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/app.js"
        "${CMAKE_BINARY_DIR}/bin/web/app.js"
    COMMENT "Copying web files to output directory"
)

add_dependencies(winksm_service copy_web)
