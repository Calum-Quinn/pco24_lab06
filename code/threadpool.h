#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <stack>
#include <vector>
#include <chrono>
#include <cassert>
#include <pcosynchro/pcologger.h>
#include <pcosynchro/pcothread.h>
#include <pcosynchro/pcomutex.h>
#include <pcosynchro/pcoconditionvariable.h>

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
        : maxThreadCount(maxThreadCount), maxNbWaiting(maxNbWaiting), idleTimeout(idleTimeout) {}

    ~ThreadPool() {
        // TODO : End smoothly

        mutex.lock();
        stop = true;
        mutex.unlock();
        condition.notifyAll();
        for (auto& thread : threads) {
            if (tread.joinable()) {
                thread.join();
            }
        }
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
        // TODO

        mutex.lock();
        // Check if there is an available thread
        if (idleThreads) {
            // Add the runnable to the queue
            queue.push(std::move(runnable));
            // Notify a thread
            mutex.unlock();
            condition.notifyOne();
            // Return true
            return true;
        }
        // Else check if the pool can grow
        else if (threads.size() < maxThreadCount) {
            // Create a new thread
            threads.emplace_back(&ThreadPool::workerThread, this);
            // Add the runnable to the queue
            queue.push(std::move(runnable));
            // Notify a thread
            mutex.unlock();
            condition.notifyOne();
            // Return true
            return true;
        }
        // Else check if less than max are waiting
        else if (queue.size() < maxNbWaiting) {
            // Block caller until a thread is available
            wait(condition); // NECESSARY? JUST ADD TO QUEUE?? BUT THEN WHERE USE MONITOR???
            // Add the runnable to the queue
            queue.push(std::move(runnable));
            // Notify a thread
            mutex.unlock();
            condition.notifyOne();
            // Return true
            return true;
        }
        // Else
            // Return false
        mutex.unlock();
        return false;
    }

    /* Returns the number of currently running threads. 
     * They do not need to be executing a task, just alive.
     */
    size_t currentNbThreads() {
        // TODO

        mutex.lock();
        size_t count = threads.size();
        mutex.unlock();

        return count;
    }

private:

    size_t maxThreadCount;
    size_t maxNbWaiting;
    std::chrono::milliseconds idleTimeout;

    std::vector<std::thread> threads; // Thread pool
    std::queue<std::unique_ptr<Runnable>> queue; // List of waiting runnables
    PcoMutex mutex;
    PcoConditionVariable condition;
    size_t idleThreads; // Keep count of number of inactive threads
    bool stop;  // Signals termination
};

#endif // THREADPOOL_H
