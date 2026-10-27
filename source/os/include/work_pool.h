#pragma once
#include "threads.h"
#include <functional>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <chrono>

namespace threads {

//A fixed set of worker threads running jobs from a few lanes. Each lane runs its
//jobs in the order they were pushed, and workers always take from the lowest
//numbered lane that has work, so urgent work never waits behind long jobs.
//Idle workers sleep until there is work, or until the next timed job is due.
class WorkPool {
public:
	typedef std::function<void()> Job;
	static const unsigned MaxLanes = 4;

	//threadStart and threadEnd run on each worker before its first and after its last job
	void start(unsigned workers, const char* name,
		std::function<void()> threadStart = nullptr, std::function<void()> threadEnd = nullptr);
	//Waits for running jobs to finish and the workers to exit, discarding queued jobs
	void stop();
	bool isRunning();
	unsigned workerCount();

	void push(unsigned lane, Job job);
	//Queues a job on the lane once the delay has passed
	void pushAfter(unsigned lane, double seconds, Job job);

	//Workers stop taking jobs from a held lane until it is released as many times
	void hold(unsigned lane);
	void release(unsigned lane);
	//Waits until no thread other than the caller is running a job from the lane
	void waitIdle(unsigned lane);

	//Runs one queued job from the lanes in laneMask on the calling thread, if there is one
	bool runOne(unsigned laneMask = ~0u);
	//Runs jobs from the lanes in laneMask on the calling thread until done() returns true,
	//sleeping while there are none. done() is checked with the pool locked, again each
	//time a job finishes on any thread, and every recheckSeconds when that is not 0,
	//for conditions that change outside of jobs.
	void helpUntil(const std::function<bool()>& done, unsigned laneMask = ~0u, double recheckSeconds = 0);
	//Rechecks every helpUntil condition
	void notify();

	WorkPool();
	~WorkPool();
	WorkPool(const WorkPool&) = delete;
	WorkPool& operator=(const WorkPool&) = delete;

private:
	typedef std::chrono::steady_clock Clock;
	struct TimedJob {
		Clock::time_point due;
		unsigned lane;
		Job job;
	};

	std::mutex mutex;
	std::condition_variable workCv, helperCv, idleCv;
	std::deque<Job> lanes[MaxLanes];
	unsigned held[MaxLanes];
	unsigned running[MaxLanes];
	//Timed jobs as a min-heap by due time
	std::vector<TimedJob> timed;
	unsigned workers, activeWorkers, helpersWaiting;
	bool stopping;
	std::function<void()> threadStart, threadEnd;

	static threadreturn threadcall workerThread(void* arg);
	void workerLoop();
	bool take(unsigned laneMask, Job& job, unsigned& lane);
	void runTaken(std::unique_lock<std::mutex>& lock, Job& job, unsigned lane);
};

};
