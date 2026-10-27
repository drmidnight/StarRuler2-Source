//Tests for the threading primitives in threads.h
//  make -f source/linux/Makefile test_threads
//  make -f source/linux/Makefile test_threads SANITIZE=thread
#include "threads.h"
#include "work_pool.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { \
	if(!(cond)) { \
		printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		++failures; \
	} \
} while(0)

//Runs func on count threads made by threads::createThread, and waits for all of them
template<class F>
static void runThreads(int count, F func) {
	struct Run { F* func; int index; std::atomic<int>* done; };
	std::atomic<int> done(0);
	std::vector<Run> runs;
	for(int i = 0; i < count; ++i)
		runs.push_back({ &func, i, &done });
	for(int i = 0; i < count; ++i) {
		threads::createThread([](void* arg) -> threads::threadreturn {
			Run* run = (Run*)arg;
			(*run->func)(run->index);
			run->done->fetch_add(1);
			run->done->notify_all();
			return 0;
		}, &runs[i], "test");
	}
	for(int finished = done.load(); finished != count; finished = done.load())
		done.wait(finished);
}

static void sleepMs(int ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void testAtomicInt() {
	threads::atomic_int a(5);
	CHECK(++a == 6);
	CHECK(a++ == 6);
	CHECK(a.get() == 7);
	CHECK(--a == 6);
	CHECK(a-- == 6);
	CHECK((a += 10) == 5);
	CHECK((a -= 3) == 15);
	CHECK(a == 12);
	CHECK((a |= 1) == 13);
	CHECK((a &= 4) == 4);
	CHECK(a.exchange(9) == 4);
	CHECK(a.compare_exchange_strong(20, 9) == 9);
	CHECK(a == 20);
	CHECK(a.compare_exchange_strong(30, 9) == 20);
	CHECK(a == 20);

	threads::atomic_int counter;
	runThreads(8, [&](int) {
		for(int i = 0; i < 100000; ++i)
			++counter;
	});
	CHECK(counter == 800000);
}

static void testMutex() {
	threads::Mutex mutex;
	CHECK(!mutex.hasLock());
	mutex.lock();
	mutex.lock();
	CHECK(mutex.hasLock());
	CHECK(mutex.try_lock());
	mutex.release();
	mutex.release();
	CHECK(mutex.hasLock());

	//Other threads cannot take it while it is held
	bool otherGot = true;
	runThreads(1, [&](int) {
		otherGot = mutex.try_lock();
		if(otherGot)
			mutex.release();
	});
	CHECK(!otherGot);
	mutex.release();
	CHECK(!mutex.hasLock());

	//Mutual exclusion under contention, with plain data in the critical section
	long long total = 0;
	int inside = 0;
	bool overlapped = false;
	runThreads(8, [&](int) {
		for(int i = 0; i < 50000; ++i) {
			threads::Lock lock(mutex);
			if(++inside != 1)
				overlapped = true;
			total += 1;
			--inside;
		}
	});
	CHECK(!overlapped);
	CHECK(total == 400000);
}

static void testReadWriteMutex() {
	threads::ReadWriteMutex rw;

	//Recursive reads, reads inside a write, recursive writes
	rw.readLock();
	rw.readLock();
	CHECK(rw.hasLock());
	CHECK(!rw.hasWriteLock());
	rw.release();
	rw.release();
	CHECK(!rw.hasLock());

	rw.writeLock();
	rw.writeLock();
	rw.readLock();
	CHECK(rw.hasWriteLock());
	rw.release();
	rw.release();
	CHECK(rw.hasWriteLock());
	rw.release();
	CHECK(!rw.hasLock());
	CHECK(!rw.hasWriteLock());

	//Upgrading a read lock throws, and leaves the read lock held
	rw.readLock();
	bool threw = false;
	try {
		rw.writeLock();
	}
	catch(const char*) {
		threw = true;
	}
	CHECK(threw);
	CHECK(rw.hasLock());
	rw.release();

	//Readers run together
	std::atomic<int> readers(0), maxReaders(0);
	runThreads(4, [&](int) {
		threads::ReadLock lock(rw);
		int now = ++readers;
		for(int prev = maxReaders.load(); prev < now && !maxReaders.compare_exchange_weak(prev, now);) {}
		sleepMs(50);
		--readers;
	});
	CHECK(maxReaders.load() > 1);

	//Writers exclude readers: a writer keeps a and b equal, readers check them
	long long a = 0, b = 0;
	bool torn = false;
	runThreads(8, [&](int index) {
		for(int i = 0; i < 20000; ++i) {
			if(index % 4 == 0) {
				threads::WriteLock lock(rw);
				++a;
				++b;
			}
			else {
				threads::ReadLock lock(rw);
				if(a != b)
					torn = true;
			}
		}
	});
	CHECK(!torn);
	CHECK(a == 40000 && b == 40000);
}

static void testSignal() {
	//wait() returns once another thread sets the value, and sees what it wrote before
	threads::Signal signal(0);
	int payload = 0;
	runThreads(2, [&](int index) {
		if(index == 0) {
			sleepMs(20);
			payload = 42;
			signal.signal(1);
		}
		else {
			signal.wait(1);
			CHECK(payload == 42);
		}
	});

	//waitNot
	threads::Signal busy(1);
	runThreads(2, [&](int index) {
		if(index == 0) {
			sleepMs(20);
			busy.signalDown();
		}
		else {
			busy.waitNot(1);
			CHECK(busy.check(0));
		}
	});

	//Counting up and down from many threads, then waiting for zero
	threads::Signal count(0);
	count.signalUp(16);
	runThreads(16, [&](int) {
		sleepMs(5);
		count.signalDown();
	});
	count.wait(0);
	CHECK(count.check(0));

	//waitAndSignal as a token passed between threads
	threads::Signal token(0);
	int turns = 0;
	runThreads(2, [&](int index) {
		for(int i = 0; i < 1000; ++i) {
			token.waitAndSignal(index, 2);
			++turns;
			token.signal(1 - index);
		}
	});
	CHECK(turns == 2000);

	//checkAndSignal
	threads::Signal flag(3);
	CHECK(!flag.checkAndSignal(2, 5));
	CHECK(flag.checkAndSignal(3, 5));
	CHECK(flag.check(5));
}

static void testThreadInfo() {
	CHECK(threads::getPerformanceCores() >= 1);
	CHECK(threads::getPerformanceCores() <= threads::getNumberOfProcessors());

	int mainID = threads::getThreadID();
	int otherID = mainID;
	runThreads(1, [&](int) {
		threads::setThreadPriority(threads::TP_High);
		otherID = threads::getThreadID();
	});
	CHECK(otherID != mainID);
}

static void testWorkPool() {
	//Jobs run on the calling thread when there are no workers
	{
		threads::WorkPool pool;
		std::vector<int> order;
		for(int i = 0; i < 5; ++i)
			pool.push(0, [&order, i] { order.push_back(i); });
		pool.helpUntil([&] { return order.size() == 5; });
		CHECK((order == std::vector<int>{ 0, 1, 2, 3, 4 }));
	}

	//Many jobs on many workers
	{
		threads::WorkPool pool;
		pool.start(4, "test");
		std::atomic<int> count(0);
		for(int i = 0; i < 10000; ++i)
			pool.push(i % 3, [&] { ++count; });
		pool.helpUntil([&] { return count.load() == 10000; }, 0);
		CHECK(count.load() == 10000);
		pool.stop();
	}

	//Lower lanes run first, and each lane is first in first out
	{
		threads::WorkPool pool;
		std::atomic<bool> go(false);
		std::atomic<int> ran(0);
		std::vector<int> order;
		pool.start(1, "test");
		pool.push(0, [&] { go.wait(false); });
		pool.push(1, [&] { order.push_back(10); ++ran; });
		pool.push(1, [&] { order.push_back(11); ++ran; });
		pool.push(0, [&] { order.push_back(0); ++ran; });
		go = true;
		go.notify_all();
		pool.helpUntil([&] { return ran.load() == 3; }, 0);
		CHECK((order == std::vector<int>{ 0, 10, 11 }));
		pool.stop();
	}

	//Held lanes don't run until released, and waitIdle waits for running jobs
	{
		threads::WorkPool pool;
		pool.start(2, "test");
		std::atomic<int> count(0);
		pool.hold(1);
		pool.push(1, [&] { ++count; });
		sleepMs(20);
		CHECK(count.load() == 0);
		pool.release(1);
		pool.helpUntil([&] { return count.load() == 1; }, 0);

		std::atomic<bool> finished(false);
		pool.push(2, [&] { sleepMs(30); finished = true; });
		sleepMs(5);
		pool.waitIdle(2);
		CHECK(finished.load());

		//A job waiting for its own lane doesn't wait for itself
		std::atomic<bool> selfWaited(false);
		pool.push(2, [&] { pool.waitIdle(2); selfWaited = true; });
		pool.helpUntil([&] { return selfWaited.load(); }, 0);
		pool.stop();
	}

	//Timed jobs wait for their delay, and wake sleeping workers
	{
		threads::WorkPool pool;
		pool.start(2, "test");
		std::atomic<bool> ran(false);
		auto start = std::chrono::steady_clock::now();
		pool.pushAfter(0, 0.05, [&] { ran = true; });
		pool.helpUntil([&] { return ran.load(); }, 0);
		CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(50));
		pool.stop();
	}

	//helpUntil rechecks conditions that change outside of the pool
	{
		threads::WorkPool pool;
		std::atomic<bool> flag(false);
		runThreads(2, [&](int index) {
			if(index == 0) {
				sleepMs(10);
				flag = true;
			}
			else {
				pool.helpUntil([&] { return flag.load(); }, 0, 0.001);
			}
		});
		CHECK(flag.load());
	}

	//Stopping discards queued jobs and can be restarted
	{
		threads::WorkPool pool;
		std::atomic<int> count(0);
		pool.start(1, "test");
		pool.hold(0);
		pool.push(0, [&] { ++count; });
		pool.stop();
		CHECK(count.load() == 0);
		pool.start(1, "test");
		pool.push(0, [&] { ++count; });
		pool.helpUntil([&] { return count.load() == 1; }, 0);
		pool.stop();
	}
}

int main() {
	struct { const char* name; void (*run)(); } tests[] = {
		{ "atomic_int", testAtomicInt },
		{ "Mutex", testMutex },
		{ "ReadWriteMutex", testReadWriteMutex },
		{ "Signal", testSignal },
		{ "thread info", testThreadInfo },
		{ "WorkPool", testWorkPool },
	};
	for(auto& test : tests) {
		int before = failures;
		test.run();
		printf("%s %s\n", failures == before ? "ok  " : "FAIL", test.name);
	}
	if(failures != 0) {
		printf("%d checks failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
