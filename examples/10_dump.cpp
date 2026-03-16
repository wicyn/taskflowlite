/// @file 10_dump.cpp
/// @brief 演示任务命名与 D2 图形化脚本导出，直观展示框架拓扑结构。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <fstream>

int main() {

    tfl::Flow flow;

    // ================================================================
    // 信号量定义 — 模拟真实资源约束
    // ================================================================
    tfl::Semaphore db_pool(3, "db_pool");           // 数据库连接池，最多 3 并发
    tfl::Semaphore io_band(2, "io_bandwidth");       // IO 带宽限制，最多 2 并发
    tfl::Semaphore cpu_slot(4, "cpu_slot");           // CPU 密集型槽位
    tfl::Semaphore mq_chan(1, "mq_channel");          // 消息队列通道，互斥
    tfl::Semaphore cache_lock(1, "cache_lock");       // 缓存写锁
    tfl::Semaphore net_quota(2, "net_quota");          // 网络请求配额
    tfl::Semaphore gpu_mem(2, "gpu_mem");              // GPU 显存槽位
    tfl::Semaphore txn_lock(1, "txn_lock");            // 分布式事务锁

    // ================================================================
    // Stage 1: 数据采集链 (Basic → Basic → Basic)
    // ================================================================
    auto source    = flow.emplace([]{});  source.name("source");
    auto fetch     = flow.emplace([]{});  fetch.name("fetch_data");
    auto normalize = flow.emplace([]{});  normalize.name("normalize");

    source.precede(fetch);
    fetch.precede(normalize);

    // fetch_data: 网络配额 + IO 带宽
    fetch.acquire(net_quota, 1);
    fetch.acquire(io_band, 1);
    fetch.release(net_quota, 1);
    fetch.release(io_band, 1);

    // normalize: CPU 槽位
    normalize.acquire(cpu_slot, 1);
    normalize.release(cpu_slot, 1);

    // ================================================================
    // Stage 2: 验证 + 错误重试 (Branch + Jump)
    // ================================================================
    // validate (Branch): 验证前获取事务锁，确保验证期间数据一致性
    auto validate = flow.emplace([](tfl::Branch& br) {
        br.allow(0);
    });
    validate.name("validate");
    validate.acquire(txn_lock, 1);
    validate.acquire(db_pool, 1);
    validate.release(txn_lock, 1);
    validate.release(db_pool, 1);

    normalize.precede(validate);

    auto pre_dispatch = flow.emplace([]{});  pre_dispatch.name("pre_dispatch");
    auto log_err      = flow.emplace([]{});  log_err.name("log_error");

    validate.precede(pre_dispatch);
    validate.precede(log_err);

    // log_error: IO 带宽写日志
    log_err.acquire(io_band, 1);
    log_err.release(io_band, 1);

    // retry (Jump): 重试前获取网络配额，确保重试路径不会打爆网络
    auto retry = flow.emplace([](tfl::Jump& jmp) {
        jmp.to(0);
    });
    retry.name("retry");
    retry.acquire(net_quota, 1);
    retry.release(net_quota, 1);

    log_err.precede(retry);
    retry.precede(normalize);

    // ================================================================
    // Stage 3: MultiBranch 3 路分发
    // ================================================================
    // dispatch (MultiBranch): 分发决策需要读数据库路由表 + CPU 计算路由
    auto dispatch = flow.emplace([](tfl::MultiBranch& mbr) {
        mbr.allow(0);
        mbr.allow(1);
        mbr.allow(2);
    });
    dispatch.name("dispatch");
    dispatch.acquire(db_pool, 1);
    dispatch.acquire(cpu_slot, 1);
    dispatch.release(db_pool, 1);
    dispatch.release(cpu_slot, 1);

    pre_dispatch.precede(dispatch);

    // ================================================================
    // Stage 4: 三条并行 Subflow（含图中图）
    // ================================================================

    // -- 管线 A: 3 步 Basic --
    tfl::Flow pipeline_a;
    auto a_read    = pipeline_a.emplace([]{});  a_read.name("read_A");
    auto a_process = pipeline_a.emplace([]{});  a_process.name("process_A");
    auto a_write   = pipeline_a.emplace([]{});  a_write.name("write_A");
    a_read.precede(a_process);
    a_process.precede(a_write);

    // read_A: IO 带宽, process_A: CPU×2, write_A: db 连接
    a_read.acquire(io_band, 1);
    a_read.release(io_band, 1);
    a_process.acquire(cpu_slot, 2);
    a_process.release(cpu_slot, 2);
    a_write.acquire(db_pool, 1);
    a_write.release(db_pool, 1);

    // sub_a (Subflow/Graph): 整条管线执行期间占用 GPU 显存
    auto sub_a = flow.emplace(std::move(pipeline_a), 1);
    sub_a.name("pipeline_A");
    sub_a.acquire(gpu_mem, 1);
    sub_a.release(gpu_mem, 1);

    // -- 管线 B: Basic → Subflow(ETL, 含 Runtime) → Basic --
    tfl::Flow etl_inner;
    auto extract   = etl_inner.emplace([]{});              extract.name("extract");
    auto transform = etl_inner.emplace([](tfl::Runtime&){}); transform.name("transform");
    auto load_task = etl_inner.emplace([]{});              load_task.name("load");
    extract.precede(transform);
    transform.precede(load_task);

    // extract: 获取 db 连接 + 网络（跨阶段持有 db，load 才归还）
    extract.acquire(db_pool, 1);
    extract.acquire(net_quota, 1);
    extract.release(net_quota, 1);
    // transform: CPU 密集
    transform.acquire(cpu_slot, 2);
    transform.release(cpu_slot, 2);
    // load: IO + 归还 extract 拿的 db 连接
    load_task.acquire(io_band, 1);
    load_task.release(io_band, 1);
    load_task.release(db_pool, 1);

    tfl::Flow pipeline_b;
    auto b_init    = pipeline_b.emplace([]{});  b_init.name("init_B");
    auto inner_etl = pipeline_b.emplace(std::move(etl_inner), 2);
    inner_etl.name("ETL_inner");
    auto b_done    = pipeline_b.emplace([]{});  b_done.name("done_B");
    b_init.precede(inner_etl);
    inner_etl.precede(b_done);

    // sub_b (Subflow/Graph): 整条管线占用事务锁 + GPU 显存
    auto sub_b = flow.emplace(std::move(pipeline_b), 1);
    sub_b.name("pipeline_B");
    sub_b.acquire(txn_lock, 1);
    sub_b.acquire(gpu_mem, 1);
    sub_b.release(txn_lock, 1);
    sub_b.release(gpu_mem, 1);

    // -- 管线 C: Runtime → Branch(ok/fail) --
    tfl::Flow pipeline_c;
    auto c_rt    = pipeline_c.emplace([](tfl::Runtime&){}); c_rt.name("dynamic_C");
    auto c_check = pipeline_c.emplace([](tfl::Branch& br){ br.allow(0); });
    c_check.name("check_C");
    auto c_ok    = pipeline_c.emplace([]{});  c_ok.name("ok_C");
    auto c_fail  = pipeline_c.emplace([]{});  c_fail.name("fail_C");
    c_rt.precede(c_check);
    c_check.precede(c_ok);
    c_check.precede(c_fail);

    // dynamic_C (Runtime): CPU + 网络
    c_rt.acquire(cpu_slot, 1);
    c_rt.acquire(net_quota, 1);
    c_rt.release(cpu_slot, 1);
    c_rt.release(net_quota, 1);
    // check_C (Branch): 检查前读缓存
    c_check.acquire(cache_lock, 1);
    c_check.release(cache_lock, 1);

    // sub_c (Subflow/Graph): 整条管线占用 GPU 显存
    auto sub_c = flow.emplace(std::move(pipeline_c), 1);
    sub_c.name("pipeline_C");
    sub_c.acquire(gpu_mem, 1);
    sub_c.release(gpu_mem, 1);

    // MultiBranch → 三条管线
    dispatch.precede(sub_a);
    dispatch.precede(sub_b);
    dispatch.precede(sub_c);

    // ================================================================
    // Stage 5: 汇聚 + Runtime
    // ================================================================
    auto merge = flow.emplace([]{});  merge.name("merge_results");
    sub_a.precede(merge);
    sub_b.precede(merge);
    sub_c.precede(merge);

    // aggregate (Runtime): CPU 全力 + GPU 加速
    auto aggregate = flow.emplace([](tfl::Runtime&){});
    aggregate.name("aggregate");
    merge.precede(aggregate);
    aggregate.acquire(cpu_slot, 4);
    aggregate.acquire(gpu_mem, 2);
    aggregate.release(cpu_slot, 4);
    aggregate.release(gpu_mem, 2);

    // ================================================================
    // Stage 6: MultiJump 散射输出
    // ================================================================
    // scatter (MultiJump): 散射前获取事务锁，保证输出原子性
    auto scatter = flow.emplace([](tfl::MultiJump& mjmp) {
        mjmp.to(0); mjmp.to(1); mjmp.to(2); mjmp.to(3);
    });
    scatter.name("scatter_output");
    scatter.acquire(txn_lock, 1);
    scatter.release(txn_lock, 1);

    aggregate.precede(scatter);

    auto out_db    = flow.emplace([]{});  out_db.name("write_DB");
    auto out_cache = flow.emplace([]{});  out_cache.name("write_cache");
    auto out_file  = flow.emplace([]{});  out_file.name("write_file");
    auto out_queue = flow.emplace([]{});  out_queue.name("push_MQ");

    scatter.precede(out_db);
    scatter.precede(out_cache);
    scatter.precede(out_file);
    scatter.precede(out_queue);

    // write_DB: db 连接池×2 + IO 带宽
    out_db.acquire(db_pool, 2);
    out_db.acquire(io_band, 1);
    out_db.release(db_pool, 2);
    out_db.release(io_band, 1);

    // write_cache: 缓存写锁
    out_cache.acquire(cache_lock, 1);
    out_cache.release(cache_lock, 1);

    // write_file: IO 带宽
    out_file.acquire(io_band, 1);
    out_file.release(io_band, 1);

    // push_MQ: 消息队列互斥 + 网络配额
    out_queue.acquire(mq_chan, 1);
    out_queue.acquire(net_quota, 1);
    out_queue.release(mq_chan, 1);
    out_queue.release(net_quota, 1);

    sub_c.acquire(mq_chan, 1);
    sub_c.acquire(net_quota, 1);
    sub_c.release(mq_chan, 1);
    sub_c.release(net_quota, 1);


    // ================================================================
    // Stage 7: 收尾
    // ================================================================
    auto finalize = flow.emplace([]{});  finalize.name("finalize");
    auto cleanup  = flow.emplace([]{});  cleanup.name("cleanup");

    out_db.precede(finalize);
    out_cache.precede(finalize);
    out_file.precede(finalize);
    out_queue.precede(finalize);

    finalize.precede(cleanup);

    // ================================================================
    // 输出 D2 到文件，D2 渲染器直接读取 UTF-8
    // ================================================================
    flow.dump(std::cout);

    return 0;
}

