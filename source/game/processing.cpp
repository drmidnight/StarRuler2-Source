#include "processing.h"
#include "main/references.h"
#include "threads.h"
#include "work_pool.h"
#include "main/initialization.h"
#include "main/logging.h"
#include "scripts/context_cache.h"
#include "obj/lock.h"
#include "design/projectiles.h"
#include "empire.h"
#include <deque>
#include <atomic>

namespace processing {

threads::threadreturn threadcall empireMessageProcess(void* data);

//Runs lock group ticks, actions and frame jobs
threads::WorkPool pool;
//Runs messages to lock groups. Jobs on the main pool can wait for a message to be
//handled, so messages get their own threads, which a waiting job can never use up.
threads::WorkPool messagePool;
std::atomic<bool> stopProcessing = false, paused = false;

threads::Mutex actionLock;
//Actions waiting to run, each with a job queued on L_Action to run it
std::deque<Action*> actionQueue;
//Actions waiting for the next frame
std::vector<Action*> frameActions;
std::vector<Action*> isolation_queue;
//Queue size, to check for work without taking actionLock
std::atomic<size_t> isolationQueued;

//Empires with a message thread, and how many of those threads are running
threads::Mutex empireThreadLock;
std::vector<Empire*> empireThreads;
threads::Signal empireThreadsActive;

#ifdef PROFILE_PROCESSING
Threaded(ProcessingData*) threadProc;
#endif

void runNextAction();

void queueAction(Action* action) {
	{
		threads::Lock lock(actionLock);
		actionQueue.push_back(action);
	}
	pool.push(L_Action, runNextAction);
}

void queueIsolationAction(Action* action) {
	threads::Lock lock(actionLock);
	isolation_queue.push_back(action);
	isolationQueued.store(isolation_queue.size(), std::memory_order_relaxed);
}

void runNextAction() {
	Action* action = nullptr;
	{
		threads::Lock lock(actionLock);
		if(actionQueue.empty())
			return;
		action = actionQueue.front();
		actionQueue.pop_front();
	}

	switch(action->run()) {
		case AR_Done:
			delete action;
		break;
		case AR_Again:
			queueAction(action);
		break;
		case AR_NextFrame: {
			threads::Lock lock(actionLock);
			frameActions.push_back(action);
		} break;
	}
}

void runIsolation() {
	if(isolationQueued.load(std::memory_order_relaxed) == 0)
		return;
	processing::pause();
	actionLock.lock();
	foreach(it, isolation_queue) {
		(*it)->run();
		delete *it;
	}
	isolation_queue.clear();
	isolationQueued = 0;
	actionLock.release();
	processing::resume();
}

void queueJob(Lane lane, std::function<void()> job) {
	if(lane == L_Message)
		messagePool.push(0, std::move(job));
	else
		pool.push(lane, std::move(job));
}

void queueJobAfter(Lane lane, double seconds, std::function<void()> job) {
	if(lane == L_Message)
		messagePool.pushAfter(0, seconds, std::move(job));
	else
		pool.pushAfter(lane, seconds, std::move(job));
}

void helpUntil(const std::function<bool()>& done) {
	pool.helpUntil(done, (1u << L_Frame) | (1u << L_Action));
}

void frame() {
	{
		threads::Lock lock(actionLock);
		for(auto* action : frameActions) {
			actionQueue.push_back(action);
			pool.push(L_Action, runNextAction);
		}
		frameActions.clear();
	}

	if(!stopProcessing && pool.isRunning())
		requestLockCycle();
}

void start(unsigned workers, unsigned messageWorkers) {
	stopProcessing = false;
	paused = false;
	messagePool.start(messageWorkers, "sr2-messages",
		[] {
			enterSection(NS_MessageThread);
			threads::setThreadPriority(threads::TP_High);
			initNewThread();
		},
		[] {
			cleanupThread();
		});
	pool.start(workers, "sr2-worker",
		[] {
			enterSection(NS_Processing);
			threads::setThreadPriority(threads::TP_High);
#ifdef PROFILE_PROCESSING
			threadProc = new ProcessingData();
#endif
			initNewThread();
		},
		[] {
			cleanupThread();
		});
	resetLockScheduling();
}

void end() {
	stopProcessing = true;
	{
		threads::Lock lock(empireThreadLock);
		for(auto* emp : empireThreads)
			emp->wake();
		empireThreads.clear();
	}
	empireThreadsActive.wait(0);
	pool.stop();
	messagePool.stop();
	paused = false;
}

bool isRunning() {
	return pool.isRunning() && !paused;
}

void clear() {
	if(devices.scripts.cache_server)
		devices.scripts.cache_server->clearScriptThreads();
	if(devices.scripts.cache_shadow)
		devices.scripts.cache_shadow->clearScriptThreads();
	if(devices.scripts.client)
		devices.scripts.client->clearScriptThreads();

	threads::Lock lock(actionLock);
	foreach(it, actionQueue)
		delete *it;
	actionQueue.clear();
	foreach(it, frameActions)
		delete *it;
	frameActions.clear();
}

void pause() {
	pauseProjectiles();
	if(devices.scripts.cache_server)
		devices.scripts.cache_server->pauseScriptThreads();
	if(devices.scripts.cache_shadow)
		devices.scripts.cache_shadow->pauseScriptThreads();
	if(devices.scripts.client)
		devices.scripts.client->pauseScriptThreads();

	if(!paused.exchange(true)) {
		pool.hold(L_Action);
		pool.hold(L_Tick);
	}
	pool.waitIdle(L_Action);
	pool.waitIdle(L_Tick);

	//Objects waiting to join a lock group, and all messages, are handled before pausing
	drainLockQueues();
}

void resume() {
	if(paused.exchange(false)) {
		pool.release(L_Tick);
		pool.release(L_Action);
	}
	if(stopProcessing)
		return;
	if(devices.scripts.cache_server)
		devices.scripts.cache_server->resumeScriptThreads();
	if(devices.scripts.cache_shadow)
		devices.scripts.cache_shadow->resumeScriptThreads();
	if(devices.scripts.client)
		devices.scripts.client->resumeScriptThreads();
	resumeProjectiles();
}

void pauseMessageHandling() {
	//Wait for a moment with no messages left. Empire messages are handled on their
	//own threads, so the condition is also rechecked on a timer.
	messagePool.helpUntil([] { return !hasRemainingMessages(); }, ~0u, 0.0005);
}

void resumeMessageHandling() {
}

#ifdef PROFILE_PROCESSING
threads::atomic_int printProcProfile;

void printProcessingProfile() {
	printProcProfile = pool.workerCount() + 1;
}

ProcessingData::ProcessingData() {
	syncTimer = 0;
	clear();
}

void ProcessingData::clear() {
	for(unsigned i = 0, cnt = objTime.size(); i < cnt; ++i) {
		objTime[i] = 0;
		objCount[i] = 0;
	}

	globalCount = 0;
	switchedTime = 0;
	targUpdateTime = 0;
	switchedCount = 0;
	targetUpdates = 0;
}

void ProcessingData::measureType(int type, double time, int amount) {
	if(objTime.size() <= type) {
		objTime.resize(type+1, 0);
		objCount.resize(type+1, 0);
	}

	objTime[type] += time;
	objCount[type] += amount;
}

void profileProcessing() {
	if(!threadProc)
		return;
	double curTime = devices.driver->getAccurateTime();
	if(curTime >= threadProc->syncTimer) {
		if(printProcProfile > 0) {

			print("Thread %d Processing Profile:", threads::getThreadID());
			for(unsigned i = 0, cnt = threadProc->objTime.size(); i < cnt; ++i) {
				if(threadProc->objCount[i] == 0)
					continue;
				print("    %s: %d objects in %.4gms (~%.3gms per)",
					getScriptObjectType(i)->name.c_str(),
					threadProc->objCount[i],
					threadProc->objTime[i] * 1000.0,
					(threadProc->objTime[i] / threadProc->objCount[i]) * 1000.0);
			}
			if(threadProc->globalCount != 0) {
				print("    Global processing: %d ticks", threadProc->globalCount);
				if(threadProc->switchedCount != 0)
					print("        Switched objects: %d objects in %.4gms (~%.3gms per)",
						threadProc->switchedCount, threadProc->switchedTime * 1000.0,
						(threadProc->switchedTime / threadProc->switchedCount) * 1000.0);
				if(threadProc->targetUpdates != 0)
					print("        Target Update: %d updates in %.4gms (~%.3gms per)",
						threadProc->targetUpdates, threadProc->targUpdateTime * 1000.0,
						threadProc->targUpdateTime * 1000.0 / threadProc->targetUpdates);
			}

			--printProcProfile;
		}

		threadProc->clear();
		threadProc->syncTimer = curTime + 1.0;
	}
}
#endif

void startEmpireThread(Empire* emp) {
	{
		threads::Lock lock(empireThreadLock);
		empireThreads.push_back(emp);
	}
	empireThreadsActive.signalUp();
	threads::createThread(empireMessageProcess, emp, "sr2-empire-messages");
}

//Each empire handles its messages on its own thread, which sleeps until one arrives
threads::threadreturn threadcall empireMessageProcess(void* data) {
	enterSection(NS_MessageThread);
	threads::setThreadPriority(threads::TP_High);
	Empire* emp = (Empire*)data;
	initNewThread();

	while(!stopProcessing) {
		unsigned seen = emp->wakeCount.load();
		if(emp->hasMessages()) {
			emp->processMessages();
			continue;
		}
		if(stopProcessing)
			break;
		emp->wakeCount.wait(seen);
	}

	cleanupThread();
	empireThreadsActive.signalDown();
	return 0;
}

};
