#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <stack>
#include <vector>
#include <chrono>
#include <cassert>
#include <memory>
#include <pcosynchro/pcohoaremonitor.h>
#include <pcosynchro/pcologger.h>
#include <pcosynchro/pcothread.h>
#include <pcosynchro/pcomutex.h>
#include <pcosynchro/pcoconditionvariable.h>
#include <queue>
#include <string>
#include <unordered_set>

class Runnable {
public:
    /*
    * An empy virtual destructor
    */
    virtual ~Runnable() = default;

    /*
    * Function executing the Runnable task.
    */
    virtual void run() = 0;

    /*
    * Function that can be called from the outside, to ask for the cancellation of the runnable.
    * Shall be called by the threadpool if the Runnable is not started.
    */
    virtual void cancelRun() = 0;

    /*
    * Simply retrieve an identifier for this runnable
    */
    virtual std::string id() = 0;
};

class ThreadPool : PcoHoareMonitor {
public:
    ThreadPool(int maxThreadCount, int maxNbWaiting, std::chrono::milliseconds idleTimeout)
        : maxThreadCount(maxThreadCount), maxNbWaiting(maxNbWaiting), idleTimeout(idleTimeout), idleThreads(0), stop(false), managerThread(&ThreadPool::removeThread, this) {}

    /**
     * @brief clears up all threads before finally terminating the thread pool
     */
    ~ThreadPool() {
        // Set termination variable
        monitorIn();
        stop = true;
        // Notify all threads so they terminate
        for (auto& thread : threads) {
            signal(workCondition);
        }
        // Notify manager thread so it terminates
        signal(cleanupCondition);
        monitorOut();


        // Join all worker threads
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        // Join manager thread
        managerThread.join();
    }

    /*
     * Start a runnable. 
     * - If a thread in the pool is available, assign the runnable to it. 
     * - If no thread is available but the pool can grow, create a new pool thread and assign the runnable to it. 
     * - If no thread is available and the pool is at max capacity and there are less than maxNbWaiting threads waiting,
     * block the caller until a thread becomes available again.
     * - Else do not run the runnable.
     * 
     * If the runnable has been started, returns true, and else (the last case), return false.
     */
    bool start(std::unique_ptr<Runnable> runnable) {

        monitorIn();

        // Check if a task can be added
        if (queue.size() < maxNbWaiting || idleThreads > 0 || threads.size() < maxThreadCount) {
            // Add task to queue
            queue.push(std::move(runnable));
            // Check if available thread
            if (idleThreads > 0) {
                signal(workCondition);
            } 
            // Check if another thread can be added
            else if (threads.size() < maxThreadCount) {
                threads.emplace_back(&ThreadPool::workerThread, this);
                signal(workCondition);
            }
            monitorOut();
            return true;
        }

        // Cancel the runnable as the queue is full
        runnable->cancelRun();
        monitorOut();
        return false;
    }

    /* Returns the number of currently running threads. 
     * They do not need to be executing a task, just alive.
     */
    size_t currentNbThreads() {
        // Retrieve the number of threads currently in the list
        monitorIn();
        size_t count = threads.size();
        monitorOut();

        return count;
    }

private:

    /**
     * @brief function continually executed by the threads to handle tasks
     */
    void workerThread() {        
        // Keep handling tasks until explicitly stopped
        while (true) {
            // printf("Starting the worker thread again\n");
            monitorIn();
            // printf("Starting the worker thread again2\n");

            // Augment the amount of idle threads so it will be considered for future tasks
            ++idleThreads;

            auto startTime = std::chrono::steady_clock::now();

            // While termination has not been requested
            while (!stop) {
                if (!queue.empty()) {
                    std::unique_ptr<Runnable> task = std::move(queue.front());
                    queue.pop();
                    --idleThreads;
                    monitorOut();

                    // Run the task
                    task->run();
                    monitorIn();
                    break;
                }

                // Check for timeout
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime) >= idleTimeout) {
                    --idleThreads;
                    timedOutThreads.insert(std::this_thread::get_id());
                    // Notify that thread can be removed
                    signal(cleanupCondition);
                    monitorOut();
                    return;
                }

                // Wait for new work or signal to stop
                // printf("About to wait for work\n");
                wait(workCondition);
                // printf("Just got notified of work\n");
            }

            // If termination was signaled and there are no tasks left
            if (stop && queue.empty()) {
                --idleThreads;
                monitorOut();
                // printf("Exiting thread after stop\n");
                return;
            }
            monitorOut();
        }
    }

    /**
     * @brief function run continuously by the manager thread to remove timed out threads
     */
    void removeThread() {
        while (true) {
            monitorIn();

            // If stop is true and no timed-out threads exist, exit the manager thread
            if (stop && timedOutThreads.empty()) {
                monitorOut();
                return;
            }

            // Wait for cleanupCondition or simulate a timeout
            auto startTime = std::chrono::steady_clock::now();
            while (timedOutThreads.empty() && !stop) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime) >= std::chrono::milliseconds(500)) {
                    break; // Simulate timeout
                }
                wait(cleanupCondition);
            }

            // Clean up timed-out threads
            for (auto it = threads.begin(); it != threads.end();) {
                if (timedOutThreads.find(it->get_id()) != timedOutThreads.end()) {
                    if (it->joinable()) {
                        it->join();
                        it = threads.erase(it);
                    }
                } else {
                    ++it;
                }
            }

            timedOutThreads.clear();
            monitorOut();
        }

        // while (true) {
        //     monitorIn();
        //     wait(cleanupCondition);


        //     if (stop) {
        //         monitorOut();
        //         return;
        //     }

        //     for (auto it = threads.begin(); it != threads.end();) {
        //         if (timedOutThreads.find(it->get_id()) != timedOutThreads.end()) {
        //             if (it->joinable()) {
        //                 it->join();
        //                 it = threads.erase(it);
        //             }
        //         }
        //         else {
        //             ++it;
        //         }
        //     }

        //     timedOutThreads.clear();
        //     monitorOut();
        // }
    }

    size_t maxThreadCount;
    size_t maxNbWaiting;
    std::chrono::milliseconds idleTimeout;

    std::thread managerThread; // Thread that manages the removal of inactive threads
    std::vector<std::thread> threads; // Thread pool
    std::queue<std::unique_ptr<Runnable>> queue; // List of waiting runnables
    std::unordered_set<std::thread::id> timedOutThreads; // List of threads that have timed out
    size_t idleThreads; // Keep count of number of inactive threads
    bool stop;  // Signals termination

    Condition workCondition;
    Condition cleanupCondition;
};

#endif // THREADPOOL_H
