# 从入口头文件读取版本；主项目和独立文档构建共用。
file(READ "${CMAKE_CURRENT_LIST_DIR}/../taskflowlite/taskflowlite.hpp" _tfl_version_header)
foreach(_part MAJOR MINOR PATCH)
    string(REGEX MATCH "#define[ \t]+TASKFLOWLITE_VERSION_${_part}[ \t]+([0-9]+)"
        _match "${_tfl_version_header}")
    if(NOT _match)
        message(FATAL_ERROR "Missing TASKFLOWLITE_VERSION_${_part} in taskflowlite.hpp")
    endif()
    set(_tfl_version_${_part} "${CMAKE_MATCH_1}")
endforeach()
set(TFL_VERSION "${_tfl_version_MAJOR}.${_tfl_version_MINOR}.${_tfl_version_PATCH}")
