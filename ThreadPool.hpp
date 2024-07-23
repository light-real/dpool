#ifndef THREADPOOL_H
#define THREADPOOL_H

/*
#ifndef（如果未定义）和#endif（结束条件编译）常用于防止头文件被多次包含。
这种技术称为“头文件保护”或“包含保护”，确保头文件中的内容只被编译一次，避免重复定义导致的编译错误

检查宏是否定义：#ifndef（如果未定义）用来检查某个宏是否已经定义。如果宏未定义，则执行后续代码。
定义宏：使用#define定义一个宏，表示该头文件已经被包含过。
结束条件编译：#endif用于结束条件编译。
*/
#include <cassert>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace dpool
{

    class ThreadPool
    {
    public:
        using MutexGuard = std::lock_guard<std::mutex>;
        using UniqueLock = std::unique_lock<std::mutex>;
        using Thread = std::thread;
        using ThreadID = std::thread::id;
        using Task = std::function<void()>;

        ThreadPool()
            : ThreadPool(Thread::hardware_concurrency())
        {
        }

        explicit ThreadPool(size_t maxThreads) // explicit 构造函数只能显式声明，不能进行隐式转换
            : quit_(false),
              currentThreads_(0),
              idleThreads_(0),
              maxThreads_(maxThreads) // 只传入一个最大线程数
        {
        }

        // disable the copy operations
        ThreadPool(const ThreadPool &) = delete;            // 删除拷贝构造
        ThreadPool &operator=(const ThreadPool &) = delete; // 删除赋值运算符

        ~ThreadPool()
        {
            {
                MutexGuard guard(mutex_);
                quit_ = true;
            }
            cv_.notify_all(); // 通知所有条件变量

            // std::unordered_map<ThreadID, Thread>  threads_;
            for (auto &elem : threads_)
            {
                assert(elem.second.joinable());
                elem.second.join(); // 等待所有线程执行完成
            }
        }
        // 模版函数 使用了模版函数包来处理可变数量的模版参数 typename... Ts表示一个模版参数包，可以匹配0个或多个类型参数
        //  Func是一个类型模版参数，表示任何可以调用的对象类型，这个对象类型可以是普通函数、函数指针、lambda表达式、仿函数（重载了operator（）的类）
        template <typename Func, typename... Ts>
        auto submit(Func &&func, Ts &&...params)
            -> std::future<typename std::result_of<Func(Ts...)>::type>
        {
            auto execute = std::bind(std::forward<Func>(func), std::forward<Ts>(params)...);

            using ReturnType = typename std::result_of<Func(Ts...)>::type;
            using PackagedTask = std::packaged_task<ReturnType()>;

            auto task = std::make_shared<PackagedTask>(std::move(execute));
            auto result = task->get_future();

            MutexGuard guard(mutex_);
            assert(!quit_);

            tasks_.emplace([task]()
                           { (*task)(); });
            if (idleThreads_ > 0)
            {
                cv_.notify_one();
            }
            else if (currentThreads_ < maxThreads_)
            {
                Thread t(&ThreadPool::worker, this);
                assert(threads_.find(t.get_id()) == threads_.end());
                threads_[t.get_id()] = std::move(t);
                ++currentThreads_;
            }

            return result;
        }

        size_t threadsNum() const
        {
            MutexGuard guard(mutex_);
            return currentThreads_;
        }

    private:
        void worker()
        {
            while (true)
            {
                Task task;
                {
                    UniqueLock uniqueLock(mutex_);
                    ++idleThreads_;
                    auto hasTimedout = !cv_.wait_for(uniqueLock,
                                                     std::chrono::seconds(WAIT_SECONDS),
                                                     [this]()
                                                     {
                                                         return quit_ || !tasks_.empty();
                                                     });
                    --idleThreads_;
                    if (tasks_.empty())
                    {
                        if (quit_)
                        {
                            --currentThreads_;
                            return;
                        }
                        if (hasTimedout)
                        {
                            --currentThreads_;
                            joinFinishedThreads();
                            finishedThreadIDs_.emplace(std::this_thread::get_id());
                            return;
                        }
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        }

        void joinFinishedThreads()
        {
            while (!finishedThreadIDs_.empty())
            {
                auto id = std::move(finishedThreadIDs_.front());
                finishedThreadIDs_.pop();
                auto iter = threads_.find(id);

                assert(iter != threads_.end());
                assert(iter->second.joinable());

                iter->second.join();
                threads_.erase(iter);
            }
        }

        static constexpr size_t WAIT_SECONDS = 2;

        bool quit_;
        size_t currentThreads_;
        size_t idleThreads_;
        size_t maxThreads_;

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::queue<Task> tasks_;
        std::queue<ThreadID> finishedThreadIDs_;
        std::unordered_map<ThreadID, Thread> threads_;
    };

    constexpr size_t ThreadPool::WAIT_SECONDS;

} // namespace dpool

#endif /* THREADPOOL_H */
