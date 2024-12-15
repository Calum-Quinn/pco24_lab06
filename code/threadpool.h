#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <stack>
#include <vector>
#include <chrono>
#include <cassert>
#include <memory>
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

class ThreadPool {
public:
    ThreadPool(int maxThreadCount, int maxNbWaiting, std::chrono::milliseconds idleTimeout)
        : maxThreadCount(maxThreadCount), maxNbWaiting(maxNbWaiting), idleTimeout(idleTimeout), idleThreads(0), stop(false), managerThread(&ThreadPool::removeThread, this) {}

    /**
     * @brief clears up all threads before finally terminating the thread pool
     */
    ~ThreadPool() {
        // Set termination variable
        mutex.lock();
        stop = true;
        mutex.unlock();

        // Notify all threads so they terminate
        condition.notifyAll();
        // Notify manager thread so it terminates
        cleanup.notifyOne();

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
        mutex.lock();
        // Check if there is an available thread
        if (idleThreads > 0) {
            // Add the runnable to the queue and notify a waiting thread
            queue.push(std::move(runnable));
            mutex.unlock();
            condition.notifyOne();
            return true;
        }
        // Else check if the pool can grow
        else if (threads.size() < maxThreadCount) {
            // Create a new thread
            threads.emplace_back(&ThreadPool::workerThread, this);
            // Add the runnable to the queue and notify a waiting thread
            queue.push(std::move(runnable));
            mutex.unlock();
            condition.notifyOne();
            return true;
        }
        // Else check if less than max are waiting
        else if (queue.size() < maxNbWaiting) {
            // Add the runnable to the queue
            queue.push(std::move(runnable));
            mutex.unlock();
            return true;
        }

        // Cancel the runnable as the queue is full
        runnable->cancelRun();
        mutex.unlock();

        return false;
    }

    /* Returns the number of currently running threads. 
     * They do not need to be executing a task, just alive.
     */
    size_t currentNbThreads() {
        // Retrieve the number of threads currently in the list
        mutex.lock();
        size_t count = threads.size();
        mutex.unlock();

        return count;
    }

private:

    /**
     * @brief function continually executed by the threads to handle tasks
     */
    void workerThread() {        
        // Keep handling tasks until explicitly stopped
        while (true) {
            std::unique_ptr<Runnable> task;

            mutex.lock();

            // Augment the amount of idle threads so it will be considered for future tasks
            ++idleThreads;

            // Wait for there to be available tasks or for termination to be requested
            while (!stop && queue.empty()) {
                // Wait for timeout, skip if task received
                if (!condition.waitForSeconds(&mutex, idleTimeout.count() / 1000)) {
                    // Indicate that the thread can be removed once signaled
                    timedOutThreads.insert(std::this_thread::get_id());

                    // Remove the thread from the list of idle threads to avoid it being considered for tasks
                    --idleThreads;

                    // Signal that the thread can be removed
                    mutex.unlock();
                    cleanup.notifyOne();
                    return;
                }
            }

            // Check is termination was requested and the queue is empty
            if (stop && queue.empty()) {
                --idleThreads;
                mutex.unlock();
                return;
            }

            // Choose the next task in the queue
            task = std::move(queue.front());
            queue.pop();
            --idleThreads;
            mutex.unlock();

            // If it actually received a task, run it
            if (task) {
                task->run();
            }
        }
    }

    /**
     * @brief function run continuously by the manager thread to remove timed out threads
     */
    void removeThread() {
        // Run until the thread pool terminates
        while (!stop) {
            mutex.lock();
            // Wait for signal that there are threads to clean up
            cleanup.wait(&mutex);

            // Check which threads have timed out since last check
            for (auto it = timedOutThreads.begin(); it != timedOutThreads.end();) {
                // Find the thread in the threads vector
                auto threadIt = std::find_if(threads.begin(), threads.end(), [&](std::thread& t) {
                    return t.get_id() == *it;
                });

                // If the thread is found and is joinable, join it and remove from the threads vector
                if (threadIt != threads.end() && threadIt->joinable()) {
                    threadIt->join();
                    threads.erase(threadIt);
                }

                // Remove from the list of timed out threads and advance the iterator
                it = timedOutThreads.erase(it);
            }
            mutex.unlock();
        }
    }

    size_t maxThreadCount;
    size_t maxNbWaiting;
    std::chrono::milliseconds idleTimeout;

    std::thread managerThread; // Thread that manages the removal of inactive threads
    std::vector<std::thread> threads; // Thread pool
    std::queue<std::unique_ptr<Runnable>> queue; // List of waiting runnables
    std::unordered_map<std::thread::id, bool> threadTimeouts; // Map to store whether threads have timed out and need to be removed
    std::unordered_set<std::thread::id> timedOutThreads; // List of threads that have timed out
    PcoMutex mutex; // Locking the shared variables (e.g. idleThreads, stop,...)
    PcoConditionVariable condition; // To make threads wait for new tasks
    PcoConditionVariable cleanup; // To trigger the removal of timed out threads
    size_t idleThreads; // Keep count of number of inactive threads
    bool stop;  // Signals termination
};

#endif // THREADPOOL_H
