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
        : maxThreadCount(maxThreadCount), maxNbWaiting(maxNbWaiting), idleTimeout(idleTimeout), idleThreads(0), stop(false), managerThread(&ThreadPool::removeThread, this) {}

    ~ThreadPool() {
        // TODO : End smoothly

        // Set terminating variable
        mutex.lock();
        stop = true;
        mutex.unlock();

        // Notify all threads so they terminate
        condition.notifyAll();
        cleanup.notifyOne();

        // Join all threads
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
        // TODO

        mutex.lock();
        // Check if there is an available thread
        if (idleThreads > 0) {
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

    /**
     * @brief function continually executed by the threads to handle tasks
     */
    void workerThread() {
        std::thread::id id = std::this_thread::get_id();
        // Start procedure to remove thread if it times out
        threadTimeouts[id] == false;


        // THIS COMMAND BLOCKS THE EXECUTION OF THE THREAD !!!!!!
        // removeThread()
        // WE NEED A WAY OF WAITING THAT CAN BE SIGNALED FOR A SPECIFIC THREAD BUT ONLY WHEN THE THREAD HAS TIMED OUT
        // removeThread(id); // Will wait for signal
        
        while (true) {
            std::unique_ptr<Runnable> task;
            mutex.lock();
            // Initialise timeout token
            ++idleThreads;

            while (!stop && queue.empty()) {
                // Wait for timeout, skip if task received
                if (!condition.waitForSeconds(&mutex, idleTimeout.count() / 1000)) {
                    // Indicate that thread can be removed once signaled
                    threadTimeouts[id] = true;
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
                task->run();
            }
        }
    }


    void removeThread(/*std::thread::id id*/) {

        // TWO IDEAS TO REMOVE PROBLEM OF IDENTIFYING WHICH THREAD TERMINATED
            // JUST REMOVE ALL TERMINATED THREADS (NOT SURE IF WILL PASS TEST 5 IF THEY ARE REMOVED TO QUICKLY)
            // CREATE FIFO LIST AND REMOVE THE NEXT THREAD UPON NOTIFICATION

        // WHERE TO CALL removeThread()??? BLOCKS EXECUTION

        while (!stop) {
            mutex.lock();
            // Wait for signal that there are threads to clean up
            cleanup.wait(&mutex);

            // Check which threads have timed out since last check
            for (auto it = threadTimeouts.begin(); it != threadTimeouts.end();) {
                // Check if the thread has timed out
                if (it->second) {
                    // Find the thread in the threads vector
                    auto threadIt = std::find_if(threads.begin(), threads.end(), [&](std::thread& t) {
                        return t.get_id() == it->first;
                    });

                    // If the thread is found and is joinable, join it and remove from the threads vector
                    if (threadIt != threads.end() && threadIt->joinable()) {
                        threadIt->join(); // Wait for the thread to finish
                        threads.erase(threadIt); // Remove the thread from the pool
                    }

                    // Remove from the timeout tracking map
                    it = threadTimeouts.erase(it);
                } else {
                    ++it; // Move to the next element if not timed out
                }
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
    PcoMutex mutex;
    PcoConditionVariable condition; // To make threads wait for new tasks
    PcoConditionVariable cleanup; // To trigger the removal of timed out threads
    size_t idleThreads; // Keep count of number of inactive threads
    bool stop;  // Signals termination
};

#endif // THREADPOOL_H
