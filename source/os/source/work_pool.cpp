#include "work_pool.h"
#include <algorithm>

namespace threads {

//How many jobs from each lane the current thread is running, so waitIdle can
//leave out a job that is itself waiting
static thread_local unsigned runningHere[WorkPool::MaxLanes];

//Orders timed jobs into a min-heap by due time
static const auto dueLater = [](const auto& a, const auto& b) {
	return a.due > b.due;
};

WorkPool::WorkPool() : workers(0), activeWorkers(0), helpersWaiting(0), stopping(false) {
	for(unsigned i = 0; i < MaxLanes; ++i) {
		held[i] = 0;
		running[i] = 0;
	}
}

WorkPool::~WorkPool() {
	stop();
}

void WorkPool::start(unsigned count, const char* name, std::function<void()> onStart, std::function<void()> onEnd) {
	{
		std::lock_guard<std::mutex> lock(mutex);
		if(workers != 0)
			return;
		stopping = false;
		workers = count;
		activeWorkers = count;
		threadStart = std::move(onStart);
		threadEnd = std::move(onEnd);
	}
	for(unsigned i = 0; i < count; ++i)
		createThread(workerThread, this, name);
}

void WorkPool::stop() {
	std::unique_lock<std::mutex> lock(mutex);
	if(workers == 0)
		return;
	stopping = true;
	workCv.notify_all();
	idleCv.wait(lock, [this] { return activeWorkers == 0; });

	workers = 0;
	for(unsigned i = 0; i < MaxLanes; ++i) {
		lanes[i].clear();
		held[i] = 0;
	}
	timed.clear();
	helperCv.notify_all();
}

bool WorkPool::isRunning() {
	std::lock_guard<std::mutex> lock(mutex);
	return workers != 0 && !stopping;
}

unsigned WorkPool::workerCount() {
	std::lock_guard<std::mutex> lock(mutex);
	return workers;
}

void WorkPool::push(unsigned lane, Job job) {
	bool wakeHelpers;
	{
		std::lock_guard<std::mutex> lock(mutex);
		lanes[lane].push_back(std::move(job));
		wakeHelpers = helpersWaiting != 0;
	}
	workCv.notify_one();
	if(wakeHelpers)
		helperCv.notify_all();
}

void WorkPool::pushAfter(unsigned lane, double seconds, Job job) {
	auto due = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
	bool wakeHelpers;
	{
		std::lock_guard<std::mutex> lock(mutex);
		timed.push_back({ due, lane, std::move(job) });
		std::push_heap(timed.begin(), timed.end(), dueLater);
		wakeHelpers = helpersWaiting != 0;
	}
	//A sleeping worker may need to wake earlier than it planned to
	workCv.notify_one();
	if(wakeHelpers)
		helperCv.notify_all();
}

void WorkPool::hold(unsigned lane) {
	std::lock_guard<std::mutex> lock(mutex);
	++held[lane];
}

void WorkPool::release(unsigned lane) {
	{
		std::lock_guard<std::mutex> lock(mutex);
		if(held[lane] != 0)
			--held[lane];
	}
	workCv.notify_all();
	helperCv.notify_all();
}

void WorkPool::waitIdle(unsigned lane) {
	std::unique_lock<std::mutex> lock(mutex);
	idleCv.wait(lock, [this, lane] { return running[lane] == runningHere[lane]; });
}

//Moves due timed jobs onto their lanes, then takes the first job from the
//highest priority lane in the mask that is not held. Called with the mutex locked.
bool WorkPool::take(unsigned laneMask, Job& job, unsigned& lane) {
	if(!timed.empty()) {
		auto now = Clock::now();
		unsigned moved = 0;
		while(!timed.empty() && timed.front().due <= now) {
			std::pop_heap(timed.begin(), timed.end(), dueLater);
			lanes[timed.back().lane].push_back(std::move(timed.back().job));
			timed.pop_back();
			++moved;
		}
		if(moved > 1)
			workCv.notify_all();
	}

	for(unsigned i = 0; i < MaxLanes; ++i) {
		if((laneMask & (1u << i)) == 0 || held[i] != 0 || lanes[i].empty())
			continue;
		job = std::move(lanes[i].front());
		lanes[i].pop_front();
		++running[i];
		lane = i;
		return true;
	}
	return false;
}

//Runs a job from take() with the mutex unlocked
void WorkPool::runTaken(std::unique_lock<std::mutex>& lock, Job& job, unsigned lane) {
	struct Finish {
		WorkPool* pool;
		std::unique_lock<std::mutex>& lock;
		Job& job;
		unsigned lane;
		~Finish() {
			//Destroy what the job captured before relocking
			job = nullptr;
			lock.lock();
			--runningHere[lane];
			--pool->running[lane];
			pool->idleCv.notify_all();
			if(pool->helpersWaiting != 0)
				pool->helperCv.notify_all();
		}
	};

	++runningHere[lane];
	lock.unlock();
	Finish finish{ this, lock, job, lane };
	job();
}

bool WorkPool::runOne(unsigned laneMask) {
	std::unique_lock<std::mutex> lock(mutex);
	Job job;
	unsigned lane;
	if(!take(laneMask, job, lane))
		return false;
	runTaken(lock, job, lane);
	return true;
}

void WorkPool::helpUntil(const std::function<bool()>& done, unsigned laneMask, double recheckSeconds) {
	std::unique_lock<std::mutex> lock(mutex);
	while(!done()) {
		Job job;
		unsigned lane;
		if(take(laneMask, job, lane)) {
			runTaken(lock, job, lane);
			continue;
		}

		auto until = Clock::time_point::max();
		if(recheckSeconds > 0)
			until = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(recheckSeconds));
		if(!timed.empty() && (laneMask & (1u << timed.front().lane)) != 0)
			until = std::min(until, timed.front().due);

		++helpersWaiting;
		if(until == Clock::time_point::max())
			helperCv.wait(lock);
		else
			helperCv.wait_until(lock, until);
		--helpersWaiting;
	}
}

void WorkPool::notify() {
	std::lock_guard<std::mutex> lock(mutex);
	helperCv.notify_all();
}

threadreturn threadcall WorkPool::workerThread(void* arg) {
	((WorkPool*)arg)->workerLoop();
	return 0;
}

void WorkPool::workerLoop() {
	if(threadStart)
		threadStart();

	std::unique_lock<std::mutex> lock(mutex);
	while(!stopping) {
		Job job;
		unsigned lane;
		if(take(~0u, job, lane)) {
			runTaken(lock, job, lane);
			continue;
		}

		if(timed.empty())
			workCv.wait(lock);
		else
			workCv.wait_until(lock, timed.front().due);
	}

	lock.unlock();
	if(threadEnd)
		threadEnd();
	lock.lock();
	--activeWorkers;
	idleCv.notify_all();
}

};
