/*
 * 文件名称：lifecycle_coverage.cpp
 * 摘    要：CThreadPool 生命周期用例集(断言式回归测试)
 *
 * 覆盖对象是扩展成员 waitAllTaskDone() 与析构语义:
 *   1) 初始状态
 *      0 线程空池也能安全析构(不塞任务即可)
 *   2) waitAllTaskDone 空池立即返回
 *      无任务时调用不阻塞
 *   3) waitAllTaskDone 等满全部任务
 *      返回后队列空、完成计数齐全、所有 future 已就绪
 *   4) 析构排空语义
 *      不调 waitAllTaskDone 直接析构, 在飞 + 排队任务全部跑完才放行(语义是有意的)
 *   5) 析构等在飞任务
 *      析构耗时 >= 任务剩余时长, 证明"排空"而非"快停"
 *
 * 完成日期：2026年10月21日
 */

#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>
#include <future>

#include "ThreadPool.h"

#ifdef _WIN32
#include <windows.h>
#endif

static int g_failed = 0;
static void check(const char* what, bool ok) {
	std::cout << (ok ? "  [OK]   " : "  [FAIL] ") << what << "\n";
	if (!ok)
		++g_failed;
}

static long ElapsedMs(std::chrono::steady_clock::time_point start) {
	return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count());
}

// ─────────────────────────────────────────────────────────────────────────
// 1) 初始状态: 0 线程空池可安全析构(不塞任务即可)
// ─────────────────────────────────────────────────────────────────────────
static void CaseInitialState() {
	std::cout << "\n1) 初始状态\n";

	// 0 线程池: 只要不塞任务, waitAllTaskDone/析构都直接放行(塞了任务会永久等待, 属使用禁忌)
	auto t0 = std::chrono::steady_clock::now();
	{
		CThreadPool pool(0);
		pool.waitAllTaskDone();
	}
	check("0 线程空池 waitAllTaskDone + 析构 立即返回(< 200ms)", ElapsedMs(t0) < 200);
}
// ─────────────────────────────────────────────────────────────────────────
// 2) waitAllTaskDone 空池立即返回
// ─────────────────────────────────────────────────────────────────────────
static void CaseWaitAllIdle() {
	std::cout << "\n2) waitAllTaskDone 空池\n";

	CThreadPool pool(2);
	auto t0 = std::chrono::steady_clock::now();
	pool.waitAllTaskDone();
	check("空池 waitAllTaskDone() 立即返回(< 200ms)", ElapsedMs(t0) < 200);
}

// ─────────────────────────────────────────────────────────────────────────
// 3) waitAllTaskDone 等满全部任务
// ─────────────────────────────────────────────────────────────────────────
static void CaseWaitAllDrains() {
	std::cout << "\n3) waitAllTaskDone 等满全部任务\n";

	CThreadPool pool(4);
	std::atomic<int> done(0);
	std::vector<std::future<int>> futs;

	for (int i = 0; i < 100; ++i) {
		futs.push_back(pool.enqueue([&done](int v) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			++done;
			return v;
		}, i));
	}

	pool.waitAllTaskDone();

	check("waitAllTaskDone 后完成计数 == 100", done.load() == 100);

	bool allReady = true;
	long long sum = 0;
	for (auto& f : futs) {
		if (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			allReady = false;
		sum += f.get();
	}
	check("全部 future 已就绪", allReady);
	check("结果总和 == 4950(无丢失)", sum == 4950);
}

// ─────────────────────────────────────────────────────────────────────────
// 4) 析构排空语义: 不调 waitAllTaskDone 直接析构, 任务也要全部跑完
// ─────────────────────────────────────────────────────────────────────────
static void CaseDestructorDrains() {
	std::cout << "\n4) 析构排空\n";

	std::atomic<int> done(0);
	std::vector<std::future<int>> futs;

	{
		CThreadPool pool(2);
		for (int i = 0; i < 20; ++i) {
			futs.push_back(pool.enqueue([&done](int v) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				++done;
				return v;
			}, i));
		}
		// 故意不调 waitAllTaskDone, 直接出作用域触发析构
	}

	bool allReady = true;
	long long sum = 0;
	for (auto& f : futs) {
		if (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			allReady = false;
		sum += f.get();
	}
	check("析构后 20 个任务全部完成", done.load() == 20);
	check("析构后全部 future 已就绪", allReady);
	check("结果总和 == 190(排空而非丢弃)", sum == 190);
}

// ─────────────────────────────────────────────────────────────────────────
// 5) 析构等在飞任务: 耗时 >= 任务时长, 证明是"排空"不是"快停"
// ─────────────────────────────────────────────────────────────────────────
static void CaseDestructorWaitsInFlight() {
	std::cout << "\n5) 析构等在飞任务\n";

	auto t0 = std::chrono::steady_clock::now();
	{
		CThreadPool pool(1);
		pool.enqueue([] {
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
		});
		// 析构必须等这个 400ms 的任务跑完
	}
	long dt = ElapsedMs(t0);
	check("析构耗时 >= 350ms(在飞任务不被掐断)", dt >= 350);
}

int main() {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
#endif

	std::cout << "=== CThreadPool 生命周期用例集 ===\n";

	CaseInitialState();            // 1 初始状态
	CaseWaitAllIdle();             // 2 waitAllTaskDone 空池
	CaseWaitAllDrains();           // 3 waitAllTaskDone 等满全部任务
	CaseDestructorDrains();        // 4 析构排空
	CaseDestructorWaitsInFlight(); // 5 析构等在飞任务

	std::cout << "\n"
	          << (g_failed ? "*** 有断言失败 ***" : "全部断言通过")
	          << "\n";
	return g_failed ? 1 : 0;
}
