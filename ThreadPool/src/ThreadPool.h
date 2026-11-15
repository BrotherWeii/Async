/*
 * 文件名称：ThreadPool.h
 * 摘    要：线程池 CThreadPool 的声明与实现
 *
 * 当前版本：1.0
 * 作    者：aijiawei
 * 完成日期：2026年10月22日
 */
#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>


/**
 * 类名：线程池
 * 概要：1.enqueue 投递任意可调用对象 + 任意参数，返回 future 取结果或异常
 *       2.析构时，会等待所有任务完成才结束
 * 
 * 注意：初始线程数不要指定为0，否则析构时若存在任务未处理会死锁
 */
class CThreadPool
{
public:
    CThreadPool(size_t = std::thread::hardware_concurrency() / 2);
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
		-> std::future<typename std::result_of<F(Args...)>::type>;
	~CThreadPool();

	void waitAllTaskDone();

private:
	std::vector< std::thread > workers;         // 线程队列
	std::queue< std::function<void()> > tasks;  // 任务队列
	std::mutex mutex;                           // 互斥对象
	std::condition_variable ready;              // 来任务/停工时 唤醒
	std::condition_variable done;               // 所有任务完成时 唤醒
	size_t running_cnt{ 0 };                    // 正在执行任务的线程数
	bool stop{ false };                         // 停工标志
};
 
/*
 * 函数名称：初始化工作线程，并等待唤醒循环取任务执行
 *
 * 输入参数：threads：工作线程数(缺省为本机CPU核数的一半)
 * 输出参数：无
 * 返 回 值：无
 *
 * 作    者：aijiawei
 * 日    期：2026年10月22日
 */
inline CThreadPool::CThreadPool(size_t threads)
	: stop(false)
{
	// 启动 threads 个工作线程, 循环取任务执行
	for (size_t i = 0; i < threads; ++i)
	{
		workers.emplace_back([this] {
			while (true)
			{
				// 1.等待唤醒，取任务
				std::function<void()> task;
				{

					std::unique_lock<std::mutex> lock(this->mutex);
					this->ready.wait(lock,                                      
						[this] { return this->stop || !this->tasks.empty(); }); // 停工或来任务时唤醒
					if (this->stop && this->tasks.empty())                      // 已唤醒，停工前收掉所有任务
						return;
					++running_cnt;
					task = std::move(this->tasks.front());                      
					this->tasks.pop();
				}

				// 2.执行任务
				task();                                                         
				// 3.通知完成一个任务
				{
					std::unique_lock<std::mutex> lock(this->mutex);
					running_cnt--;
				}
		
				done.notify_one();
			}
		});
	}
}

/*
 * 函数名称：投递任务，将任意“可调用对象+任意参数”展平成“零参可调物”，
 *           并打包返回 future 取结果或异常
 *
 * 输入参数：f    ：任意可调用对象
 *           args ：任意数量/类型的调用参数
 *
 * 输出参数：无
 * 返 回 值：std::future<返回类型>， get() 可取任务结果或异常
 *
 * 作    者：aijiawei
 * 日    期：2026年10月22日
 */
template<class F, class... Args>
auto CThreadPool::enqueue(F&& f, Args&&... args) 
-> std::future<typename std::result_of<F(Args...)>::type>
{
	// 1.bind 统一为零参可调物，并 packaged_task 打包返回 future ，用于取结果或异常
    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
	using return_type = typename std::result_of<F(Args...)>::type;
    auto task = std::make_shared<std::packaged_task<return_type()>>(std::move(bound));
    std::future<return_type> res = task->get_future();

    {
		// 2.投递任务
		std::unique_lock<std::mutex> lock(mutex);
        if (stop) // 停工后不允许再投递，直接抛异常		
            throw std::runtime_error("Thread pool already stopped"); 
		else
			tasks.emplace([task]() { (*task)(); });
    }

    // 3.通知来活了
    ready.notify_one();
    return res;
}

/*
 * 函数名称：停工，再唤醒所有 worker ，等待所有 task 跑完后，再释放线程
 *
 * 输入参数：无
 * 输出参数：无
 * 返 回 值：无
 *
 * 作    者：aijiawei
 * 日    期：2026年10月22日
 */
inline CThreadPool::~CThreadPool()
{
	// 1.置停工(此后 enqueue 抛异常)
    {
        std::unique_lock<std::mutex> lock(mutex);
        stop = true;                              
    }

	// 2.叫醒所有 worker , 并等待所有 task 完成
    ready.notify_all();                           
	waitAllTaskDone();

	// 3.join 回收全部线程（任务完成不代表线程就立马可释放）
	for (std::thread &worker : workers)
		worker.join();
}

/*
 * 函数名称：waitAllTaskDone
 *
 * 输入参数：无
 * 输出参数：无
 * 返 回 值：无
 *
 * 作    者：aijiawei
 * 日    期：2026年10月22日
 */
inline void CThreadPool::waitAllTaskDone()   
{
	// 阻塞等待全部任务完成
	std::unique_lock<std::mutex> lock(mutex);
	done.wait(lock, [&] { return tasks.empty() && (0 == running_cnt);});
}