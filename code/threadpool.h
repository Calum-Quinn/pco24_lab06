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

        printf("Just started destructor\n");

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
            printf("Going to wake thread for destruction\n");
            // thread->requestStop();
            signal(taskOrTimeout);
        }

        monitorOut();

        printf("Going to join all threads\n");

        // Join all worker threads
        for (auto& thread : threads) {
            thread->join();
        }

        printf("Joined all worker threads\n");

        // Join manager thread
        managerThread.join();

        printf("Joined manger thread\n");
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
            printf("Just added new thread\n");
            ++running;
        }

        queue.push(std::move(runnable));
        printf("Going to wake thread for task\n");
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
            monitorIn();

            // Wait for a task if the queue is empty
            while (queue.empty() && !stop) {
                printf("Going to wait\n");
                // Note beginning of waiting time
                auto now = std::chrono::steady_clock::now();
                times.push({threads.front(), now});
                signal(cleanupCondition);
                wait(taskOrTimeout);
                // Remove the waiting time so as not to be removed
                times.pop();
                printf("Just removed times entry\n");
                if (PcoThread::thisThread()->stopRequested()) {
                    printf("About to quit thread after stop requested\n");
                    return;
                }
            }

            // Check if termination was requested or if there are no tasks available and quit
            if (stop || queue.empty()) {
                printf("About to stop\n");
                --running;
                monitorOut();
                return;
            }

            // Retrieve the next task and execute it
            std::unique_ptr<Runnable> task = std::move(queue.front());
            queue.pop();

            printf("About to run task\n");
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
            if (times.empty()) {
                printf("Going to wait for cleanup request\n");
                wait(cleanupCondition);
                printf("Received cleanup request\n");

                // Check if awakened for termination
                if (PcoThread::thisThread()->stopRequested()) {
                    printf("Going to stop managerThread\n");
                    monitorOut();
                    return;
                }
            }

            // Calculate remaining time to timeout
            auto tilTimeout = idleTimeout - std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - times.front().second);
            monitorOut();

            // Wait until potential timeout
            PcoThread::usleep(tilTimeout.count() * (uint64_t) 1000);

            monitorIn();

            if (PcoThread::thisThread()->stopRequested()) {
                printf("Going to stop managerThread\n");
                monitorOut();
                return;
            }

            // Check that the first time is past timeout
            while (!times.empty() && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - times.front().second) > tilTimeout) {
                // Signal end of thread and wait for it to end
                printf("Going to wake thread for timeout\n");
                PcoThread* toRemove = times.front().first;
                times.front().first->requestStop();
                signal(taskOrTimeout);
                printf("About to join timed out thread\n");
                toRemove->join();
                printf("Just joined timed out thread\n");

                // Remove the thread from the list
                auto it = std::find(threads.begin(), threads.end(), toRemove);
                if ( it != threads.end()) {
                    threads.erase(it);
                }
                printf("Erased timed out thread\n");
            }
            monitorOut();
        }
    }

    size_t maxThreadCount;
    size_t maxNbWaiting;
    std::chrono::milliseconds idleTimeout;

    PcoThread managerThread; // Thread that manages the removal of inactive threads
    std::queue<std::unique_ptr<Runnable>> queue; // List of waiting runnables
    size_t running; // Keep track of how many threads are currently running
    bool stop;  // Signals termination
    Condition taskOrTimeout; // Checker whether there are tasks waiting
    Condition cleanupCondition; // To wait for threads to time out and need removing

    std::vector<PcoThread*> threads;
    std::queue<std::pair<PcoThread*, std::chrono::steady_clock::time_point>> times;
};

#endif // THREADPOOL_H
