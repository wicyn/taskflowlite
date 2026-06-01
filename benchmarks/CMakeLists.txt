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
        "GitHub access: direct / ghfast.top / gh-proxy.com / ghproxy.net")
    set_property(CACHE TFL_GITHUB_MIRROR PROPERTY STRINGS
        direct ghfast.top gh-proxy.com ghproxy.net)
    set(TFL_GITHUB_PREFIX "" CACHE STRING "Custom GitHub prefix (overrides TFL_GITHUB_MIRROR when non-empty)")
    set(TFL_GIT_PROXY     "" CACHE STRING "git HTTP(S) proxy, e.g. http://127.0.0.1:7890")
    option(TFL_PARTIAL_CLONE "Partial clone via --filter=blob:none" ON)
    option(TFL_UPDATE_DEPS   "Update cloned deps to latest on re-configure" OFF)
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

    # 有效缓存判断：检查关键头文件是否存在，而非仅看目录在不在。
    # 这样空目录 / 上次中断留下的半截克隆，会因缺 ${_subdir}/taskflow.hpp 被判为无效，
    # 触发重新克隆；目录非空且头文件齐全才直接复用。
    # 注：删 .git 后无法再 fetch/reset，TFL_UPDATE_DEPS 刷新改为整目录重克隆。
    if(EXISTS "${_outdir}/${_subdir}/taskflow.hpp")
        if(TFL_UPDATE_DEPS)
            message(STATUS "[${_name}] refresh requested, removing cache to re-clone ...")
            file(REMOVE_RECURSE "${_outdir}")
        else()
            message(STATUS "[${_name}] using cached checkout (add -DTFL_UPDATE_DEPS=ON to refresh)")
            return()
        endif()
    elseif(EXISTS "${_outdir}")
        # 目录在但头文件缺失（空壳 / 半截克隆）→ 清掉，走下面的全新克隆
        message(STATUS "[${_name}] cached dir incomplete (missing ${_subdir}/taskflow.hpp), re-cloning ...")
        file(REMOVE_RECURSE "${_outdir}")
    endif()

    message(STATUS "[${_name}] sparse clone ${_subdir}/ @ ${_branch} -> ${_outdir}")
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
        message(STATUS "[${_name}] partial clone failed, retrying with full depth-1 ...")
        file(REMOVE_RECURSE "${_outdir}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" ${_proxy} ${_speed} clone --depth 1 --sparse
                    --branch "${_branch}" "${_url}" "${_outdir}"
            RESULT_VARIABLE _rc)
    endif()

    if(NOT _rc EQUAL 0)
        file(REMOVE_RECURSE "${_outdir}")   # 清掉坏的半成品, 下次 configure 干净重试
        message(FATAL_ERROR
            "[${_name}] git clone failed (rc=${_rc}). "
            "Likely cannot reach github: set a global git proxy/mirror, or point ${_name}_LOCAL_PATH to a local source tree.")
    endif()

    # --no-cone：仅检出指定子目录，连仓库根目录的散文件(README/LICENSE/CMakeLists 等)
    # 都不要——cone 模式默认会保留根文件，no-cone 才能做到"只有 ${_subdir}/ 一个目录"。
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_outdir}" sparse-checkout set --no-cone "${_subdir}"
        RESULT_VARIABLE _rc2)
    if(NOT _rc2 EQUAL 0)
        file(REMOVE_RECURSE "${_outdir}")
        message(FATAL_ERROR "[${_name}] sparse-checkout set ${_subdir} failed (rc=${_rc2})")
    endif()

    # 仅作 header-only 基线，不需要 git 历史：删掉 .git/ 省体积，并清掉残留的根散文件，
    # 使 ${_outdir}/ 下最终只剩 ${_subdir}/ 一个目录。
    file(REMOVE_RECURSE "${_outdir}/.git")
    file(GLOB _stray LIST_DIRECTORIES true "${_outdir}/*")
    foreach(_p ${_stray})
        if(NOT _p STREQUAL "${_outdir}/${_subdir}")
            file(REMOVE_RECURSE "${_p}")
        endif()
    endforeach()
endfunction()

# ──────────────────────────────────────────────────────────────────────────────
# Taskflow (header-only, 对比基线) —— 跟随 master 最新
# 本地优先（与 test/catch2/ 对称）。注意 include 根：Taskflow 内部用
# #include <taskflow/taskflow.hpp>，故 include 根目录下必须有一个名为 taskflow/ 的子目录。
#   ① -DTASKFLOW_LOCAL_PATH=<include根> 显式指定（其下需有 taskflow/taskflow.hpp）
#   ② benchmarks/taskflow/（本目录约定放置点；该目录本身即 <taskflow/>，include 根是 benchmarks/）
#   ③ 仓库同级 ../taskflow（其下有 taskflow/taskflow.hpp，include 根是 ../）
#   ④ 都没有 → 联网稀疏克隆到 build/third_party/taskflow（include 根为该目录）
# taskflow_SOURCE_DIR 统一表示“include 根”（其直接子目录里有 taskflow/）。
# ──────────────────────────────────────────────────────────────────────────────
set(TASKFLOW_LOCAL_PATH "" CACHE PATH "Local Taskflow include root (must contain taskflow/taskflow.hpp)")

set(taskflow_SOURCE_DIR "")
set(TFL_TASKFLOW_SOURCE "")

# ① 显式指定：其下须有 taskflow/taskflow.hpp
if(TASKFLOW_LOCAL_PATH AND EXISTS "${TASKFLOW_LOCAL_PATH}/taskflow/taskflow.hpp")
    set(taskflow_SOURCE_DIR "${TASKFLOW_LOCAL_PATH}")
    set(TFL_TASKFLOW_SOURCE "local(-DTASKFLOW_LOCAL_PATH)")
endif()

# ② benchmarks/taskflow/ —— 该目录本身就是 <taskflow/>，故 include 根为 benchmarks/ (= 本目录)
if(NOT taskflow_SOURCE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/taskflow/taskflow.hpp")
    set(taskflow_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(TFL_TASKFLOW_SOURCE "local(benchmarks/taskflow/)")
endif()

# ③ 仓库同级 ../taskflow —— 其下有 taskflow/taskflow.hpp，include 根为 ..
if(NOT taskflow_SOURCE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/../taskflow/taskflow/taskflow.hpp")
    set(taskflow_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../taskflow")
    set(TFL_TASKFLOW_SOURCE "local(../taskflow)")
endif()

if(taskflow_SOURCE_DIR)
    message(STATUS "[taskflow] using local source -> ${taskflow_SOURCE_DIR}/taskflow/")
else()
    # ④ 联网稀疏克隆：include 根 = third_party/taskflow，其下 taskflow/ 子目录由 sparse-checkout 取得
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
message(STATUS "-- TaskflowLite Benchmarks --------------------")
message(STATUS "  Targets       : bench_taskflowlite / bench_taskflow")
message(STATUS "  Taskflow src  : ${TFL_TASKFLOW_SOURCE}")
message(STATUS "  Taskflow path : ${taskflow_SOURCE_DIR}")
message(STATUS "  Run all       : run_all_benchmarks")
message(STATUS "-----------------------------------------------")