/******************************************************************************
    Copyright © 2012-2015 Martin Karsten

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/
#include "runtime/RuntimeImpl.h"
#include "runtime/Scheduler.h"
#include "runtime/Stack.h"
#include "runtime/Thread.h"
#include "kernel/Output.h"
#include "world/Access.h"
#include "machine/Machine.h"
#include "devices/Keyboard.h"

// Initialize minimum run-time to maximum long integer value
// as a constant. It will be updated to the current virtual
// run-time as we check the thread test-cases in switchTest().
mword MIN_RUNTIME = 0xFFFFFFFF;
// Needed for recalibrating scheduler parameters
// in terms of ticks per second.
mword totalPrio = 0;
mword taskTimeslice = 1;
// Task running times
mword timeStart;
mword timeWait;

// Since we could not parse the 
// schedparams, the default values
// for granularity and epoch length
// are defined here.
int schedMinGranularity = 20;
int defaultEpochLength = 4;

// We do the calculations here as well, although
// in terms of design we should have done this 
// in Kernel.cc ("at the kernel level").
mword Scheduler::schedMinGranularityTicks = 4000000;
mword Scheduler::defaultEpochLengthTicks = 20000000;
mword Scheduler::currentEpochLengthTicks = 20000000;
	   
/***********************************
    Used as a node in the tree to 
	reference the thread instance
	Created by: Adam Fazekas (Fall 2015)
***********************************/
class ThreadNode{
	friend class Scheduler;
	Thread *th;
	
	public:
		bool operator < (ThreadNode other) const {
			return th->priority < other.th->priority;
		}
		bool operator == (ThreadNode other) const {
			return th->priority == other.th->priority;
		}
		bool operator > (ThreadNode other) const {
			return th->priority > other.th->priority;
		}
    
	//this is how we want to do it
	ThreadNode(Thread *t){
		th = t;
	}
};	   
	   
/***********************************
			Constructor
***********************************/	   
Scheduler::Scheduler() : readyCount(0), preemption(0), resumption(0), partner(this) {
	//Initialize the idle thread
	//(It keeps the CPU awake when there are no other threads currently running)
	Thread* idleThread = Thread::create((vaddr)idleStack, minimumStack);
	idleThread->setAffinity(this)->setPriority(idlePriority);
	// use low-level routines, since runtime context might not exist
	idleThread->stackPointer = stackInit(idleThread->stackPointer, &Runtime::getDefaultMemoryContext(), (ptr_t)Runtime::idleLoop, this, nullptr, nullptr);
	
	//Initialize the tree that contains the threads waiting to be served
	readyTree = new Tree<ThreadNode>();
	
	//Add the idle thread to the tree
	readyTree->insert(*(new ThreadNode(idleThread)));
	readyCount += 1;

	// Updating the value of the current time slice.
	// The denominator (the readyCount) encapsulates
	// the total priority values of all the tasks
	// currently in the readyTree.
	taskTimeslice = currentEpochLengthTicks/readyCount;
}

/***********************************
		Static functions
***********************************/      
static inline void unlock() {}

template<typename... Args>
static inline void unlock(BasicLock &l, Args&... a) {
  l.release();
  unlock(a...);
}	   

/***********************************
    Gets called whenever a thread 
	should be added to the tree
***********************************/
void Scheduler::enqueue(Thread& t) {
  // Before adding a thread to the tree,
  // the total running priority should be
  // updated in anticipation of a tick
  // recalculation.
  GENASSERT1(t.priority < maxPriority, t.priority);
  readyLock.acquire();
  readyTree->insert(*(new ThreadNode(&t)));	
  bool wake = (readyCount == 0);
  readyCount += 1;	 

  // Adjusting epochLengthTicks according to the provisions
  // in the assignment guidelines. 
  if (defaultEpochLengthTicks >= (readyCount * schedMinGranularityTicks)) {

  } else {
		currentEpochLengthTicks = (readyCount + 1)*schedMinGranularityTicks;
  }

  if (!t.threadAwake) {
	t.vRuntime += MIN_RUNTIME;
    t.threadAwake = true;
  }
			
  readyLock.release();
  Runtime::debugS("Thread ", FmtHex(&t), " queued on ", FmtHex(this));
  if (wake) Runtime::wakeUp(this);
}

/***********************************
    Gets triggered at every RTC
	interrupt (per Scheduler)
***********************************/
void Scheduler::preempt(){		// IRQs disabled, lock count inflated
	//Get current running thread
	Thread* currentThread = Runtime::getCurrThread();

	//Get its target scheduler
	Scheduler* target = currentThread->getAffinity();						
	
	//Check if the thread should move to a new scheduler
	//(based on the affinity)
	if(target != this && target){						
		//Switch the served thread on the target scheduler
		switchThread(target);				
	}
	
	//Check if it is time to switch the thread on the current scheduler
	if(switchTest(currentThread)){
		//Switch the served thread on the current scheduler
		switchThread(this);	
	}
}

