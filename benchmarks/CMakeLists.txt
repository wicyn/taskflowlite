# ==============================================================================
# TaskflowLite Benchmarks -- TaskflowLite vs Taskflow 对比
# 依赖: Taskflow (header-only, 稀疏/部分克隆, 只取 taskflow/ 目录, 跟随 master)
#
# 依赖获取策略:
#   - 优先使用本地路径 (TASKFLOW_LOCAL_PATH)
#   - 否则用 git 稀疏 + 部分克隆, 只取 taskflow/ 子目录, 不锁版本(跟随 master 顶端)
#   - examples / docs / test / images 等一律不下载
#   - 刷新到更新提交: 删掉 build/third_party/taskflow 重新 configure, 或加 -DTFL_UPDATE_DEPS=ON
# ==============================================================================

find_package(Git REQUIRED)

# 说明: GitHub 访问相关变量(TFL_GITHUB_MIRROR / TFL_GITHUB_PREFIX / TFL_GIT_PROXY /
#       TFL_PARTIAL_CLONE / TFL_UPDATE_DEPS)及最终前缀 TFL_EFFECTIVE_PREFIX
#       已在根 CMakeLists 顶层定义(便于在 cmake-gui 第一次 Configure 就能修改)。
#       这里直接复用父作用域的值。
#
# 独立构建 benchmarks(不经过根 CMake)时的兜底默认:
if(NOT DEFINED TFL_EFFECTIVE_PREFIX)
    set(TFL_GITHUB_MIRROR "direct" CACHE STRING
        "GitHub 访问方式: direct / ghfast.top / gh-proxy.com / ghproxy.net")
    set_property(CACHE TFL_GITHUB_MIRROR PROPERTY STRINGS
        direct ghfast.top gh-proxy.com ghproxy.net)
    set(TFL_GITHUB_PREFIX "" CACHE STRING "自定义 GitHub 前缀(非空时覆盖 TFL_GITHUB_MIRROR)")
    set(TFL_GIT_PROXY     "" CACHE STRING "git HTTP(S) proxy, e.g. http://127.0.0.1:7890")
    option(TFL_PARTIAL_CLONE "用 --filter=blob:none 部分克隆" ON)
    option(TFL_UPDATE_DEPS   "重新 configure 时把已克隆依赖更新到分支最新提交" OFF)
    if(TFL_GITHUB_PREFIX)
        set(TFL_EFFECTIVE_PREFIX "${TFL_GITHUB_PREFIX}")
    elseif(TFL_GITHUB_MIRROR STREQUAL "direct")
        set(TFL_EFFECTIVE_PREFIX "")
    else()
        set(TFL_EFFECTIVE_PREFIX "https://${TFL_GITHUB_MIRROR}/")
    endif()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# 稀疏 + 部分克隆助手
