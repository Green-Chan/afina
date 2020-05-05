#ifndef AFINA_CONCURRENCY_EXECUTOR_H
#define AFINA_CONCURRENCY_EXECUTOR_H

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace Afina {
namespace Concurrency {

/**
 * # Thread pool
 */
class Executor {
    enum class State {
        // Threadpool is fully operational, tasks could be added and get executed
        kRun,

        // Threadpool is on the way to be shutdown, no ned task could be added, but existing will be
        // completed as requested
        kStopping,

        // Threadpool is stopped or is not started yet
        kStopped
    };

public:
    Executor(std::size_t low_watermark, std::size_t high_watermark, std::size_t max_queue_size,
             std::chrono::milliseconds idle_time)
        : low_watermark(low_watermark), high_watermark(high_watermark), max_queue_size(max_queue_size),
          idle_time(idle_time), threads_cnt(0), free_threads(0), state(State::kStopped) {}
    ~Executor() { Stop(); }

    /**
     * Start thread pool
     */
    void Start() {
        std::unique_lock<std::mutex> lock(this->mutex);
        if (state == State::kRun) {
            return;
        }
        while (state != State::kStopped) {
            stop_condition.wait(lock);
        }
        for (size_t i = 0; i < low_watermark; i++) {
            std::thread new_thread(&Executor::perform, this);
            new_thread.detach();
        }
        threads_cnt = low_watermark;
        free_threads = low_watermark;
        state = State::kRun;
    }

    /**
     * Signal thread pool to stop, it will stop accepting new jobs and close threads just after each become
     * free. All enqueued jobs will be complete.
     *
     * In case if await flag is true, call won't return until all background jobs are done and all threads are stopped
     */
    void Stop(bool await = false) {
        std::unique_lock<std::mutex> lock(this->mutex);
        if (state == State::kStopped) {
            return;
        }

        if (state == State::kRun) {
            state = State::kStopping;
            if (threads_cnt != 0) {
                empty_condition.notify_all();
            } else {
                state = State::kStopped;
            }
        }

        if (await) {
            while (state != State::kStopped) {
                stop_condition.wait(lock);
            }
        }
    }

    /**
     * Add function to be executed on the threadpool. Method returns true in case if task has been placed
     * onto execution queue, i.e scheduled for execution and false otherwise.
     *
     * That function doesn't wait for function result. Function could always be written in a way to notify caller about
     * execution finished by itself
     */
    template <typename F, typename... Types> bool Execute(F &&func, Types... args) {
        // Prepare "task"
        auto exec = std::bind(std::forward<F>(func), std::forward<Types>(args)...);

        std::unique_lock<std::mutex> lock(this->mutex);
        if (state != State::kRun || tasks.size() >= max_queue_size) {
            return false;
        }

        // Enqueue new task
        tasks.push_back(exec);
        if (free_threads >= tasks.size()) {
            empty_condition.notify_one();
        } else {
            if (threads_cnt < high_watermark) {
                threads_cnt++;
                free_threads++;
                lock.unlock();
                std::thread new_thread(&Executor::perform, this);
                new_thread.detach();
            }
        }
        return true;
    }

private:
    // No copy/move/assign allowed
    Executor(const Executor &);            // = delete;
    Executor(Executor &&);                 // = delete;
    Executor &operator=(const Executor &); // = delete;
    Executor &operator=(Executor &&);      // = delete;

    /**
     * Main function that all pool threads are running. It polls internal task queue and execute tasks
     */
    void perform() {
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            if (tasks.empty()) {
                if (state == State::kStopping) {
                    // stopping thread after while
                    break;
                } else if (empty_condition.wait_for(lock, idle_time) == std::cv_status::timeout &&
                           threads_cnt > low_watermark) {
                    // too much free threads
                    // stopping thread after while
                    break;
                } else {
                    // timeout in low_watermark threads or thread was notified
                    // (we'll check why it was notified at the next iteration of while
                    continue;
                }
            } else {
                // there is a task
                auto task = tasks.front();
                tasks.pop_front();
                free_threads--;
                lock.unlock();
                try {
                    task();
                } catch (...) {
                    std::cerr << "Error in executing function" << std::endl;
                    std::abort();
                }
                lock.lock();
                free_threads++;
            }
        }
        // stopping thread
        if (--threads_cnt == 0 && state == State::kStopping) {
            state = State::kStopped;
            stop_condition.notify_all();
        }
        free_threads--;
    }

    const std::size_t low_watermark, high_watermark, max_queue_size;
    const std::chrono::milliseconds idle_time;

    /**
     * Mutex to protect state below from concurrent modification
     */
    std::mutex mutex;

    /**
     * Conditional variable to await new data in case of empty queue
     */
    std::condition_variable empty_condition;

    // Conditional variable to wait until all threads are stopped
    std::condition_variable stop_condition;

    /**
     * Task queue
     */
    std::deque<std::function<void()>> tasks;

    /**
     * Flag to stop bg threads
     */
    State state;

    std::size_t threads_cnt, free_threads;
};

} // namespace Concurrency
} // namespace Afina

#endif // AFINA_CONCURRENCY_EXECUTOR_H
