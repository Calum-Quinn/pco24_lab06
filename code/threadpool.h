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
        : maxThreadCount(maxThreadCount), maxNbWaiting(maxNbWaiting), idleTimeout(idleTimeout), idleThreads(0), stop(false) {}

    ~ThreadPool() {
        // TODO : End smoothly

        // Set terminating variable
        mutex.lock();
        stop = true;
        mutex.unlock();

        // Notify all threads so they terminate
        condition.notifyAll();

        // Join all threads
        for (auto& thread : threads) {
            if (thread.joinable()) {
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

        // printf("Starting\n");

        mutex.lock();
        // Check if there is an available thread
        if (idleThreads > 0) {
            // printf("There are available threads\n");
            // Add the runnable to the queue
            queue.push(std::move(runnable));
            // Notify a thread
            condition.notifyOne();
            mutex.unlock();
            // Return true
            return true;
        }
        // Else check if the pool can grow
        else if (threads.size() < maxThreadCount) {
            // printf("The pool can grow\n");
            // Create a new thread
            threads.emplace_back(&ThreadPool::workerThread, this);
            // Add the runnable to the queue
            queue.push(std::move(runnable));
            // Notify a thread
            condition.notifyOne();
            mutex.unlock();
            // Return true
            return true;
        }
        // Else check if less than max are waiting
        else if (queue.size() < maxNbWaiting) {
            // printf("Need to wait, queue size: %d\n", queue.size());
            // Add the runnable to the queue
            queue.push(std::move(runnable));
            // ??? Block caller until a thread is available ???
            condition.notifyOne();
            mutex.unlock();
            // Return true
            return true;
        }
        // Else
            // Cancel the runnable
        runnable->cancelRun();
        mutex.unlock();

            // Return false
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

    void workerThread() {
        while (true) {

            // printf("Thread starting run\n");

            std::unique_ptr<Runnable> task;
            mutex.lock();
            // Initialise timeout token
            // threadTimeouts[std::this_thread::get_id()] = false;
            ++idleThreads;

            while (!stop && queue.empty()) {
                // mutex.unlock();
                // removeThreads();
                // mutex.lock();


                // Wait for timeout, skip if task received
                if (!condition.waitForSeconds(&mutex, idleTimeout.count() / 1000)) {
                    // Start procedure to remove thread once it terminates
                    removeThread(std::this_thread::get_id()); // Will wait for signal
                    // Terminate the thread and notify of timeout
                    // threadTimeouts[std::this_thread::get_id()] = true;
                    --idleThreads;
                    cleanup.notifyOne(); // Signal that thread can be removed
                    mutex.unlock();
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

            if (task) {
                // printf("About to run runnable\n");
                task->run();
                // printf("Just finished running id: %s\n", runnable->id().c_str());
            }
        }
    }

    /**
     * @brief removes all threads that timed out
     */
    // void removeThreads() {
    //     mutex.lock();
    //     for (auto it = threadTimeouts.begin(); it != threadTimeouts.end();) {
    //         // Check if the thread has timed out
    //         if (it->second) {
    //             // Find the thread in the threads vector
    //             auto threadIt = std::find_if(threads.begin(), threads.end(), [&](std::thread& t) {
    //                 return t.get_id() == it->first;
    //             });

    //             // If the thread is found and is joinable, join it and remove from the threads vector
    //             if (threadIt != threads.end() && threadIt->joinable()) {
    //                 threadIt->join();
    //                 // Remove the thread from the pool
    //                 threads.erase(threadIt);
    //             }

    //             // Remove from the timeout tracking map
    //             it = threadTimeouts.erase(it);
    //         } else {
    //             // Move to the next element if not timed out
    //             ++it;
    //         }
    //     }
    //     mutex.unlock();
    // }


    void removeThread(std::thread::id id) {
        mutex.lock();
        // Wait for thread to need cleaning up
        cleanup.wait(&mutex);

        // Find the thread in the list
        auto threadIt = std::find_if(threads.begin(), threads.end(), [&](std::thread& t) {
            return t.get_id() == id;
        });

        // If the thread is found and is joinable, join it and remove from the threads vector
        if (threadIt != threads.end() && threadIt->joinable()) {
            threadIt->join();
            // Remove the thread from the pool
            threads.erase(threadIt);
        }

        mutex.unlock();
    }

    size_t maxThreadCount;
    size_t maxNbWaiting;
    std::chrono::milliseconds idleTimeout;

    std::vector<std::thread> threads; // Thread pool
    // std::unordered_map<std::thread::id, bool> threadTimeouts; // Map to store whether threads have timed out and need to be removed
    std::queue<std::unique_ptr<Runnable>> queue; // List of waiting runnables
    PcoMutex mutex;
    PcoConditionVariable condition; // To make threads wait for new tasks
    PcoConditionVariable cleanup; // To trigger the removal of timed out threads
    size_t idleThreads; // Keep count of number of inactive threads
    bool stop;  // Signals termination
};

#endif // THREADPOOL_H