#   --depth 1          : 不要历史
#   --filter=blob:none : 部分克隆, blob 按需取
#   --sparse           : 工作树初始为空(仅根文件), 再用 sparse-checkout 展开子目录
#   三者叠加 => 磁盘上基本只有指定子目录的内容
# ──────────────────────────────────────────────────────────────────────────────
function(tfl_sparse_clone _name _repo _branch _subdir _outdir)
    # 可选代理: 注入 -c http.proxy=... -c https.proxy=...
    set(_proxy "")
    if(TFL_GIT_PROXY)
        list(APPEND _proxy -c "http.proxy=${TFL_GIT_PROXY}" -c "https.proxy=${TFL_GIT_PROXY}")
    endif()

    # 卡住快速失败: 20 秒内速度低于 1000 B/s 就让 git 自己中断, 不再无限挂
    set(_speed -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=20)

    # 镜像前缀(已在上面解析): https://ghfast.top/ + https://github.com/...
    set(_url "${TFL_EFFECTIVE_PREFIX}${_repo}")

    if(EXISTS "${_outdir}/.git")
        if(TFL_UPDATE_DEPS)
            message(STATUS "[taskflow] 更新到 ${_branch} 最新提交 ...")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" ${_proxy} ${_speed} -C "${_outdir}" fetch --depth 1 origin "${_branch}")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" -C "${_outdir}" reset --hard FETCH_HEAD)
        else()
            message(STATUS "[${_name}] 使用已缓存克隆 (加 -DTFL_UPDATE_DEPS=ON 可刷新)")
        endif()
        return()
    endif()

    message(STATUS "[${_name}] 稀疏克隆 ${_subdir}/ @ ${_branch} → ${_outdir}")
    message(STATUS "[${_name}] URL: ${_url}")

    # partial clone 开关: 烂网络/镜像下卡 promisor 时, 配置加 -DTFL_PARTIAL_CLONE=OFF
    set(_filter "")
    if(TFL_PARTIAL_CLONE)
        set(_filter --filter=blob:none)
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${_proxy} ${_speed} clone --depth 1 ${_filter} --sparse
                --branch "${_branch}" "${_url}" "${_outdir}"
        RESULT_VARIABLE _rc)

    # partial clone 失败(常见: 镜像不支持 promisor 协议) → 清残留, 不带 --filter 完整重试一次
    if(NOT _rc EQUAL 0 AND _filter)
        message(STATUS "[${_name}] partial clone 失败, 清理后改用完整 depth-1 重试 ...")
        file(REMOVE_RECURSE "${_outdir}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" ${_proxy} ${_speed} clone --depth 1 --sparse
                    --branch "${_branch}" "${_url}" "${_outdir}"
            RESULT_VARIABLE _rc)
    endif()

    if(NOT _rc EQUAL 0)
        file(REMOVE_RECURSE "${_outdir}")   # 清掉坏的半成品, 下次 configure 干净重试
        message(FATAL_ERROR
            "[${_name}] git clone 失败 (rc=${_rc}). "
            "多半是连不上 github: 给 git 设全局代理/镜像, 或用 ${_name}_LOCAL_PATH 指向本地源码.")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_outdir}" sparse-checkout set "${_subdir}"
        RESULT_VARIABLE _rc2)
    if(NOT _rc2 EQUAL 0)
        file(REMOVE_RECURSE "${_outdir}")
        message(FATAL_ERROR "[${_name}] sparse-checkout set ${_subdir} 失败 (rc=${_rc2})")
    endif()
endfunction()

# ──────────────────────────────────────────────────────────────────────────────
# Taskflow (header-only, 对比基线) —— 只取 taskflow/ 目录, 跟随 master 最新
# ──────────────────────────────────────────────────────────────────────────────
set(TASKFLOW_LOCAL_PATH "" CACHE PATH "Local path to Taskflow source (备用)")

if(NOT TASKFLOW_LOCAL_PATH AND EXISTS "${CMAKE_SOURCE_DIR}/../taskflow/taskflow/taskflow.hpp")
    set(TASKFLOW_LOCAL_PATH "${CMAKE_SOURCE_DIR}/../taskflow")
endif()

if(TASKFLOW_LOCAL_PATH)
    set(TFL_TASKFLOW_SOURCE "local")
    message(STATUS "[taskflow] 使用本地源码: ${TASKFLOW_LOCAL_PATH}")
    set(taskflow_SOURCE_DIR "${TASKFLOW_LOCAL_PATH}")
else()
    set(TFL_TASKFLOW_SOURCE "cloned(master)")
    set(taskflow_SOURCE_DIR "${CMAKE_BINARY_DIR}/third_party/taskflow")
    tfl_sparse_clone(taskflow
        "https://github.com/taskflow/taskflow.git" master taskflow "${taskflow_SOURCE_DIR}")
endif()

# Taskflow 是 header-only: 直接造 INTERFACE 目标 (不走它自己的 CMake, 不编 examples/tests)
if(NOT TARGET Taskflow::Taskflow)
    add_library(Taskflow INTERFACE)
    add_library(Taskflow::Taskflow ALIAS Taskflow)
    target_include_directories(Taskflow INTERFACE "${taskflow_SOURCE_DIR}")
    target_compile_features(Taskflow INTERFACE cxx_std_17)
    find_package(Threads REQUIRED)
    target_link_libraries(Taskflow INTERFACE Threads::Threads)
