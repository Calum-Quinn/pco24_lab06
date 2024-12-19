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
        : maxThreadCount(maxThreadCount), maxNbWaiting(maxNbWaiting), idleTimeout(idleTimeout), running(0), stop(false), cleaning(false), managerThread(&ThreadPool::timing, this) {
            threads.reserve(maxThreadCount);
        }

    /**
     * @brief clears up all threads before finally terminating the thread pool
     */
    ~ThreadPool() {
        monitorIn();

        // Make sure the timeout manager is not currently removing a thread
        while (cleaning) {
            wait(cleaner);
        }

        // Set general termination variable
        stop = true;

        // Cancel any remaining runnables to avoid leaving them in an undefined state
        while (!queue.empty()) {
            queue.front()->cancelRun();
            queue.pop();
        }

        // Notify manager thread so it terminates
        managerThread.requestStop();
        signal(cleanupCondition);

        // Notify all worker threads so they terminate
        for (auto& thread : threads) {
            signal(taskOrTimeout);
        }

        monitorOut();

        // Join all worker threads
        for (auto& thread : threads) {
            thread->join();
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

        // Check if the queue is full
        if (queue.size() >= maxNbWaiting) {
            // Cancel the task
            runnable->cancelRun();
            monitorOut();
            return false;
        }

        // Check if the pool can and needs to grow
        if (threads.size() < maxThreadCount && running == threads.size()) {
            // Add a new thread to the pool
            threads.emplace_back(new PcoThread(&ThreadPool::workerThread, this));
            ++running;
        }

        // Add the task to the queue and notify a possible waiting thread
        queue.push(std::move(runnable));
        signal(taskOrTimeout);
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
        // Wait for the thread-specific value so they can be removed dynamically instead of only all at once
        while (!PcoThread::thisThread()->stopRequested()) {
            monitorIn();

            // If the queue is empty, wait for a task or timeout
            while (queue.empty() && !stop) {
                // Note beginning of waiting time
                auto now = std::chrono::steady_clock::now();
                times.push({PcoThread::thisThread(), now});

                // Notify the timeout manager so it calculates when to remove the thread in case of timeout
                signal(cleanupCondition);
                wait(taskOrTimeout);

                // If the first time is still this thread, it did not time out
                if (times.front().first == PcoThread::thisThread()) {
                    // Remove the waiting time so as not to be removed because of a false timeout
                    times.pop();
                }
                else {
                    // If the thread timed out, exit so it can be removed from the pool
                    --running;
                    monitorOut();
                    return;
                }
            }

            // Check if termination was requested or if there are no tasks available and quit
            if (stop || queue.empty()) {
                --running;
                monitorOut();
                return;
            }

            // Retrieve the next task and execute it
            auto task = std::move(queue.front());
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
            if (times.empty() && !stop) {
                wait(cleanupCondition);
            }

            // Check if awakened for termination
            if (stop) {
                monitorOut();
                return;
            }

            // Calculate time since the thread has been waiting for a task
            auto now = std::chrono::steady_clock::now();
            auto next = times.front();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - next.second);

            // If the thread has timed out
            if (elapsed >= idleTimeout) {
                // Note that a thread is currently being removed so the destructor does not join it
                cleaning = true;
                PcoThread* toRemove = next.first;

                // Remove the entry in the timing list
                times.pop();

                // Notify the thread that it needs to terminate
                toRemove->requestStop();
                signal(taskOrTimeout);

                monitorOut();
                toRemove->join();
                monitorIn();

                // Remove the terminated thread from the pool
                auto it = std::find(threads.begin(), threads.end(), toRemove);
                if (it != threads.end()) {
                    threads.erase(it);
                }
                // Notify the destructor it can now join all remaining threads
                cleaning = false;
                signal(cleaner);
                monitorOut();
            }
            else {
                // If the next thread has not yet timed out, sleep until the next probable timeout
                auto sleep = idleTimeout - elapsed;
                monitorOut();
                PcoThread::usleep(sleep.count() * 1000);
            }
        }
    }

    size_t maxThreadCount;
    size_t maxNbWaiting;
    std::chrono::milliseconds idleTimeout;

    PcoThread managerThread; // Thread that manages the removal of inactive threads
    std::queue<std::unique_ptr<Runnable>> queue; // List of waiting runnables
    std::vector<PcoThread*> threads; // Thread pool
    std::queue<std::pair<PcoThread*, std::chrono::steady_clock::time_point>> times; // Keeps track of which threads have been waiting since when
    size_t running; // Keep track of how many threads are currently running
    bool stop;  // Signals general termination
    bool cleaning; // Signals that a timed out thread is being removed and therefore should not be joined by the destructor
    Condition taskOrTimeout; // To wait for a new task or until timeout
    Condition cleanupCondition; // To wait for threads to time out and need removing
    Condition cleaner; // So that the timeout manager can signal the destructor once a thread has been removed
};

#endif // THREADPOOL_H
