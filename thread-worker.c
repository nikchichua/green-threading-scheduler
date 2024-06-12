// File:	thread-worker.c

// List all group member's name: Nikoloz Chichua
// username of iLab: nc733
// iLab Server: iLab3.cs.rutgers.edu

#include "thread-worker.h"

long tot_cntx_switches = 0;
double avg_turn_time = 0;
double avg_resp_time = 0;

static void *schedule(), sched_psjf(), sched_mlfq();

LinkedList *readyQueue;
LinkedList *descheduledQueue;
LinkedList *blockedQueue;

LinkedList *mlfQueue[MLFQ_SIZE];

TCB *schedulerThread;
TCB *terminationThread;

TCB *mainThread;
TCB *runningThread;

sigset_t timerBlocker;
struct itimerval timer;

unsigned int threadCounter = 0;
int timeQuantumByLevel = QUANTUM;
int boostCounter = 0;

void cleanUp() {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    freeTCB(schedulerThread);
    freeTCB(terminationThread);
    freeLinkedList(readyQueue);
    freeLinkedList(descheduledQueue);
    freeLinkedList(blockedQueue);
    #ifdef MLFQ
        for(int i = 1; i < MLFQ_SIZE; i++) {
            freeLinkedList(mlfQueue[i]);
        }
    #endif
}

void timerSignalHandler() {
    #ifdef MLFQ
        boostCounter += timeQuantumByLevel;
        boostJobs();
    #endif
    swapcontext(&runningThread->context, &schedulerThread->context);
}

void *terminationHandler() {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    runningThread->completion = clock();
    runningThread->state = DESCHEDULED;
    sigprocmask(SIG_UNBLOCK, &timerBlocker, NULL);
    swapcontext(&runningThread->context, &schedulerThread->context);
}

int worker_create(worker_t *thread, pthread_attr_t *attr, void *(*function)(void*), void *arg) {
    if(threadCounter == 0) {
        atexit(cleanUp);
        readyQueue = newLinkedList();
        blockedQueue = newLinkedList();
        descheduledQueue = newLinkedList();
        #ifdef MLFQ
            mlfQueue[0] = readyQueue;
            for(int i = 1; i < MLFQ_SIZE; i++) {
                mlfQueue[i] = newLinkedList();
            }
        #endif
        schedulerThread = newTCB(-1, NULL, schedule, NULL);
        terminationThread = newTCB(-2, NULL, terminationHandler, NULL);
        mainThread = newTCB(threadCounter, &terminationThread->context, NULL, NULL);
        runningThread = mainThread;
        setTimerSignalHandler(SIGPROF, timerSignalHandler);
        setTimerSignalBlocker(SIGPROF);
        setTimer(ITIMER_PROF, QUANTUM);
    }
    threadCounter++;
    *thread = threadCounter;
    TCB *newThread = newTCB(threadCounter, &terminationThread->context, function, arg);
    newThread->arrival = clock();
    append(readyQueue, newThread);
    return 0;
}

int worker_yield() {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    runningThread->yielded = true;
    swapcontext(&runningThread->context, &schedulerThread->context);
    sigprocmask(SIG_UNBLOCK, &timerBlocker, NULL);
    return 0;
}

void worker_exit(void *value_ptr) {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    runningThread->return_value = value_ptr;
    setcontext(&terminationThread->context);
}

int worker_join(worker_t thread, void **value_ptr) {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    #ifdef MLFQ
        TCB *busyThread = findThreadInMLFQ(thread);
    #else
        TCB *busyThread = findThread(readyQueue, thread);
    #endif
    if(busyThread != NULL) {
        runningThread->state = BLOCKED;
        busyThread->blocked_thread = runningThread;
    }
    sigprocmask(SIG_UNBLOCK, &timerBlocker, NULL);
    worker_yield();
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    busyThread = findThread(descheduledQueue, thread);
    if(busyThread == NULL) {
        printf("Error: worker_join() -> In descheduledQueue: busyThread ==  %d\n", thread);
        return -1;
    } else {
        if(value_ptr != NULL) {
            *value_ptr = busyThread->return_value;
        }
    }
    sigprocmask(SIG_UNBLOCK, &timerBlocker, NULL);
    return 0;
}

