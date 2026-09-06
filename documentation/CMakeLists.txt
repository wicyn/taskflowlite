cmake_minimum_required(VERSION 3.21)
include("../cmake/TflVersion.cmake")
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(TaskflowLiteDocs VERSION "${TFL_VERSION}" LANGUAGES NONE)
endif()

# 仅在显式启用文档构建时查找 Doxygen；缺失时给出明确错误。
find_package(Doxygen REQUIRED)

if(Doxygen_FOUND)
    # Doxyfile configuration
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
      "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
      @ONLY
    )

    add_custom_target(GenerateDocs
      COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      COMMENT "Generating API documentation with Doxygen"
      VERBATIM
    )
endif()
