// File: worker_t.h

// List all group member's name: Nikoloz Chichua
// username of iLab: nc733
// iLab Server: iLab3.cs.rutgers.edu

#ifndef WORKER_T_H
#define WORKER_T_H

#define _GNU_SOURCE

#define USE_WORKERS 1

#define QUANTUM      25000
#define MAX_QUANTUM 300000
#define BOOST_TIME  1000000
#define MLFQ_SIZE 8
#define YIELD_LIMIT 5


#define STACK_SIZE SIGSTKSZ

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <math.h>

typedef uint worker_t;

typedef uint worker_t;
typedef enum Status Status;
typedef struct TCB TCB;
typedef struct LinkedList LinkedList;

enum Status {
    READY,
    BLOCKED,
    DESCHEDULED
};

struct TCB {
    worker_t ID;
    Status state;
    ucontext_t context;
    void *return_value;
    int quantum_ticks;
    bool yielded;
    int priority;
    TCB *blocked_thread;
    worker_t blocking_thread_ID;
    clock_t arrival;
    clock_t first_run;
    clock_t completion;
    // For Linked List
    TCB *next;
    TCB *prev;
};

TCB *newTCB(worker_t ID, ucontext_t *link, void *(*function)(void*), void *arg);
ucontext_t getContext(TCB *this);
void freeTCB(TCB *this);

struct LinkedList {
    TCB *head;
    TCB *tail;
    int size;
};

LinkedList *newLinkedList();
void append(LinkedList *this, TCB *thread);
void prepend(LinkedList *this, TCB *thread);
TCB *findThread(LinkedList *this, worker_t ID);
TCB *findThreadInMLFQ(worker_t ID);
TCB *popBack(LinkedList *this);
TCB *popFront(LinkedList *this);
TCB *shortestThread(LinkedList *this);
TCB *removeTCB(LinkedList *this, worker_t ID);
void boostJobs();
void freeLinkedList(LinkedList *this);

void setTimerSignalHandler(int signal, void *handler);
void setTimerSignalBlocker(int signal);
void setTimer(int timerType, int timeQuantum);

typedef struct worker_mutex_t {
    bool initialized;
    worker_t owner;
    bool locked;
} worker_mutex_t;


int worker_create(worker_t *thread, pthread_attr_t *attr, void *(*function)(void*), void *arg);

int worker_yield();

void worker_exit(void *value_ptr);

int worker_join(worker_t thread, void **value_ptr);

int worker_mutex_init(worker_mutex_t *mutex, const pthread_mutexattr_t *mutexattr);

int worker_mutex_lock(worker_mutex_t *mutex);

int worker_mutex_unlock(worker_mutex_t *mutex);

int worker_mutex_destroy(worker_mutex_t *mutex);

double exponentiate(double x, int y);
int calculateTimeQuantum(int level);
void print_app_stats(void);

#ifdef USE_WORKERS
#define pthread_t worker_t
#define pthread_mutex_t worker_mutex_t
#define pthread_create worker_create
#define pthread_exit worker_exit
#define pthread_join worker_join
#define pthread_mutex_init worker_mutex_init
#define pthread_mutex_lock worker_mutex_lock
#define pthread_mutex_unlock worker_mutex_unlock
#define pthread_mutex_destroy worker_mutex_destroy
#endif

#endif

