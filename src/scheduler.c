#include "scheduler.h"
#include "proc.h"
#include "thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int *data;
    int  head;
    int  tail;
    int  cap;
} INTQ;

typedef struct {
    int         node;
    PROC_RESULT result;
} DONE_EVENT;

typedef struct {
    MUTEX        mtx;
    COND         cv_ready;
    COND         cv_done;
    INTQ         ready;
    DONE_EVENT  *done;
    int          done_n;
    int          done_cap;
    int          shutdown;
    int          retry_count;
    const GRAPH *g;
    const char  *cwd;
    const char  *logdir;
} SCHED_STATE;

static void intq_init(INTQ *q, int cap) {
    if (cap < 8) cap = 8;

    q->data = (int *)malloc(sizeof(int) * cap);
    q->head = 0;
    q->tail = 0;
    q->cap  = cap;
}

static void intq_free(INTQ *q) {
    free(q->data);
    q->data = NULL;
}

static int intq_empty(const INTQ *q) { return q->head == q->tail; }

static void intq_push(INTQ *q, int v) {
    int next = (q->tail + 1) % q->cap;

    if (next == q->head) {
        int  oldcap = q->cap;
        int  newcap = oldcap * 2;
        int *nd     = (int *)malloc(sizeof(int) * newcap);
        int  k      = 0;

        for (int i = q->head; i != q->tail; i = (i + 1) % oldcap)
            nd[k++] = q->data[i];

        free(q->data);
        q->data = nd;
        q->head = 0;
        q->tail = k;
        q->cap  = newcap;
        next    = (q->tail + 1) % q->cap;
    }

    q->data[q->tail] = v;
    q->tail = next;
}

static int intq_pop(INTQ *q) {
    int v = q->data[q->head];
    q->head = (q->head + 1) % q->cap;
    return v;
}

static void push_done(SCHED_STATE *s, int node, const PROC_RESULT *r) {
    if (s->done_n == s->done_cap) {
        s->done_cap = s->done_cap ? s->done_cap * 2 : 32;
        s->done = (DONE_EVENT *)realloc(s->done,
                                        sizeof(DONE_EVENT) * s->done_cap);
    }

    s->done[s->done_n].node   = node;
    s->done[s->done_n].result = *r;
    s->done_n++;
}

static void *worker(void *arg) {
    SCHED_STATE *s = (SCHED_STATE *)arg;

    for (;;) {
        mutex_lock(&s->mtx);

        while (intq_empty(&s->ready) && !s->shutdown)
            cond_wait(&s->cv_ready, &s->mtx);

        if (intq_empty(&s->ready) && s->shutdown) {
            mutex_unlock(&s->mtx);
            break;
        }

        int node = intq_pop(&s->ready);
        mutex_unlock(&s->mtx);

        char logpath[1024];
        snprintf(logpath, sizeof(logpath), "%s/%d.log", s->logdir, node);

        PROC_RESULT r;

        if (proc_run(s->g->nodes[node].argv, s->cwd, logpath, &r) != 0) {
            r.exit_code = -1;
            r.signaled  = 0;
            r.signal    = 0;
        }

        mutex_lock(&s->mtx);
        push_done(s, node, &r);
        cond_signal(&s->cv_done);
        mutex_unlock(&s->mtx);
    }
    return NULL;
}

static void block_push(int **stack, int *n, int *cap, int v) {
    if (*n == *cap) {
        *cap *= 2;
        *stack = (int *)realloc(*stack, sizeof(int) * (size_t)*cap);
    }
    (*stack)[(*n)++] = v;
}

static int block_descendants(const GRAPH *g, int start,
                             char *blocked, char *done) {
    int  cap   = 32;
    int  n     = 0;
    int  count = 0;
    int *stack = (int *)malloc(sizeof(int) * cap);

    for (int i = 0; i < g->nodes[start].nrdeps; i++) {
        int d = g->nodes[start].rdeps[i];

        if (blocked[d] || done[d]) continue;

        block_push(&stack, &n, &cap, d);
        blocked[d] = 1;
        count++;
    }

    while (n > 0) {
        int cur = stack[--n];

        for (int i = 0; i < g->nodes[cur].nrdeps; i++) {
            int d = g->nodes[cur].rdeps[i];

            if (blocked[d] || done[d]) continue;

            block_push(&stack, &n, &cap, d);
            blocked[d] = 1;
            count++;
        }
    }

    free(stack);
    return count;
}