int worker_mutex_init(worker_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    mutex->initialized = true;
    mutex->locked = false;
    mutex->owner = -1;
    sigprocmask(SIG_UNBLOCK, &timerBlocker, NULL);
    return 0;
}

int worker_mutex_lock(worker_mutex_t *mutex) {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    bool *locked = &mutex->locked;
    bool test = __sync_lock_test_and_set(locked, true);
    if(*locked != test) {
        mutex->owner = runningThread->ID;
    } else {
        runningThread->state = BLOCKED;
        runningThread->blocking_thread_ID = mutex->owner;
        worker_yield();
    }
    return 0;
}

int worker_mutex_unlock(worker_mutex_t *mutex) {
    if(mutex->owner == runningThread->ID) {
        bool *locked = &mutex->locked;
        bool test = __sync_lock_test_and_set(locked, false);
        if(test != *locked) {
            TCB *each = blockedQueue->head;
            while(each != NULL) {
                TCB *next = each->next;
                if(each->blocking_thread_ID == runningThread->ID) {
                    TCB *unblocked = removeTCB(blockedQueue, each->ID);
                    unblocked->state = READY;
                    #ifdef MLFQ
                        append(mlfQueue[unblocked->priority], unblocked);
                    #else
                        append(readyQueue, unblocked);
                    #endif
                }
                each = next;
            }
            mutex->owner = -1;
        }
    }
    sigprocmask(SIG_UNBLOCK, &timerBlocker, NULL);
    return 0;
}

int worker_mutex_destroy(worker_mutex_t *mutex) {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    int returnResult;
    if(mutex->initialized && !mutex->locked) {
        mutex->initialized = false;
        mutex->owner = -1;
        returnResult = 0;
    } else {
        returnResult = -1;
    }
    sigprocmask(SIG_UNBLOCK, &timerBlocker, NULL);
    return returnResult;
}

void descheduleThread() {
    if(runningThread->blocked_thread != NULL) {
        TCB *unblockedThread = removeTCB(blockedQueue, runningThread->blocked_thread->ID);
        unblockedThread->state = READY;
    #ifdef MLFQ
        prepend(mlfQueue[unblockedThread->priority], unblockedThread);
    #else
        prepend(readyQueue, unblockedThread);
    #endif
    }
    append(descheduledQueue, runningThread);
}

static void *schedule() {
    #ifdef MLFQ
        sched_mlfq();
    #else
        sched_psjf();
    #endif
}

static void sched_psjf() {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    runningThread->quantum_ticks++;
    if(runningThread->state == DESCHEDULED) {
        descheduleThread();
    } else if (runningThread->state == BLOCKED) {
        append(blockedQueue, runningThread);
    } else {
        append(readyQueue, runningThread);
    }
    runningThread = shortestThread(readyQueue);
    if(runningThread == NULL) {
        if(blockedQueue->size > 0) {
            //DEADLOCKED!
            while(1);
        }
        return;
    }
    if(runningThread->quantum_ticks == 0) {
        runningThread->first_run = clock();
    }
    tot_cntx_switches++;
    setcontext(&runningThread->context);
}

void relegateThread() {
    if(runningThread->priority + 1 < MLFQ_SIZE) {
        runningThread->priority++;
    }
    runningThread->quantum_ticks = 0;
}

void boostJobs() {
    if(boostCounter >= BOOST_TIME) {
        boostCounter = 0;
        for (int i = 1; i < MLFQ_SIZE; i++) {
            while(mlfQueue[i]->size > 0) {
                TCB *each = popFront(mlfQueue[i]);
                each->priority = 0;
                append(readyQueue, each);
            }
        }
        runningThread->priority = 0;
    }
}

