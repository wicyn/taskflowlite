cmake_minimum_required(VERSION 3.14)
project(TaskflowLiteDocs)

# Doxygen 是可选的，如果没有安装则跳过文档生成
find_package(Doxygen QUIET)

if(Doxygen_FOUND)
    set(doxygen_quietly YES)
    set(doxygen_WARNINGS NO)
    set(doxygen_WARN_IF_UNDOCUMENTED NO)
    set(doxygen_WARN_IF_DOC_ERROR NO)

    # Doxyfile configuration
    configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
      ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
    )

    add_custom_target(GenerateDocs
      ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Generating API documentation with Doxygen"
    )
else()
    message(STATUS "Doxygen not found, skipping documentation generation")
endif()
