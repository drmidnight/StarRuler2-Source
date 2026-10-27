#pragma once
#include <vector>
#include <functional>

class Empire;

namespace processing {

//#define PROFILE_PROCESSING
#ifdef PROFILE_PROCESSING
struct ProcessingData {
	std::vector<double> objTime;
	std::vector<int> objCount;
	double syncTimer;

	int globalCount;
	double switchedTime;
	int switchedCount;
	double targUpdateTime;
	int targetUpdates;

	void measureType(int type, double time, int amount);

	ProcessingData();
	void clear();
};

void printProcessingProfile();
//Prints the calling worker's profile when one was asked for
void profileProcessing();
#endif

//Lanes of the worker pool. Workers take from the most urgent lane that has work.
enum Lane {
	//Work the current frame is waiting for
	L_Frame,
	//Messages to lock groups, which run on threads of their own
	L_Message,
	//Queued actions, like script ticks
	L_Action,
	//Lock group ticks and the global step after them
	L_Tick,
};

enum ActionResult {
	//The action is finished and gets deleted
	AR_Done,
	//Run the action again once other queued actions have had a turn
	AR_Again,
	//Run the action again on the next frame
	AR_NextFrame,
};

class Action {
public:
	virtual ActionResult run() = 0;
	virtual ~Action() {}
};

void queueAction(Action* action);
void queueIsolationAction(Action* action);
void runIsolation();
void queueJob(Lane lane, std::function<void()> job);
void queueJobAfter(Lane lane, double seconds, std::function<void()> job);
//Runs frame jobs, messages and actions on the calling thread until done() returns
//true. done() is called with the pool locked, so it must only read state.
void helpUntil(const std::function<bool()>& done);
//Called at the start of every frame: requeues actions waiting for it and
//starts a simulation cycle
void frame();
void pauseMessageHandling();
void resumeMessageHandling();

void startEmpireThread(Empire* emp);
void start(unsigned workers, unsigned messageWorkers);
void end();
void clear();
bool isRunning();

void pause();
void resume();

};