static void sched_mlfq() {
    sigprocmask(SIG_BLOCK, &timerBlocker, NULL);
    if(runningThread->state == DESCHEDULED) {
        descheduleThread();
    }
    if(runningThread->yielded) {
        runningThread->quantum_ticks++;
        runningThread->yielded = false;
        if(runningThread->quantum_ticks >= YIELD_LIMIT) {
            relegateThread();
        }
    } else {
        relegateThread();
    }
    if(runningThread->state == BLOCKED) {
        append(blockedQueue, runningThread);
    }
    if(runningThread->state == READY) {
        append(mlfQueue[runningThread->priority], runningThread);
    }
    for(int i = 0; i < MLFQ_SIZE; i++) {
        runningThread = popFront(mlfQueue[i]);
        if(runningThread != NULL) break;
    }
    if(runningThread == NULL) {
        if(blockedQueue->size > 0) {
            //DEADLOCKED!
            while(1);
        }
        return;
    }
    timeQuantumByLevel = calculateTimeQuantum(runningThread->priority);
    if(runningThread->first_run == -1) {
        runningThread->first_run = clock();
    }
    tot_cntx_switches++;
    setTimer(ITIMER_PROF, timeQuantumByLevel);
    setcontext(&runningThread->context);
}

void calculateStats() {
    mainThread->completion = clock();
    double totalTurnAroundTime = 0;
    double totalResponseTime = 0;
    TCB *each = descheduledQueue->head;
    while(each != NULL) {
        double turnAroundTime = (double) (each->completion - each->arrival) / CLOCKS_PER_SEC;
        double responseTime = (double) (each->first_run - each->arrival) / CLOCKS_PER_SEC;
        totalTurnAroundTime += turnAroundTime;
        totalResponseTime += responseTime;
        each = each->next;
    }
    avg_turn_time = totalTurnAroundTime / descheduledQueue->size;
    avg_resp_time = totalResponseTime / descheduledQueue->size;
}

void print_app_stats(void) {
    #ifdef USE_WORKERS
        calculateStats();
    #endif
    fprintf(stderr, "Total context switches %ld \n", tot_cntx_switches);
    fprintf(stderr, "Average turnaround time %lf \n", avg_turn_time);
    fprintf(stderr, "Average response time  %lf \n", avg_resp_time);
}

void *functionCaller(TCB *this, void *(*function)(void*), void *arg) {
    this->return_value = function(arg);
    return NULL;
}

TCB *newTCB(worker_t ID, ucontext_t *link, void *(*function)(void*), void *arg) {
    TCB *this = malloc(sizeof(TCB));
    this->ID = ID;
    this->state = READY;
    ucontext_t context;
    getcontext(&context);
    context.uc_link = link;
    if(function != NULL) {
        context.uc_stack.ss_size = STACK_SIZE;
        context.uc_stack.ss_sp = malloc(STACK_SIZE);
        context.uc_stack.ss_flags = 0;
        makecontext(&context, (void (*)(void)) functionCaller, 3, this, function, arg);
    }
    this->context = context;
    this->quantum_ticks = 0;
    this->priority = 0;
    this->blocked_thread = NULL;
    this->blocking_thread_ID = -1;
    this->first_run = -1;
    this->next = NULL;
    this->prev = NULL;
    return this;
}

void freeTCB(TCB *this) {
    this->ID = -1;
    this->state = -1;
    if(this->context.uc_stack.ss_size == SIGSTKSZ) {
        free(this->context.uc_stack.ss_sp);
    }
    this->return_value = NULL;
    this->quantum_ticks = 0;
    this->blocked_thread = NULL;
    this->next = NULL;
    this->prev = NULL;
    free(this);
    this = NULL;
}

LinkedList *newLinkedList() {
    LinkedList *linkedList = malloc(sizeof(LinkedList));
    linkedList->head = NULL;
    linkedList->tail = NULL;
    linkedList->size = 0;
    return linkedList;
}

void append(LinkedList *this, TCB *thread) {
    if(this->size == 0) {
        this->head = thread;
    } else {
        thread->prev = this->tail;
        this->tail->next = thread;
    }
    this->tail = thread;
    this->size++;
}

