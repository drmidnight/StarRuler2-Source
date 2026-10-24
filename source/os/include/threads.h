#pragma once

#ifdef _MSC_VER
    #define Threaded(type) __declspec(thread) type

	#define threadcall __stdcall

#else
	#include <atomic>
	#include <pthread.h>

	#define Threaded(type) __thread type

	#define threadcall
#endif

//#define PROFILE_LOCKS
#ifdef PROFILE_LOCKS
#include <string>
#include <set>
#endif
#include <functional>
#include <atomic>
#include <mutex>
#ifdef __APPLE__
#include <os/lock.h>
#endif
#ifdef _MSC_VER
#include <shared_mutex>
#endif

namespace threads {

unsigned getNumberOfProcessors();
//The number of cores that are not efficiency cores, or all of them where that is unknown
unsigned getPerformanceCores();
void async(std::function<int()> f);

void sleep(unsigned int milliseconds);
//Sleep intended for use as a wait in a non-critical busy loop
void idle();

#ifndef _MSC_VER
typedef unsigned int threadreturn;
typedef threadreturn (*threadfunc)(void*);
#else
typedef unsigned long threadreturn;
typedef threadreturn threadcall threadfunc(void*);
#endif

//An integer with sequentially consistent atomic operations. On arm64 these cost the
//same as acquire/release ones, and the engine's handshakes rely on the full ordering.
class atomic_int {
	std::atomic<int> value;
public:
	int get_basic() { return value.load(); }
	void set_basic(int val) { value.store(val); }

	//Return the new value
	int operator++() { return value.fetch_add(1) + 1; }
	int operator--() { return value.fetch_sub(1) - 1; }
	int operator|=(int v) { return value.fetch_or(v) | v; }
	int operator&=(int v) { return value.fetch_and(v) & v; }

	//Return the previous value
	int operator++(int) { return value.fetch_add(1); }
	int operator--(int) { return value.fetch_sub(1); }
	int operator+=(int v) { return value.fetch_add(v); }
	int operator-=(int v) { return value.fetch_sub(v); }
	int exchange(int v) { return value.exchange(v); }

	void operator=(int v) { value.store(v); }
	atomic_int& operator=(const atomic_int& other) { value.store(other.get()); return *this; }

	//Sets the value to xchg if it is compareTo, and returns the value it had
	int compare_exchange_strong(int xchg, int compareTo) {
		value.compare_exchange_strong(compareTo, xchg);
		return compareTo;
	}

	int get() const { return value.load(); }
	operator int() const { return value.load(); }

	atomic_int() : value(0) {}
	atomic_int(int v) : value(v) {}
	//Copies the other's value as it is now; the copy is not otherwise synchronized
	atomic_int(const atomic_int& other) : value(other.get()) {}
};

//Swap a value atomically
int swap(int* ptr, int newval);
int compare_and_swap(int* ptr, int oldval, int newval);
//long long swap(long long* ptr, long long newval);
long long compare_and_swap(long long* ptr, long long oldval, long long newval);
void* swap(void** ptr, void* newval);
void* compare_and_swap(void** ptr, void* oldval, void* newval);

class _threadlocalPointer {
#ifdef _MSC_VER
	long index;
#elif defined(__GNUC__)
	pthread_key_t key;
#endif
public:
	_threadlocalPointer();
	~_threadlocalPointer();

	void set(void* ptr);
	void* get();
};

template<class T>
class threadlocalPointer  : public _threadlocalPointer {
public:
	inline operator T*() {
		return (T*)get();
	}

	inline void operator=(T* ptr) {
		set((void*)ptr);
	}

