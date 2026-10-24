#include "threads.h"
#include <pthread.h>
#include <unistd.h>
#include <string>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <pthread/qos.h>
#endif

namespace threads {

atomic_int nextThreadID(2);
Threaded(int) threadID = 1;

const int invalidThreadID = 0;

int getThreadID() {
	return threadID;
}

struct threadInfo {
	threadfunc func;
	void* arg;
	std::string name;
};

void* startThread(void* arg) {
	threadInfo* info = (threadInfo*)arg;
	threadID = nextThreadID++;
	if(!info->name.empty()) {
#ifdef __APPLE__
		pthread_setname_np(info->name.c_str());
#else
		pthread_setname_np(pthread_self(), info->name.substr(0, 15).c_str());
#endif
	}

	info->func(info->arg);

	delete info;
	return 0;
}

void setThreadPriority(ThreadPriority priority) {
#ifdef __APPLE__
	//Quality of service decides both scheduling priority and whether a thread
	//may run on the performance cores
	qos_class_t qos;
	switch(priority) {
	case TP_High:
		qos = QOS_CLASS_USER_INTERACTIVE; break;
	case TP_Normal: default:
		qos = QOS_CLASS_USER_INITIATED; break;
	case TP_Low:
		qos = QOS_CLASS_UTILITY; break;
	}
	pthread_set_qos_class_self_np(qos, 0);
#endif
}

void createThread(threadfunc func, void* arg, const char* name) {
	threadInfo* info = new threadInfo();
	info->func = func;
	info->arg = arg;
	if(name)
		info->name = name;

	//Nothing joins the game's threads, so they release their resources on exit.
	//The handle goes to a local: the thread may already have freed info by the time
	//pthread_create returns.
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifdef __APPLE__
	//macOS gives secondary threads a 512KB stack by default, against 8MB on Linux.
	//The script threads nest script and native calls deeply, and on arm64 they run
	//interpreted rather than through the x86 JIT, so match the Linux default.
	pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
#endif
	pthread_create(&thread, &attr, &startThread, info);
	pthread_attr_destroy(&attr);
}

pthread_mutex_t sleepMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t sleepCond = PTHREAD_COND_INITIALIZER;

unsigned getNumberOfProcessors() {
	return sysconf(_SC_NPROCESSORS_ONLN);
}

unsigned getPerformanceCores() {
#ifdef __APPLE__
	//Performance level 0 is the fastest core type on Apple silicon
	int cores = 0;
	size_t size = sizeof(cores);
	if(sysctlbyname("hw.perflevel0.logicalcpu", &cores, &size, nullptr, 0) == 0 && cores > 0)
		return (unsigned)cores;
#endif
	return getNumberOfProcessors();
}

void sleep(unsigned int milliseconds) {
	struct timespec wait;
	wait.tv_sec = (milliseconds / 1000);
	wait.tv_nsec = (milliseconds % 1000) * 1000 * 1000;

	nanosleep(&wait, 0);
}

void idle() {
#ifdef __APPLE__
	sleep(1);
#else
	sleep(0);
#endif
}

_threadlocalPointer::_threadlocalPointer() {
	pthread_key_create(&key, 0);
}

void _threadlocalPointer::set(void* ptr) {
	pthread_setspecific(key, ptr);
}

void* _threadlocalPointer::get() {
	return pthread_getspecific(key);
}

_threadlocalPointer::~_threadlocalPointer() {
	pthread_key_delete(key);
}

int swap(int* ptr, int newval) {
	return __atomic_exchange_n(ptr, newval, __ATOMIC_SEQ_CST);
}

int compare_and_swap(int* ptr, int oldval, int newval) {
	return __sync_val_compare_and_swap(ptr, oldval, newval);
}

long long swap(long long* ptr, long long newval) {
	return __atomic_exchange_n(ptr, newval, __ATOMIC_SEQ_CST);
}

long long compare_and_swap(long long* ptr, long long oldval, long long newval) {
	return __sync_val_compare_and_swap(ptr, oldval, newval);
}

void* swap(void** ptr, void* newval) {
	return __atomic_exchange_n(ptr, newval, __ATOMIC_SEQ_CST);
}

void* compare_and_swap(void** ptr, void* oldval, void* newval) {
	return __sync_val_compare_and_swap(ptr, oldval, newval);
}

};