static void complete_node(SCHED_STATE *s, int nd, int *indeg,
                          char *done, char *blocked, int *remaining) {
    done[nd] = 1;
    (*remaining)--;

    mutex_lock(&s->mtx);

    const GRAPH *g = s->g;

    for (int k = 0; k < g->nodes[nd].nrdeps; k++) {
        int d = g->nodes[nd].rdeps[k];

        if (blocked[d] || done[d]) continue;

        indeg[d]--;

        if (indeg[d] == 0) intq_push(&s->ready, d);
    }

    cond_broadcast(&s->cv_ready);
    mutex_unlock(&s->mtx);
}

static void fail_node(SCHED_STATE *s, int nd, int *tries, int *indeg,
                      char *done, char *blocked, int *remaining, int *failed) {
    tries[nd]++;

    if (tries[nd] <= s->retry_count) {
        mutex_lock(&s->mtx);
        intq_push(&s->ready, nd);
        cond_signal(&s->cv_ready);
        mutex_unlock(&s->mtx);

        fprintf(stderr, "[retry %d/%d] node %d\n", tries[nd], s->retry_count, nd);
        return;
    }

    *failed = 1;
    blocked[nd] = 1;
    (*remaining)--;

    *remaining -= block_descendants(s->g, nd, blocked, done);
}

int sched_run(const SCHED_OPTS *o) {
    int n = o->g->n;

    SCHED_STATE s;
    memset(&s, 0, sizeof(s));

    s.g = o->g;
    s.cwd = o->cwd;
    s.logdir = o->logdir;
    s.retry_count = o->retry;

    mutex_init(&s.mtx);
    cond_init(&s.cv_ready);
    cond_init(&s.cv_done);
    intq_init(&s.ready, n + 2);

    int *tries    = (int *)calloc(n ? n : 1, sizeof(int));
    char *done    = (char *)calloc(n ? n : 1, 1);
    char *blocked = (char *)calloc(n ? n : 1, 1);
    int *indeg    = (int *)malloc(sizeof(int) * (n ? n : 1));

    for (int i = 0; i < n; i++) indeg[i] = o->g->nodes[i].indeg;
    for (int i = 0; i < n; i++)
        if (indeg[i] == 0) intq_push(&s.ready, i);

    int nthreads = o->jobs;

    if (nthreads < 1) nthreads = 1;
    if (nthreads > n && n > 0) nthreads = n;
    if (n == 0) nthreads = 0;

    THREAD *threads =
        (THREAD *)malloc(sizeof(THREAD) * (nthreads ? nthreads : 1));

    for (int i = 0; i < nthreads; i++)
        thread_create(&threads[i], worker, &s);

    int remaining = n;
    int failed    = 0;

    while (remaining > 0) {
        mutex_lock(&s.mtx);

        while (s.done_n == 0)
            cond_wait(&s.cv_done, &s.mtx);

        int cnt = s.done_n;

        DONE_EVENT *local = (DONE_EVENT *)malloc(sizeof(DONE_EVENT) * cnt);
        memcpy(local, s.done, sizeof(DONE_EVENT) * cnt);

        s.done_n = 0;
        mutex_unlock(&s.mtx);

        for (int i = 0; i < cnt; i++) {
            int nd = local[i].node;
            PROC_RESULT *r = &local[i].result;

            if (r->exit_code == 0 && !r->signaled)
                complete_node(&s, nd, indeg, done, blocked, &remaining);
            else
                fail_node(&s, nd, tries, indeg, done, blocked,
                          &remaining, &failed);
        }

        free(local);

        if (failed && !o->keep_going) {
            for (int i = 0; i < n; i++)
                if (!done[i] && !blocked[i]) blocked[i] = 1;
            break;
        }
    }

    mutex_lock(&s.mtx);

    while (!intq_empty(&s.ready)) intq_pop(&s.ready);

    s.shutdown = 1;
    cond_broadcast(&s.cv_ready);
    mutex_unlock(&s.mtx);

    for (int i = 0; i < nthreads; i++)
        thread_join(threads[i]);

    free(threads);
    free(tries);
    free(done);
    free(blocked);
    free(indeg);
    intq_free(&s.ready);
    free(s.done);

    cond_destroy(&s.cv_ready);
    cond_destroy(&s.cv_done);
    mutex_destroy(&s.mtx);

    return failed ? 1 : 0;
}
