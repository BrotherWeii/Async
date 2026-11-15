/*
 * 文件名称：pool_coverage.cpp
 * 摘    要：CThreadPool::enqueue 用例集(断言式回归测试)
 *
 * 用例与意图:
 *   1) 变参入参
 *      自由函数 2 参 / lambda 多参 / 首参被 bind 展平, 结果类型自动推导
 *   2) 结果类型通吃
 *      int / double / std::string / 自定义结构体(可移动) / void 各走一遍
 *   3) 成员函数
 *      成员函数指针 + 对象指针入队, 走 INVOKE 语义
 *   4) 异常传播
 *      任务抛出的异常经 packaged_task 存进 future, get() 端原样重抛
 *   5) 并发投递
 *      4 线程 × 250 任务同时入队, 无丢无重, 结果全部对号
 *   6) 多池共存
 *      两个池同时工作, 互不干扰
 *
 * 完成日期：2026年10月21日
 */

#include <iostream>
#include <string>
#include <cstdio>
#include <vector>
#include <atomic>
#include <thread>
#include <future>
#include <stdexcept>

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

// 自由函数: enqueue 的首参不必是 lambda
static int AddTwo(int a, int b) {
	return a + b;
}

// 自定义返回类型: 验证 packaged_task 对非内置类型照样敲铃
struct Point {
	int x;
	int y;
};

// 带状态的类: 验证成员函数指针入队
struct Accumulator {
	int base = 100;
	int Add(int delta) const { return base + delta; }
};

// ─────────────────────────────────────────────────────────────────────────
// 1) 变参入参: 自由函数 2 参 / lambda 多参
// ─────────────────────────────────────────────────────────────────────────
static void CaseVariadicArgs() {
	std::cout << "\n1) 变参入参\n";

	CThreadPool pool(2);

	auto r1 = pool.enqueue(AddTwo, 3, 4);
	check("自由函数 AddTwo(3,4) == 7", r1.get() == 7);

	auto r2 = pool.enqueue([](int a, int b, int c) { return a * 100 + b * 10 + c; }, 1, 2, 3);
	check("lambda 三参 (1,2,3) == 123", r2.get() == 123);
}

// ─────────────────────────────────────────────────────────────────────────
// 2) 结果类型通吃: int / double / string / 自定义结构体 / void
// ─────────────────────────────────────────────────────────────────────────
static void CaseReturnTypes() {
	std::cout << "\n2) 结果类型\n";

	CThreadPool pool(2);

	auto ri = pool.enqueue([] { return 42; });
	auto rd = pool.enqueue([] { return 3.5; });
	auto rs = pool.enqueue([] { return std::string("hello"); });
	auto rp = pool.enqueue([] { return Point{ 3, 4 }; });
	auto rv = pool.enqueue([] { /* 无返回值 */ });

	check("返回 int 42", ri.get() == 42);
	char buf[64];
	snprintf(buf, sizeof(buf), "%.1f", rd.get());
	check("返回 double 3.5", std::string(buf) == "3.5");
	check("返回 string \"hello\"", rs.get() == "hello");

	Point p = rp.get();
	check("返回自定义结构体 Point{3,4}", p.x == 3 && p.y == 4);

	bool voidOk = true;
	try {
		rv.get();  // future<void>::get() 只等不取
	} catch (...) {
		voidOk = false;
	}
	check("返回 void 的任务正常完成", voidOk);
}

// ─────────────────────────────────────────────────────────────────────────
// 3) 成员函数: 成员函数指针 + 对象指针入队
// ─────────────────────────────────────────────────────────────────────────
static void CaseMemberFunction() {
	std::cout << "\n3) 成员函数\n";

	CThreadPool pool(2);
	Accumulator acc;

	auto r = pool.enqueue(&Accumulator::Add, &acc, 5);
	check("成员函数 Add(5) == 105", r.get() == 105);
}

// ─────────────────────────────────────────────────────────────────────────
// 4) 异常传播: 任务抛异常 → get() 端原样重抛
// ─────────────────────────────────────────────────────────────────────────
static void CaseExceptionPropagation() {
	std::cout << "\n4) 异常传播\n";

	CThreadPool pool(1);

	auto r = pool.enqueue([]() -> int {
		throw std::runtime_error("boom");
	});

	bool caught = false;
	std::string msg;
	try {
		(void)r.get();
	} catch (const std::runtime_error& e) {
		caught = true;
		msg = e.what();
	}
	check("get() 重抛 runtime_error", caught);
	check("异常消息原样保留 \"boom\"", msg == "boom");

	// 抛过异常的任务不应影响池继续工作
	auto r2 = pool.enqueue([] { return 7; });
	check("异常后池继续可用", r2.get() == 7);
}

// ─────────────────────────────────────────────────────────────────────────
// 5) 并发投递: 4 线程 × 250 任务同时入队, 无丢无重
// ─────────────────────────────────────────────────────────────────────────
static void CaseConcurrentEnqueue() {
	std::cout << "\n5) 并发投递\n";

	const int kProducers = 4;
	const int kPerProducer = 250;

	CThreadPool pool(4);
	std::vector<std::future<long long>> futures;
	futures.reserve(kProducers * kPerProducer);

	// 生产者线程同时往同一个池里塞任务
	std::vector<std::thread> producers;
	std::mutex futMutex;  // 只保护 futures 容器
	for (int t = 0; t < kProducers; ++t) {
		producers.emplace_back([&, t] {
			for (int i = 1; i <= kPerProducer; ++i) {
				auto fut = pool.enqueue([](int a, int b) { return 1LL * a * b; }, t + 1, i);
				std::lock_guard<std::mutex> lk(futMutex);
				futures.push_back(std::move(fut));
			}
		});
	}
	for (auto& th : producers)
		th.join();

	check("future 总数 == 1000", futures.size() == (size_t)kProducers * kPerProducer);

	long long expect = 0, actual = 0;
	for (int t = 0; t < kProducers; ++t)
		for (int i = 1; i <= kPerProducer; ++i)
			expect += 1LL * (t + 1) * i;
	for (auto& f : futures)
		actual += f.get();
	check("1000 个任务结果总和正确(无丢无重)", actual == expect);
}

// ─────────────────────────────────────────────────────────────────────────
// 6) 多池共存: 两个池同时工作互不干扰
// ─────────────────────────────────────────────────────────────────────────
static void CaseMultiPool() {
	std::cout << "\n6) 多池共存\n";

	CThreadPool poolA(2);
	CThreadPool poolB(2);

	std::vector<std::future<int>> fa, fb;
	for (int i = 0; i < 20; ++i) {
		fa.push_back(poolA.enqueue([](int v) { return v; }, i));
		fb.push_back(poolB.enqueue([](int v) { return 1000 + v; }, i));
	}

	bool ok = true;
	for (int i = 0; i < 20; ++i) {
		if (fa[i].get() != i)      ok = false;
		if (fb[i].get() != 1000 + i) ok = false;
	}
	check("两个池各自返回各自的结果", ok);
}

int main() {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
#endif

	std::cout << "=== CThreadPool enqueue 用例集 ===\n";

	CaseVariadicArgs();          // 1 变参入参
	CaseReturnTypes();           // 2 结果类型
	CaseMemberFunction();        // 3 成员函数
	CaseExceptionPropagation();  // 4 异常传播
	CaseConcurrentEnqueue();     // 5 并发投递
	CaseMultiPool();             // 6 多池共存

	std::cout << "\n"
	          << (g_failed ? "*** 有断言失败 ***" : "全部断言通过")
	          << "\n";
	return g_failed ? 1 : 0;
}
