#include "threads.h"
#include <assert.h>

namespace threads {

threadreturn threadcall asyncWrapper(void* pData) {
	std::function<int()>* pFunc = (std::function<int()>*)pData;

	int r = (*pFunc)();

	delete pFunc;
	return r;
}

void async(std::function<int()> f) {
	createThread(asyncWrapper, new std::function<int()>(f));
}

const unsigned Mutex::spinCount = 64;
const unsigned Signal::spinCount = 64;

//Tells the core we are spinning, so a sibling or the lock holder can make progress
static inline void cpuRelax() {
#if defined(__aarch64__)
	__builtin_arm_yield();
#elif defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#endif
}

void Mutex::lock() {
	int id = getThreadID();
	if(owningThread.get() == id) {
		++lockCount;
	}
	else {
#ifdef __APPLE__
		bool locked = false;
		for(unsigned i = 0; i < spinCount && !locked; ++i) {
			locked = os_unfair_lock_trylock(&impl);
			if(!locked)
				cpuRelax();
		}
		if(!locked)
			os_unfair_lock_lock(&impl);
#else
		impl.lock();
#endif
		owningThread = id;
		lockCount = 1;
	}

#ifdef PROFILE_LOCKS
	++profileCount;
#endif
}

bool Mutex::try_lock() {
	int id = getThreadID();
	if(owningThread.get() != id) {
#ifdef __APPLE__
		if(!os_unfair_lock_trylock(&impl))
			return false;
#else
		if(!impl.try_lock())
			return false;
#endif
		owningThread = id;
		lockCount = 1;
	}
	else {
		++lockCount;
	}

#ifdef PROFILE_LOCKS
	++profileCount;
#endif
	return true;
}

void Mutex::release() {
	assert(lockCount > 0);
	if(--lockCount == 0) {
		owningThread = invalidThreadID;
#ifdef __APPLE__
		os_unfair_lock_unlock(&impl);
#else
		impl.unlock();
#endif
	}
}

bool Mutex::hasLock() {
	return owningThread == getThreadID();
}

//Each thread records which read-write mutexes it holds, how many times, and whether
//it holds the write lock, so recursive locks only take the underlying lock once
struct HeldRWLock {
	ReadWriteMutex* lock;
	unsigned count;
	bool write;
};
static const int maxHeldRWLocks = 16;
Threaded(HeldRWLock) heldRWLocks[maxHeldRWLocks];
Threaded(int) heldRWLockCount = 0;

static HeldRWLock* findHeldRWLock(ReadWriteMutex* lock) {
	for(int i = heldRWLockCount - 1; i >= 0; --i)
		if(heldRWLocks[i].lock == lock)
			return &heldRWLocks[i];
	return nullptr;
}

static void addHeldRWLock(ReadWriteMutex* lock, bool write) {
	assert(heldRWLockCount < maxHeldRWLocks);
	heldRWLocks[heldRWLockCount++] = { lock, 1, write };
}

static void removeHeldRWLock(HeldRWLock* held) {
	int index = (int)(held - heldRWLocks);
	for(int i = index; i < heldRWLockCount - 1; ++i)
		heldRWLocks[i] = heldRWLocks[i+1];
	--heldRWLockCount;
}

void ReadWriteMutex::writeLock() {
	HeldRWLock* held = findHeldRWLock(this);
	if(held) {
		//Cannot upgrade locks to write locks, this creates deadlocks
		if(!held->write)
			throw "Upgrading a read lock is invalid.";
		++held->count;
	}
	else {
#ifdef _MSC_VER
		impl.lock();
#else
		pthread_rwlock_wrlock(&impl);
#endif
		owningThread = getThreadID();
		addHeldRWLock(this, true);
	}

#ifdef PROFILE_LOCKS
	++profileWriteCount;
#endif
}

void ReadWriteMutex::readLock() {
	HeldRWLock* held = findHeldRWLock(this);
	if(held) {
		//Read locks nest inside read locks and the write lock alike
		++held->count;
	}
	else {
#ifdef _MSC_VER
		impl.lock_shared();
#else
		pthread_rwlock_rdlock(&impl);
#endif
		addHeldRWLock(this, false);
	}

#ifdef PROFILE_LOCKS
	++profileReadCount;
#endif
}

void ReadWriteMutex::release() {
	HeldRWLock* held = findHeldRWLock(this);
	assert(held);
	if(--held->count != 0)
		return;

	bool write = held->write;
	removeHeldRWLock(held);
	if(write)
		owningThread = invalidThreadID;
#ifdef _MSC_VER
	if(write)
		impl.unlock();
	else
		impl.unlock_shared();
#else
	pthread_rwlock_unlock(&impl);
#endif
}

bool ReadWriteMutex::hasLock() {
	return findHeldRWLock(this) != nullptr;
}

bool ReadWriteMutex::hasWriteLock() {
	return owningThread == getThreadID();
}

Signal::Signal(int start) : flag(start) {
}

void Signal::signal(int value) {
	flag.store(value);
	flag.notify_all();
}

void Signal::signalDown() {
	flag.fetch_sub(1);
	flag.notify_all();
}

void Signal::signalUp() {
	flag.fetch_add(1);
	flag.notify_all();
}

void Signal::signalDown(int value) {
	flag.fetch_sub(value);
	flag.notify_all();
}

void Signal::signalUp(int value) {
	flag.fetch_add(value);
	flag.notify_all();
}

bool Signal::check(int checkFor) const {
	return flag.load() == checkFor;
}

bool Signal::checkAndSignal(int checkFor, int newSignal) {
	if(!flag.compare_exchange_strong(checkFor, newSignal))
		return false;
	flag.notify_all();
	return true;
}

void Signal::wait(int waitFor) const {
	for(unsigned spins = 0; ; ++spins) {
		int current = flag.load();
		if(current == waitFor)
			return;
		if(spins < spinCount)
			cpuRelax();
		else
			flag.wait(current);
	}
}

void Signal::waitNot(int waitForNot) const {
	for(unsigned spins = 0; flag.load() == waitForNot; ++spins) {
		if(spins < spinCount)
			cpuRelax();
		else
			flag.wait(waitForNot);
	}
}

void Signal::waitAndSignal(int waitFor, int newSignal) {
	for(unsigned spins = 0; ; ++spins) {
		int current = waitFor;
		if(flag.compare_exchange_strong(current, newSignal)) {
			flag.notify_all();
			return;
		}
		if(spins < spinCount)
			cpuRelax();
		else
			flag.wait(current);
	}
}

#ifdef PROFILE_LOCKS
std::set<Mutex*>* mutexes = 0;
std::set<ReadWriteMutex*>* rwMutexes = 0;
Mutex listMutex;

Mutex::Mutex() : observed(false) {
	if(this != &listMutex) {
		threads::Lock lock(listMutex);
		if(!mutexes)
			mutexes = new std::set<Mutex*>();
		mutexes->insert(this);
	}

	char buff[256];
	sprintf(buff, "Mutex %p", this);
	name = buff;
}

Mutex::Mutex(const char* mtxName) : observed(false) {
	if(this != &listMutex) {
		threads::Lock lock(listMutex);
		if(!mutexes)
			mutexes = new std::set<Mutex*>();
		mutexes->insert(this);
	}

	name = mtxName;
}

Mutex::~Mutex() {
	if(this != &listMutex) {
		threads::Lock lock(listMutex);
		mutexes->erase(this);
	}
}

ReadWriteMutex::ReadWriteMutex() : observed(false) {
	{
		threads::Lock lock(listMutex);
		if(!rwMutexes)
			rwMutexes = new std::set<ReadWriteMutex*>();
		rwMutexes->insert(this);
	}

	char buff[256];
	sprintf(buff, "ReadWriteMutex %p", this);
	name = buff;
}

ReadWriteMutex::~ReadWriteMutex() {
	threads::Lock lock(listMutex);
	rwMutexes->erase(this);
}

void profileMutexCycle(std::function<void(Mutex*)> cb) {
	threads::Lock lock(listMutex);
	for(auto it = mutexes->begin(), end = mutexes->end(); it != end; ++it) {
		if(cb)
			cb(*it);
		(*it)->profileCount = 0;
	}
}

void profileReadWriteMutexCycle(std::function<void(ReadWriteMutex*)> cb) {
	threads::Lock lock(listMutex);
	for(auto it = rwMutexes->begin(), end = rwMutexes->end(); it != end; ++it) {
		if(cb)
			cb(*it);
		(*it)->profileReadCount = 0;
		(*it)->profileWriteCount = 0;
	}
}
#endif

};
