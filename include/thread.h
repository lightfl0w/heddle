#ifndef THREAD_H
#define THREAD_H

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

typedef HANDLE             THREAD;
typedef CRITICAL_SECTION   MUTEX;
typedef CONDITION_VARIABLE COND;

#else

#include <pthread.h>

typedef pthread_t       THREAD;
typedef pthread_mutex_t MUTEX;
typedef pthread_cond_t  COND;

#endif

int thread_create(THREAD *t, void *(*fn)(void *), void *arg);

int thread_join(THREAD t);

void mutex_init(MUTEX *m);

void mutex_lock(MUTEX *m);

void mutex_unlock(MUTEX *m);

void mutex_destroy(MUTEX *m);

void cond_init(COND *c);

void cond_wait(COND *c, MUTEX *m);

void cond_signal(COND *c);

void cond_broadcast(COND *c);

void cond_destroy(COND *c);

#endif