/***********************************
    Checks if it is time to stop
	serving the current thread
	and start serving the next
	one
***********************************/
bool Scheduler::switchTest(Thread* t){
	mword totalRun = (t->endThread) - (t->startThread);

	// After performing the provided calculation
	// for updating the virtual run-time to the current
	// time point, we must check the conditions for 
	// whether the thread should be switched or not.
	// This will be contingent on the boolean values
	// of the total run-time against the min. granularity
	// ticks of the scheduler and the time slice of the process
	// that is running.

    t->vRuntime+=totalRun/(( (t->priority)%1) + 1);
	if (t->vRuntime < MIN_RUNTIME) {
		MIN_RUNTIME = t->vRuntime;
		return false;
	}

	if (totalRun >= taskTimeslice) {
		return true;
	}

	if (totalRun >= Scheduler::schedMinGranularityTicks) {
		return true;
	} 

	return false;
	
	t->vRuntime++;
	if (t->vRuntime % 10 == 0)
		return true;
	return false; 
//Otherwise return that the thread should not be switched
}

/***********************************
    Switches the current running
	thread with the next thread
	waiting in the tree
***********************************/
template<typename... Args>
inline void Scheduler::switchThread(Scheduler* target, Args&... a) {
  preemption += 1;
  CHECK_LOCK_MIN(sizeof...(Args));
  Thread* nextThread;
  readyLock.acquire();
	
  if(!readyTree->empty()){
	  nextThread = readyTree->popMinNode()->th;
	  nextThread->endThread = CPU::readTSC();
	  nextThread->startThread = nextThread->endThread;

	  taskTimeslice = (currentEpochLengthTicks*nextThread->priority+1)/(totalPrio+1);
      
	  nextThread->timeWait = nextThread->timeWait + CPU::readTSC() - nextThread->timeStart;

	MIN_RUNTIME = nextThread->vRuntime;
   	  readyCount -= 1;
 	  goto threadFound;
	}

  readyLock.release();
  GENASSERT0(target);
  GENASSERT0(!sizeof...(Args));
  return;                                         // return to current thread

threadFound:
  readyLock.release();
  resumption += 1;
  Thread* currThread = Runtime::getCurrThread();
  GENASSERTN(currThread && nextThread && nextThread != currThread, currThread, ' ', nextThread);

  if (target) currThread->nextScheduler = target; // yield/preempt to given processor
  else currThread->nextScheduler = this;          // suspend/resume to same processor
  unlock(a...);                                   // ...thus can unlock now
  CHECK_LOCK_COUNT(1);
  Runtime::debugS("Thread switch <", (target ? 'Y' : 'S'), ">: ", FmtHex(currThread), '(', FmtHex(currThread->stackPointer), ") to ", FmtHex(nextThread), '(', FmtHex(nextThread->stackPointer), ')');

  Runtime::MemoryContext& ctx = Runtime::getMemoryContext();
  Runtime::setCurrThread(nextThread);
  Thread* prevThread = stackSwitch(currThread, target, &currThread->stackPointer, nextThread->stackPointer);
  // REMEMBER: Thread might have migrated from other processor, so 'this'
  //           might not be currThread's Scheduler object anymore.
  //           However, 'this' points to prevThread's Scheduler object.
  Runtime::postResume(false, *prevThread, ctx);
  if (currThread->state == Thread::Cancelled) {
    currThread->state = Thread::Finishing;
    switchThread(nullptr);
    unreachable();
  }
}

/***********************************
    Gets triggered when a thread is 
	suspended
***********************************/
void Scheduler::suspend(BasicLock& lk) {
  Runtime::FakeLock fl;
  switchThread(nullptr, lk);
}
void Scheduler::suspend(BasicLock& lk1, BasicLock& lk2) {
  Runtime::FakeLock fl;
  switchThread(nullptr, lk1, lk2);
}

/***********************************
    Gets triggered when a thread is 
	awake after suspension
***********************************/
void Scheduler::resume(Thread& t) {
  GENASSERT1(&t != Runtime::getCurrThread(), Runtime::getCurrThread());
  if (t.nextScheduler) t.nextScheduler->enqueue(t);
  else Runtime::getScheduler()->enqueue(t);
}

/***********************************
    Gets triggered when a thread is 
	done but not destroyed yet
***********************************/
void Scheduler::terminate() {
  Runtime::RealLock rl;
  Thread* thr = Runtime::getCurrThread();
  GENASSERT1(thr->state != Thread::Blocked, thr->state);
  thr->state = Thread::Finishing;
  switchThread(nullptr);
  unreachable();
}

/***********************************
		Other functions
***********************************/      
extern "C" Thread* postSwitch(Thread* prevThread, Scheduler* target) {
  CHECK_LOCK_COUNT(1);
  if fastpath(target) Scheduler::resume(*prevThread);
  return prevThread;
}

extern "C" void invokeThread(Thread* prevThread, Runtime::MemoryContext* ctx, funcvoid3_t func, ptr_t arg1, ptr_t arg2, ptr_t arg3) {
  Runtime::postResume(true, *prevThread, *ctx);
  func(arg1, arg2, arg3);
  Runtime::getScheduler()->terminate();
}
