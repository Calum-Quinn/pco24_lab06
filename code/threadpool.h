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
        : maxThreadCount(maxThreadCount), maxNbWaiting(maxNbWaiting), idleTimeout(idleTimeout), running(0), stop(false), managerThread(&ThreadPool::timing, this) {}

    /**
     * @brief clears up all threads before finally terminating the thread pool
     */
    ~ThreadPool() {
        monitorIn();

        // Set termination variable
        stop = true;

        // Cancel any remaining runnables to avoid leaving them in an undefined state
        while (!queue.empty()) {
            std::unique_ptr<Runnable> task = std::move(queue.front());
            queue.pop();
            task->cancelRun();
        }

        // Notify manager thread so it terminates
        managerThread.requestStop();
        signal(cleanupCondition);

        // Notify all worker threads so they terminate
        for (auto& thread : threads) {
            signal(notEmpty);
        }

        monitorOut();

        // Join manager thread
        managerThread.join();

        // Join all worker threads
        for (auto& thread : threads) {
            thread->join();
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

        monitorIn();

        // Check if the queue is full
        if (queue.size() >= maxNbWaiting) {
            // Cancel the task
            runnable->cancelRun();
            monitorOut();
            return false;
        }

        // Check if the pool can and needs to grow
        if (threads.size() < maxThreadCount && running == threads.size()) {
            // Create a new thread
            threads.emplace_back(new PcoThread(&ThreadPool::workerThread, this));
            ++running;
        }

        queue.push(std::move(runnable));
        signal(notEmpty);
        monitorOut();
        return true;
    }

    /* Returns the number of currently running threads. 
     * They do not need to be executing a task, just alive.
     */
    size_t currentNbThreads() {
        return running;
    }

private:

    /**
     * @brief function continually executed by the threads to handle tasks
     */
    void workerThread() {
        while (!PcoThread::thisThread()->stopRequested()) {
            monitorIn();

            // Wait for a task if the queue is empty
            while (queue.empty() && !stop) {
                // Note beginning of waiting time
                startingTimes.push(std::chrono::steady_clock::now());
                signal(cleanupCondition);
                wait(notEmpty);
                // Remove the waiting time so as not to be removed
                startingTimes.pop();
            }

            // Check if termination was requested or if there are no tasks available and quit
            if (stop || queue.empty()) {
                --running;
                monitorOut();
                return;
            }

            // Retrieve the next task and execute it
            std::unique_ptr<Runnable> task = std::move(queue.front());
            queue.pop();

            monitorOut();
            task->run();
        }
    }

    /**
     * @brief function run continuously by the manager thread to remove timed out threads
     */
    void timing() {
        while(!PcoThread::thisThread()->stopRequested()) {
            monitorIn();

            // Wait for the next thread to need cleaning up
            if (startingTimes.empty()) {
                wait(cleanupCondition);

                // Check if awakened for termination
                if (PcoThread::thisThread()->stopRequested()) {
                    monitorOut();
                    return;
                }
            }

            // Calculate remaining time to timeout
            std::chrono::milliseconds tilTimeout = idleTimeout - std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startingTimes.front());
            monitorOut();

            // Wait until potential timeout
            PcoThread::usleep(tilTimeout.count() * (uint64_t) 1000);

            monitorIn();
            // Check that the first time is past timeout
            while (!startingTimes.empty() && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startingTimes.front()) > tilTimeout) {
                signal(notEmpty);
            }
            monitorOut();
        }
    }

    size_t maxThreadCount;
    size_t maxNbWaiting;
    std::chrono::milliseconds idleTimeout;

    PcoThread managerThread; // Thread that manages the removal of inactive threads
    std::vector<std::unique_ptr<PcoThread>> threads; // Thread pool
    std::queue<std::unique_ptr<Runnable>> queue; // List of waiting runnables
    std::queue<std::chrono::steady_clock::time_point> startingTimes; // Keep of track of when the threads start waiting for tasks
    size_t running; // Keep track of how many threads are currently running
    bool stop;  // Signals termination
    Condition notEmpty; // Checker whether there are tasks waiting
    Condition cleanupCondition; // To wait for threads to time out and need removing
};

#endif // THREADPOOL_H
