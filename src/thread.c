#include "thread.h"

#include <stdlib.h>

#ifdef _WIN32

typedef struct {
    void *(*fn)(void *);
    void  *arg;
} THREAD_START;

static DWORD WINAPI thread_trampoline(LPVOID p) {
    THREAD_START *s = (THREAD_START *)p;

    void *(*fn)(void *) = s->fn;
    void  *arg          = s->arg;

    free(s);
    fn(arg);
    return 0;
}

int thread_create(THREAD *t, void *(*fn)(void *), void *arg) {
    THREAD_START *s = (THREAD_START *)malloc(sizeof(*s));
    if (!s) return -1;

    s->fn  = fn;
    s->arg = arg;

    *t = CreateThread(NULL, 0, thread_trampoline, s, 0, NULL);
    if (!*t) { free(s); return -1; }
    return 0;
}

int thread_join(THREAD t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

void mutex_init(MUTEX *m) { InitializeCriticalSection(m); }
void mutex_lock(MUTEX *m) { EnterCriticalSection(m); }
void mutex_unlock(MUTEX *m) { LeaveCriticalSection(m); }
void mutex_destroy(MUTEX *m) { DeleteCriticalSection(m); }

void cond_init(COND *c) { InitializeConditionVariable(c); }
void cond_wait(COND *c, MUTEX *m) { SleepConditionVariableCS(c, m, INFINITE); }
void cond_signal(COND *c) { WakeConditionVariable(c); }
void cond_broadcast(COND *c) { WakeAllConditionVariable(c); }
void cond_destroy(COND *c) { (void)c; }

#else

int thread_create(THREAD *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}

int thread_join(THREAD t) {
    return pthread_join(t, NULL);
}

void mutex_init(MUTEX *m) { pthread_mutex_init(m, NULL); }
void mutex_lock(MUTEX *m) { pthread_mutex_lock(m); }
void mutex_unlock(MUTEX *m) { pthread_mutex_unlock(m); }
void mutex_destroy(MUTEX *m) { pthread_mutex_destroy(m); }

void cond_init(COND *c) { pthread_cond_init(c, NULL); }
void cond_wait(COND *c, MUTEX *m) { pthread_cond_wait(c, m); }
void cond_signal(COND *c) { pthread_cond_signal(c); }
void cond_broadcast(COND *c) { pthread_cond_broadcast(c); }
void cond_destroy(COND *c) { pthread_cond_destroy(c); }

#endif