	inline T* operator->() {
		return (T*)get();
	}
};

enum ThreadPriority {
	TP_High,
	TP_Normal,
	TP_Low
};

extern const int invalidThreadID;
//The name shows in debuggers, Instruments and crash logs
void createThread(threadfunc func, void* arg, const char* name = nullptr);
int getThreadID();
void setThreadPriority(ThreadPriority priority);

//A recursive mutex. Waiting threads spin briefly, then sleep in the kernel, which on
//macOS also raises the owner's priority while they wait.
struct Mutex {
private:
	atomic_int owningThread;
	unsigned lockCount = 0;
#ifdef __APPLE__
	os_unfair_lock impl = OS_UNFAIR_LOCK_INIT;
#else
	std::mutex impl;
#endif
	static const unsigned spinCount;
public:
#ifdef PROFILE_LOCKS
	atomic_int profileCount;
	std::string name;
	bool observed;
	Mutex();
	Mutex(const char* name);
	~Mutex();
#else
	Mutex() = default;
#endif
	Mutex(const Mutex&) = delete;
	Mutex& operator=(const Mutex&) = delete;

	void lock();
	bool try_lock();
	void release();

	bool hasLock();
};

struct Lock {
private:
	Mutex* mutex;
public:

	Lock(Mutex& mtx) : mutex(&mtx) { mtx.lock(); }
	~Lock() { mutex->release(); }
};

//A read-write mutex. Both read and write locks are recursive, and the writer may also
//take read locks, but a reader may not upgrade to a write lock. Each thread can hold
//up to 16 different read-write mutexes at once.
struct ReadWriteMutex {
private:
	atomic_int owningThread;
#ifdef _MSC_VER
	std::shared_mutex impl;
#else
	pthread_rwlock_t impl = PTHREAD_RWLOCK_INITIALIZER;
#endif
public:
#ifdef PROFILE_LOCKS
	atomic_int profileReadCount;
	atomic_int profileWriteCount;
	std::string name;
	bool observed;
	ReadWriteMutex();
	~ReadWriteMutex();
#else
	ReadWriteMutex() = default;
#endif
	ReadWriteMutex(const ReadWriteMutex&) = delete;
	ReadWriteMutex& operator=(const ReadWriteMutex&) = delete;

	void writeLock();
	void readLock();
	void release();

	bool hasLock();
	bool hasWriteLock();
};

struct ReadLock {
private:
	ReadWriteMutex* mutex;
public:
	ReadLock(ReadWriteMutex& mtx) : mutex(&mtx) { mtx.readLock(); }
	~ReadLock() { mutex->release(); }
};

struct WriteLock {
private:
	ReadWriteMutex* mutex;
public:
	WriteLock(ReadWriteMutex& mtx) : mutex(&mtx) { mtx.writeLock(); }
	~WriteLock() { mutex->release(); }
};

//An integer other threads can wait on. Waiting threads spin briefly, then sleep until
//the value changes.
struct Signal {
private:
	std::atomic<int> flag;
	static const unsigned spinCount;
public:
	Signal(int start = 0);

	//Sets the signal to the specified value
	void signal(int value);
	//Reduces the signal's value by one
	void signalDown();
	//Increases the signal's value by one
	void signalUp();
	//Reduces the signal's value by a value
	void signalDown(int value);
	//Increases the signal's value by a value
	void signalUp(int value);

	//Returns true if the flag is at the value
	bool check(int checkFor) const;
	//Checks if the flag is at value, and sets it if it is
	bool checkAndSignal(int checkFor, int newSignal);

	//Waits until the value is the specified value
	void wait(int waitFor) const;
	//Waits until the value is not the specified value
	void waitNot(int waitForNot) const;
	//Waits until the value is the specified value, then sets it to a new value
	void waitAndSignal(int waitFor, int newSignal);
};

template<class T>
struct SharedData {
	mutable atomic_int count;
	T data;

	SharedData(int startCount = 1) : count(startCount), data() {}

	void grab() const {
		++count;
	}

	void drop() const {
		if(--count == 0)
			delete this;
	}

	T& operator*() {
		return data;
	}

	const T& operator*() const {
		return data;
	}

	T* operator->() {
		return &data;
	}

	const T* operator->() const {
		return &data;
	}
};

#ifdef PROFILE_LOCKS
void profileMutexCycle(std::function<void(Mutex*)> cb);
void profileReadWriteMutexCycle(std::function<void(ReadWriteMutex*)> cb);
#endif

};
