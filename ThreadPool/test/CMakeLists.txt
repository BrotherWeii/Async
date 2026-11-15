# ── 自测用例 ──
# 库目标(ThreadPool INTERFACE)定义在上级 CMakeLists.txt, 这里只注册用例并链接。

# 注册一个测试可执行目标: 自动链接 ThreadPool, 并归入 "test" 文件夹分组
function(add_pool_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ThreadPool)
    set_target_properties(${name} PROPERTIES FOLDER "test")
endfunction()

# 断言式回归用例(输出 [OK]/[FAIL] + 「全部断言通过」, 退出码非 0 即失败)
add_pool_test(pool_coverage)       # enqueue 语义: 变参通吃 / 结果类型 / 异常传播 / 并发投递
add_pool_test(lifecycle_coverage)  # 生命周期: waitAllTaskDone 阻塞语义 / 0 线程空池 / 析构排空

# 原版使用示例(AsyncFramework 的 example.cpp 同款角色, 打印输出无断言)
add_pool_test(example)

# ── 后续用例/示例在这里追加 add_pool_test(...) ──