void prepend(LinkedList *this, TCB *thread) {
    if(this->size == 0) {
        this->tail = thread;
    } else {
        thread->next = this->head;
        this->head->prev = thread;
    }
    this->head = thread;
    this->size++;
}

TCB *findThread(LinkedList *this, worker_t ID) {
    TCB *each = this->head;
    while(each != NULL) {
        if(each->ID == ID) {
            return each;
        }
        each = each->next;
    }
    return NULL;
}

TCB *findThreadInMLFQ(worker_t ID) {
    for(int i = 0; i < MLFQ_SIZE; i++) {
        TCB *current = findThread(mlfQueue[i], ID);
        if(current != NULL) return current;
    }
    return NULL;
}

TCB *popBack(LinkedList *this) {
    if(this->size <= 0) {
        return NULL;
    }
    TCB *oldTail = this->tail;
    if(this->size <= 1) {
        this->head = NULL;
        this->tail = NULL;
    } else {
        this->tail = this->tail->prev;
        this->tail->next = NULL;
    }
    oldTail->next = NULL;
    oldTail->prev = NULL;
    this->size--;
    return oldTail;
}

TCB *popFront(LinkedList *this) {
    if(this->size <= 0) {
        return NULL;
    }
    TCB *oldHead = this->head;
    if(this->size <= 1) {
        this->head = NULL;
        this->tail = NULL;
    } else {
        this->head = this->head->next;
        this->head->prev = NULL;
    }
    oldHead->next = NULL;
    oldHead->prev = NULL;
    this->size--;
    return oldHead;
}

TCB *shortestThread(LinkedList *this) {
    if(this->size <= 0) {
        return NULL;
    }
    if(this->size == 1) return popBack(this);
    TCB *min = this->head;
    TCB *each = this->head;
    while(each != NULL) {
        if(each->quantum_ticks < min->quantum_ticks) {
            min = each;
        }
        each = each->next;
    }
    if(min->prev == NULL) return popFront(this);
    if(min->next == NULL) return popBack(this);
    min->prev->next = min->next;
    min->next->prev = min->prev;
    min->next = NULL;
    min->prev = NULL;
    this->size--;
    return min;
}

TCB *removeTCB(LinkedList *this, worker_t ID) {
    if(this->size <= 0) {
        return NULL;
    }
    TCB *each = this->head;
    while(each != NULL) {
        if(each->ID == ID) {
            if(each->prev == NULL) {
                return popFront(this);
            }
            if(each->next == NULL) {
                return popBack(this);
            }
            each->prev->next = each->next;
            each->next->prev = each->prev;
            each->next = NULL;
            each->prev = NULL;
            return each;
        }
        each = each->next;
    }
    return NULL;
}

void freeLinkedList(LinkedList *this) {
    TCB *each = this->head;
    while(each != NULL) {
        TCB *temp = each;
        each = each->next;
        freeTCB(temp);
    }
    this->head = NULL;
    this->tail = NULL;
    this->size = 0;
    free(this);
}

void setTimerSignalHandler(int signal, void *handler) {
    struct sigaction action;
    action.sa_handler = handler;
    action.sa_flags = SA_RESTART;
    sigaction(signal, &action, NULL);
}

void setTimerSignalBlocker(int signal) {
    sigemptyset(&timerBlocker);
    sigaddset(&timerBlocker, signal);
}

void setTimer(int timerType, int timeQuantum) {
    timer.it_interval.tv_usec = timeQuantum;
    timer.it_interval.tv_sec = 0;
    timer.it_value.tv_usec = timeQuantum;
    timer.it_value.tv_sec = 0;
    setitimer(timerType, &timer, NULL);
}

double exponentiate(double x, int y) {
    double result = 1.0;
    while (y > 0) {
        if (y % 2 == 1) {
            result *= x;
        }
        x *= x;
        y /= 2;
    }
    return result;
}

int calculateTimeQuantum(int level) {
    int time = (int)(QUANTUM * exponentiate(1.5,  level));
    if(time > 500000){
        time = 500000;
    }
    return time;
}
