# AGENTS.md

本文件为在本仓库工作的 AI agent 提供指引：项目结构、构建方式、架构要点与改动时的注意事项。

## Overview

Header-only C++11 线程池库，单文件实现（`src/ThreadPool.h`），类名 `CThreadPool`。

基于 Jakob Progsch & Václav Zeman 的开源 ThreadPool（zlib 许可）二次开发，改动包括：类名 `ThreadPool` → `CThreadPool`、新增 `waitAllTaskDone()` 及其配套的 `running_cnt` / `done`、构造函数默认参数、重写全部注释。


## Build & Run

唯一构建路径是 CMake，构建目录 `build/`：

```powershell
cmake -S . -B build -G "Visual Studio 15 2017"      # 从零配置
cmake --build build --config Debug                  # 增量构建库 + 全部用例
cmake -B build -DBUILD_TEST=OFF                     # 只配库, 不要测试
```

测试目标共 3 个（都是独立可执行文件，在 `build\test\Debug\`）：

- `example`                              — 最简使用示例（4 线程跑 8 个任务，打印 hello/world 与结果，无断言）
- `pool_coverage` / `lifecycle_coverage` — 断言式回归用例，输出 `[OK]`/`[FAIL]`，以「全部断言通过」收尾，退出码非 0 即失败


```powershell
# 运行单个用例
.\build\test\Debug\pool_coverage.exe
# 运行全部断言用例（退出码全 0 才算通过）
Get-ChildItem build\test\Debug\*_coverage.exe | ForEach-Object { & $_.FullName; if ($LASTEXITCODE -ne 0) { throw "$($_.Name) FAILED" } }
```

## 目录结构

```
ThreadPool/
├── CMakeLists.txt   # 顶层: INTERFACE 库(目标名 ThreadPool) + BUILD_TEST 开关
├── src/
│   └── ThreadPool.h     # 库本体(header-only, 类 CThreadPool)
└── test/
    ├── CMakeLists.txt   # add_pool_test() 注册宏
    ├── example.cpp      # 最简使用示例
    └── pool_coverage.cpp / lifecycle_coverage.cpp   # 断言用例集
```

`test/CMakeLists.txt` 用 `add_pool_test(name)` 注册用例：丢一个 `xxx.cpp` 进 `test/`，再追加一行 `add_pool_test(xxx)`。它会自动链接 `ThreadPool` INTERFACE 目标（头文件路径 → `src/`）并归入 VS 的 `test` 文件夹分组。

注意：**CMake 目标名仍是 `ThreadPool`**，与类名 `CThreadPool` 有意解耦——改名只发生在 C++ 层，构建标识符保持稳定。

## Architecture

`CThreadPool` 全部实现在 `ThreadPool.h` 中。

### 成员与同步原语

| 成员 | 作用 |
|---|---|
| `workers` | `vector<thread>`，常驻工作线程 |
| `tasks` | `queue<function<void()>>`，类型擦除后的任务队列 |
| `mutex` | 保护 `tasks` / `running_cnt` / `stop` |
| `ready` | 有任务或停工时的唤醒条件变量 |
| `done` | 「全部任务跑完」的唤醒条件变量（配合 `waitAllTaskDone`） |
| `running_cnt` | 正在执行任务的线程数（队列空 ≠ 干完了） |
| `stop` | 停工标志 |

### enqueue：五步打包流水线

```cpp
bind(forward(f), forward(args)...)      // 1. 任意 f + args → 无参调用体
  → packaged_task<return_type()>        // 2. 自带结果仓库 + 铃铛(移动-only)
  → make_shared(...)                    // 3. 包一层指针, 变成可拷贝
  → tasks.emplace([task]{ (*task)(); }) // 4. 类型擦除入队
  → return future                       // 5. 凭条交给投递者
```

- `return_type` 由 `typename std::result_of<F(Args...)>::type` 推导
- **`shared_ptr` 是必需的渡桥**：`function<void()>` 要求可拷贝，`packaged_task` 是移动-only；lambda 拷贝的只是指针本体，引用计数保证任务活到执行完
- 停机后调用会 `throw std::runtime_error("Thread pool already stopped")`

### Worker 循环：两个谓词的分工

```cpp
ready.wait(lock, [this]{ return stop || !tasks.empty(); });  // 醒: 停工 或 有活
if (stop && tasks.empty()) return;                           // 走: 停工 且 没活
++running_cnt;                                               // 在退出判断之后才自增
task = std::move(tasks.front()); tasks.pop();
// —— 解锁后执行 task() ——
running_cnt--;                                               // 加锁改计数
done.notify_one();                                           // 解锁后通知
```

**醒用 `||`、走用 `&&`** 是温和退出的关键：`stop` 已置但队列非空时，worker 醒来继续干活，不丢任务。

### 析构：四步

```cpp
{ lock; stop = true; }              // 1. 锁内停工(此后 enqueue 抛异常)
ready.notify_all();                 // 2. 叫醒所有 worker
waitAllTaskDone();                  // 3. 等队列排空 + 在飞任务跑完
for (auto& w : workers) w.join();   // 4. 回收线程
```

第4步 不能省：任务完成 ≠ 线程退出，线程还要绕回循环头、抢锁、算谓词才 `return`；且 `std::thread` 析构时仍 joinable 会直接 `std::terminate`。

### waitAllTaskDone：上游没有的扩展

阻塞直到 `tasks.empty() && running_cnt == 0`。因为 worker 把任务 `move` 出队列后队列就空了、活却还在跑，所以必须靠 `running_cnt` 才能判断「真的都干完了」。

## Gotchas when modifying

- **线程数不要传 0**：投了任务没人跑，析构会永久阻塞。
- **析构是排空语义**：已投递任务全部跑完才返回，一个永不返回的任务会卡住析构（上游是快速退出，这里是刻意选择）。
- `waitAllTaskDone` 的谓词**故意不含 `stop`**：当前析构顺序（停工 → 通知 → 等排空）保证该状态可达。若调整析构顺序，必须重审这条。
- `running_cnt` 必须在 `stop && tasks.empty()` 退出判断**之后**自增，否则即将退出的 worker 会虚增一个永不归零的计数，`waitAllTaskDone` 永远等不到。
- 计数自减要**加锁**，`done.notify_one()` 放在解锁之后（避免唤醒者立刻撞锁）。
- `std::result_of<F(Args...)>::type` 是 C++11；C++17 起弃用，替代品是 `std::invoke_result_t<F, Args...>`（注意是逗号语法，不是调用语法）。本库地板是 C++11，故保留 `result_of`。
- **源文件必须存成 UTF-8**：项目用 MSVC `/utf-8` 编译，若编辑器把中文注释存成 GBK，会报 `C4828: 文件包含在当前源字符集中无效的字符`。
- Windows 控制台默认 GBK(936)：打印中文前需 `SetConsoleOutputCP(CP_UTF8)`，否则 `/utf-8` 编译进去的中文在运行时乱码（`example.cpp` 全英文输出，故未调用）。
- 增删 `test/*.cpp` 后要重新执行 `cmake -S . -B build ...` 让 CMake 重生成工程，再 `cmake --build`。
