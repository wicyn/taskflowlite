cmake_minimum_required(VERSION 3.21)
set(_fixture "${CMAKE_CURRENT_LIST_DIR}/../tests/build-options")
if(NOT DEFINED BINARY_ROOT)
    set(BINARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/build/options-checks")
endif()

function(check_options name mode error)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_fixture}" -B "${BINARY_ROOT}/${name}"
            "-DEXPECT_SANITIZER=${mode}" ${ARGN}
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _errors)
    if(error)
        if(_result EQUAL 0 OR NOT "${_output}${_errors}" MATCHES "${error}")
            message(FATAL_ERROR "${name}: expected rejection '${error}'\n${_output}${_errors}")
        endif()
    elseif(NOT _result EQUAL 0)
        message(FATAL_ERROR "${name}: configuration failed\n${_output}${_errors}")
    endif()
    message(STATUS "PASS: ${name}")
endfunction()

check_options(off OFF "" -DTFL_SANITIZER=OFF)
check_options(asan ASAN "" -DTFL_SANITIZER=asan)
check_options(legacy-asan ASAN "" -DTFL_TEST_ENABLE_ASAN=ON)
check_options(conflict OFF "Conflicting sanitizer options"
    -DTFL_SANITIZER=ASAN -DTFL_TEST_ENABLE_TSAN=ON)
check_options(legacy-conflict OFF "Conflicting sanitizer options"
    -DTFL_TEST_ENABLE_ASAN=ON -DTFL_TEST_ENABLE_TSAN=ON)
check_options(explicit-off OFF "Conflicting sanitizer options"
    -DTFL_SANITIZER=OFF -DTFL_TEST_ENABLE_ASAN=ON)
check_options(invalid OFF "TFL_SANITIZER must be" -DTFL_SANITIZER=invalid)
if(NOT CMAKE_HOST_WIN32)
    check_options(tsan TSAN "" -DTFL_SANITIZER=TSAN)
    check_options(legacy-tsan TSAN "" -DTFL_TEST_ENABLE_TSAN=ON)
endif()
