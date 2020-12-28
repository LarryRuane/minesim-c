#ifndef PROTOTHREAD_H
#define PROTOTHREAD_H 1
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>

#ifndef PT_DEBUG
#define PT_DEBUG 1  /* enabled (else 0) */
#endif
#define pt_assert(condition) do { if (PT_DEBUG) assert(condition); } while (0)

/* standard definitions */
#include <stdbool.h>
typedef void * env_t;

/* Number of wait queues (size of wait hash table), power of 2 */
#define PT_NWAIT (1 << 16)

/* Function return values; hide things a bit so user can't
 * accidentally return a NULL or an integer.
 */
enum pt_return_e {
    PT_RETURN_WAIT,
    PT_RETURN_DONE,
};

typedef struct pt_return_s {
    enum pt_return_e pt_rv;
} pt_t;

/* pointer to a top-level protothread function
 */
typedef pt_t (*pt_f_t)(env_t);

/* One per thread:
 */
struct pt_thread_s {
    struct pt_thread_s * next;         /* next thread in wait or run list */
    pt_f_t func;                       /* top level function */
    env_t env;                         /* top level function's context */
    void *channel;                     /* if waiting (never dereferenced) */
    struct protothread_s * s;          /* pointer to state */
    void (*atexit)(env_t env);         /* optional user defined destructor */
#if PT_DEBUG
    struct pt_func_s * pt_func;        /* top-level function's pt_func_t */
#endif
};
typedef struct pt_thread_s pt_thread_t;

/* Usually there is one instance of struct protothread_s for
 * the overall system.
 */
typedef struct protothread_s {
    void (*ready_function)(env_t); /* call when a thread becomes ready */
    env_t ready_env;               /* environment to pass to ready_function() */
    pt_thread_t *running;          /* current running protothread (or NULL) */
    pt_thread_t *ready;            /* ready to run list (points to newest) */
    pt_thread_t *wait[PT_NWAIT];   /* waiting for an event (points to newest) */
} *protothread_t;

typedef struct protothread_s *state_t;

static inline pt_t
pt_return_wait(void) {
    pt_t p;
    p.pt_rv = PT_RETURN_WAIT;
    return p;
}

static inline pt_t
pt_return_done(void) {
    pt_t p;
    p.pt_rv = PT_RETURN_DONE;
    return p;
}

#define PT_WAIT pt_return_wait()
#define PT_DONE pt_return_done()


/* One of these per nested function (call frame); every function environment
 * struct must contain one of these.
 */
typedef struct pt_func_s {
    pt_thread_t * thread;
    void *label;                   /* function resume point (goto target) */
#if PT_DEBUG
    struct pt_func_s * next;       /* pt_func of function that we called */
    char const * file;             /* __FILE__ */
    int line;                      /* __LINE__ */
    char const * function;         /* __FUNCTION__ */
#endif
} pt_func_t;

/* This should be at the beginning of every protothread function */
#define pt_resume(c) do { if ((c)->pt_func.label) goto *(c)->pt_func.label; } while (0)

/* This can be used to reset a thread or thread function */
#define pt_reset(c) do { (c)->pt_func.label = NULL; } while (0)

/* link thread as the newest in the given (ready or wait) list */
static inline void
pt_link(pt_thread_t ** const head, pt_thread_t * const n) {
    if (*head) {
        n->next = (*head)->next;
        (*head)->next = n;
    } else {
        n->next = n;
    }
    *head = n;
}

/* unlink and return the thread following prev, updating head if necessary */
static inline pt_thread_t *
pt_unlink(pt_thread_t ** const head, pt_thread_t * const prev) {
    pt_thread_t * const next = prev->next;
    prev->next = next->next;
    if (next == prev) {
        *head = NULL;
    } else if (next == *head) {
        *head = prev;
    }
    if (PT_DEBUG) {
        next->next = NULL;
    }
    return next;
}

/* unlink and return the oldest (last) thread */
static inline pt_thread_t *
pt_unlink_oldest(pt_thread_t ** const head) {
    return pt_unlink(head, *head);
}

/* finds thread <n> in list <head> and unlinks it.  Returns TRUE if
 * it was found.
 */
bool pt_find_and_unlink(pt_thread_t ** const head, pt_thread_t * const n);

void pt_add_ready(state_t const s, pt_thread_t * const t);

/* This is called by pt_create(), not by user code directly */
void pt_create_thread(
        state_t const s,
        pt_thread_t * const t,
        pt_func_t * const pt_func,
        pt_f_t const func,
        env_t env
);