endif()

# INTERFACE 库在 VS 里不生成项目, 看不到头文件。
# 造一个纯浏览用的假目标(不参与构建), 仅为在 IDE 里能点开 taskflow/ 头文件。
file(GLOB_RECURSE TASKFLOW_HEADERS CONFIGURE_DEPENDS
    "${taskflow_SOURCE_DIR}/taskflow/*.hpp"
    "${taskflow_SOURCE_DIR}/taskflow/*.h")
if(TASKFLOW_HEADERS AND NOT TARGET taskflow_headers)
    add_custom_target(taskflow_headers SOURCES ${TASKFLOW_HEADERS})
    source_group(TREE "${taskflow_SOURCE_DIR}/taskflow" FILES ${TASKFLOW_HEADERS})
    set_target_properties(taskflow_headers PROPERTIES FOLDER "ThirdParty")
endif()

# ──────────────────────────────────────────────────────────────────────────────
# bench_taskflowlite -- TaskflowLite benchmarks (20 scheduling scenarios)
# ──────────────────────────────────────────────────────────────────────────────
add_executable(bench_taskflowlite bench_taskflowlite.cpp)
target_link_libraries(bench_taskflowlite PRIVATE
    TaskflowLite::taskflowlite
    tfl_internal_flags
    Threads::Threads
)
target_compile_features(bench_taskflowlite PRIVATE cxx_std_23)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES bench_taskflowlite.cpp)

# ──────────────────────────────────────────────────────────────────────────────
# bench_taskflow -- Taskflow comparison benchmarks
# ──────────────────────────────────────────────────────────────────────────────
add_executable(bench_taskflow bench_taskflow.cpp)
target_link_libraries(bench_taskflow PRIVATE
    Taskflow::Taskflow
    Threads::Threads
)
target_compile_features(bench_taskflow PRIVATE cxx_std_20)
# bench_common.hpp 是 UTF-8(含中文注释); MSVC 在中文系统(cp936)下需 /utf-8 才能正确解析,
# 否则会报一堆假的 C2059/C2143/C2086(C4819 是源头警告)。
# bench_taskflowlite 因链接了带 /utf-8 的 taskflowlite/tfl_internal_flags 而无此问题。
target_compile_options(bench_taskflow PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES bench_taskflow.cpp)

# 已建出的 benchmark 目标(给 run_all_benchmarks 用)
set(BENCHMARKS bench_taskflowlite bench_taskflow)

# ──────────────────────────────────────────────────────────────────────────────
# run_all_benchmarks -- one-command execution of all benchmark targets
# ──────────────────────────────────────────────────────────────────────────────
add_custom_target(run_all_benchmarks
    COMMAND ${CMAKE_COMMAND} -E echo "Running all benchmarks..."
    USES_TERMINAL
)
foreach(b ${BENCHMARKS})
    add_dependencies(run_all_benchmarks ${b})
    add_custom_command(TARGET run_all_benchmarks POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "===== Running ${b} ====="
        COMMAND $<TARGET_FILE:${b}>
    )
endforeach()

# ──────────────────────────────────────────────────────────────────────────────
# IDE 解决方案文件夹分组 (依赖根 CMakeLists 的 USE_FOLDERS ON)
# ──────────────────────────────────────────────────────────────────────────────
foreach(b ${BENCHMARKS})
    set_target_properties(${b} PROPERTIES FOLDER "Benchmarks")
endforeach()
set_target_properties(run_all_benchmarks PROPERTIES FOLDER "Run")

# ──────────────────────────────────────────────────────────────────────────────
# 状态摘要
# ──────────────────────────────────────────────────────────────────────────────
message(STATUS "── TaskflowLite Benchmarks ────────────────────")
message(STATUS "  对比目标      : bench_taskflowlite / bench_taskflow")
message(STATUS "  Taskflow 来源 : ${TFL_TASKFLOW_SOURCE}")
message(STATUS "  Taskflow 路径 : ${taskflow_SOURCE_DIR}")
message(STATUS "  一键运行      : run_all_benchmarks")
message(STATUS "───────────────────────────────────────────────")
