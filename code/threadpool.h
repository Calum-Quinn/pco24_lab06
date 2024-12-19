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
        : maxThreadCount(maxThreadCount), maxNbWaiting(maxNbWaiting), idleTimeout(idleTimeout), running(0), stop(false), cleaning(false), managerThread(&ThreadPool::timing, this) {}

    /**
     * @brief clears up all threads before finally terminating the thread pool
     */
    ~ThreadPool() {
        monitorIn();

        // printf("Just started destructor\n");

        // Make sure the timeout manager is not currently removing a thread
        while (cleaning) {
            wait(cleaner);
        }

        // Set termination variable
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
            // printf("Going to wake thread for destruction\n");
            signal(taskOrTimeout);
        }

        monitorOut();

        // printf("Going to join all threads\n");

        // Join all worker threads
        for (auto& thread : threads) {
            // printf("Going to join worker thread\n");
            thread->join();
            // printf("Joined worker thread\n");
        }

        // printf("Joined all worker threads\n");

        // Join manager thread
        managerThread.join();

        // printf("Joined manger thread\n");
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
            // printf("Just added new thread\n");
            ++running;
        }

        queue.push(std::move(runnable));
        // printf("Going to wake thread for task\n");
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
        while (!PcoThread::thisThread()->stopRequested()) {
            // printf("Starting worker thread loop\n");
            monitorIn();

            // Wait for a task if the queue is empty
            while (queue.empty() && !stop) {
                // printf("Going to wait\n");
                // Note beginning of waiting time
                auto now = std::chrono::steady_clock::now();
                times.push({PcoThread::thisThread(), now});
                signal(cleanupCondition);
                wait(taskOrTimeout);
                // Remove the waiting time so as not to be removed
                if (times.front().first == PcoThread::thisThread()) {
                    times.pop();
                }
                // If the entry is no longer in the list, it was removed by the timeout manager
                else {
                    --running;
                    monitorOut();
                    return;
                }
            }

            // Check if termination was requested or if there are no tasks available and quit
            if (stop || queue.empty()) {
                // printf("About to stop\n");
                --running;
                monitorOut();
                return;
            }

            // Retrieve the next task and execute it
            auto task = std::move(queue.front());
            queue.pop();

            // printf("About to run task\n");
            monitorOut();
            task->run();
            // printf("Finished running task\n");
        }
    }

    /**
     * @brief function run continuously by the manager thread to remove timed out threads
     */
    void timing() {
        while(!PcoThread::thisThread()->stopRequested()) {
            monitorIn();

            // Wait for the next thread to need cleaning up
            if (times.empty()) {
                // printf("Going to wait for cleanup request\n");
                wait(cleanupCondition);
                // printf("Received cleanup request\n");

                // Check if awakened for termination
                if (stop) {
                    // printf("Going to stop managerThread\n");
                    monitorOut();
                    return;
                }
            }

            // Calculate remaining time to timeout
            auto now = std::chrono::steady_clock::now();
            auto next = times.front();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - next.second);

            if (elapsed >= idleTimeout) {
                // printf("Cleaning started\n");
                cleaning = true;
                PcoThread* toRemove = next.first;

                times.pop();
                toRemove->requestStop();
                signal(taskOrTimeout);

                monitorOut();

                // printf("About to join a thread after timeout\n");
                toRemove->join();
                // printf("Joined a thread after timeout\n");

                monitorIn();

                auto it = std::find(threads.begin(), threads.end(), toRemove);
                if (it != threads.end()) {
                    // printf("Erasing timed out thread\n");
                    threads.erase(it);
                }
                cleaning = false;
                // printf("Cleaning finished\n");
                signal(cleaner);
                monitorOut();
            }
            else {
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
    size_t running; // Keep track of how many threads are currently running
    bool stop;  // Signals termination
    bool cleaning; // Signals that a timed out thread is being removed and therefore should not be joined by the destructor
    Condition taskOrTimeout; // Checker whether there are tasks waiting
    Condition cleanupCondition; // To wait for threads to time out and need removing
    Condition cleaner; // So that the timeout manager can signal the destructor once a thread has been removed

    std::vector<PcoThread*> threads;
    std::queue<std::pair<PcoThread*, std::chrono::steady_clock::time_point>> times;
};

#endif // THREADPOOL_H
