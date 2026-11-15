#include <iostream>
#include <vector>
#include <chrono>

#include "ThreadPool.h"

int main()
{
    // 创建一个4个工作线程的线程池
    CThreadPool pool(4);

	// 投递任务并保存 future
    std::vector<std::future<int>> results;
    for (int i = 0; i < 8; ++i)
    {
        results.emplace_back(
            pool.enqueue([i] {
                char buf[64] = { 0 };
                snprintf(buf, sizeof(buf), "hello world: %d\n", i);
                std::cout << buf;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                return i * i;
            })
        );
    }

	// 从 future 中取得结果
    for (auto &&result : results)
    {
        char buf[64] = { 0 };
        snprintf(buf, sizeof(buf), "result: %d\n", result.get());
        std::cout << buf;
    }
    return 0;
}