/* sets a user defined callback for finalization at the end of pt_kill() */
void pt_set_atexit(pt_thread_t * pt, void (*func)(env_t));

/* should only be called by the macro pt_yield() */
void pt_enqueue_yield(pt_thread_t * const t);

/* Return which wait list to use (hash table)
 * https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
 * #FNV-1a_hash
 */
static inline pt_thread_t **pt_get_wait_list(state_t const s, void * chan) {
    return &s->wait[((uintptr_t)chan >> 4) & (PT_NWAIT-1)];
}

/* should only be called by the macro pt_wait() */
void pt_enqueue_wait(pt_thread_t * const t, void * const channel);

/* Construct goto labels using the current line number (so they are unique). */
#define PT_LABEL_HELP2(line) pt_label_ ## line
#define PT_LABEL_HELP(line) PT_LABEL_HELP2(line)
#define PT_LABEL PT_LABEL_HELP(__LINE__)

#if !PT_DEBUG
#define pt_debug_save(env)
#define pt_debug_wait(env)
#define pt_debug_call(env, child_env)
#else
#define pt_debug_save(env) do { \
    (env)->pt_func.file = __FILE__; \
    (env)->pt_func.line = __LINE__; \
    (env)->pt_func.function = __func__; \
} while (0)

#define pt_debug_wait(env) do { \
    pt_debug_save(env); \
    (env)->pt_func.next = NULL; \
} while (0)

#define pt_debug_call(env, child_env) do { \
    pt_debug_save(env); \
    (env)->pt_func.next = &(child_env)->pt_func; \
} while (0)

#endif

/* Wait for a channel to be signaled */
#define pt_wait(env, channel) \
    do { \
        (env)->pt_func.label = &&PT_LABEL; \
        pt_enqueue_wait((env)->pt_func.thread, channel); \
        pt_debug_wait(env); \
        return PT_WAIT; \
      PT_LABEL:; \
    } while (0)

/* Let other ready protothreads run, then resume this thread */
#define pt_yield(env) \
    do { \
        (env)->pt_func.label = &&PT_LABEL; \
        pt_enqueue_yield((env)->pt_func.thread); \
        pt_debug_wait(env); \
        return PT_WAIT; \
      PT_LABEL:; \
    } while (0)

/* Call a function (which may wait) */
#define pt_call(env, child_func, child_env, ...) \
    do { \
        (child_env)->pt_func.thread = (env)->pt_func.thread; \
        (child_env)->pt_func.label = NULL; \
        (env)->pt_func.label = NULL; \
        pt_debug_call(env, child_env); \
      PT_LABEL: \
        if (child_func(child_env, ##__VA_ARGS__).pt_rv == PT_WAIT.pt_rv) { \
            (env)->pt_func.label = &&PT_LABEL; \
            return PT_WAIT; \
        } \
    } while (0)

/* Did the most recent pt_call() block (break context)? */
#define pt_call_waited(env) ((env)->pt_func.label != NULL)

#define pt_create(pt, thr, func, env) \
    pt_create_thread(pt, thr, &(env)->pt_func, func, env);

/* This allows protothreads (which might not have an explicit pointer to the
 * protothread object) to call pt_create(), pt_signal() or pt_broadcast().
 */
static inline protothread_t
pt_get_protothread(pt_func_t const * pt_func) {
    return pt_func->thread->s;
}
#define pt_get_pt(env) pt_get_protothread(&(env)->pt_func)

void protothread_init(state_t const s);
state_t protothread_create(void);
void protothread_deinit(state_t const s);
void protothread_free(state_t const s);
bool protothread_run(state_t const s);

/* Set a function to call when a protothread becomes ready. 
 * This is optional.  The passed function will generally
 * schedule a function that will call prothread_run() repeatedly
 * until it returns FALSE (or, if it limits the number of calls
 * and the last call to protothread_run() returned TRUE, it
 * must reschedule itself).
 */
void protothread_set_ready_function(
        state_t const s, void (*f)(env_t), env_t env);

/* Make the thread or threads that are waiting on the given
 * channel (if any) runnable.
 */
void pt_wake(state_t const s, void * const channel, bool const wake_one);

static inline void
pt_signal(state_t const s, void * const channel) {
    pt_wake(s, channel, true);
}

static inline void
pt_broadcast(state_t const s, void * const channel) {
    pt_wake(s, channel, false);
}

/* This is used to prevent a thread from scheduling again.  This can be
 * very dangerous if the thread in question isn't written to expect this
 * operation.
 */
bool pt_kill(pt_thread_t * const t);

#endif
