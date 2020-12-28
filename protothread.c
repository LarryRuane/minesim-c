#include "protothread.h"

#ifndef PT_DEBUG
#define PT_DEBUG 1  /* enabled (else 0) */
#endif
#define pt_assert(condition) do { if (PT_DEBUG) assert(condition); } while (0)

/* finds thread <n> in list <head> and unlinks it.  Returns TRUE if
 * it was found.
 */
bool pt_find_and_unlink(pt_thread_t ** const head, pt_thread_t * const n) {
    pt_thread_t * prev = *head;

    while (*head) {
        pt_thread_t * const t = prev->next;
        if (n == t) {
            pt_unlink(head, prev);
            return true;
        }
        /* Advance to next thread */
        prev = t;
        /* looped back to start? finished */
        if (prev == *head) {
            break;
        }
    }
    return false;
}

void pt_add_ready(state_t const s, pt_thread_t * const t) {
    if (s->ready_function && !s->ready && !s->running) {
        /* this should schedule protothread_run() */
        s->ready_function(s->ready_env);
    }
    pt_link(&s->ready, t);
}

/* This is called by pt_create(), not by user code directly */
void pt_create_thread(
        state_t const s,
        pt_thread_t * const t,
        pt_func_t * const pt_func,
        pt_f_t const func,
        env_t env
) {
    pt_func->thread = t;
    pt_func->label = NULL;
    t->func = func;
    t->env = env;
    t->s = s;
    t->channel = NULL;
#if PT_DEBUG
    t->pt_func = pt_func;
    t->next = NULL;
#endif

    /* add the new thread to the ready list */
    pt_add_ready(s, t);
}

/* sets a user defined callback for finalization at the end of pt_kill() */
void pt_set_atexit(pt_thread_t * pt, void (*func)(env_t)) {
    pt->atexit = func;
}

/* should only be called by the macro pt_yield() */
void pt_enqueue_yield(pt_thread_t * const t) {
    state_t const s = t->s;
    pt_assert(s->running == t);
    pt_add_ready(s, t);
}

/* should only be called by the macro pt_wait() */
void pt_enqueue_wait(pt_thread_t * const t, void * const channel) {
    state_t const s = t->s;
    pt_thread_t ** const wq = pt_get_wait_list(s, channel);
    pt_assert(s->running == t);
    t->channel = channel;
    pt_link(wq, t);
}

void protothread_init(state_t const s) {
    memset(s, 0, sizeof(*s));
}

state_t protothread_create(void) {
    state_t const s = malloc(sizeof(*s));
    protothread_init(s);
    return s;
}

void protothread_deinit(state_t const s) {
    if (PT_DEBUG) {
        int i;
        for (i = 0; i < PT_NWAIT; i++) {
            pt_assert(s->wait[i] == NULL);
        }
        pt_assert(s->ready == NULL);
        pt_assert(s->running == NULL);
    }
}

void protothread_free(state_t const s) {
    protothread_deinit(s);
    free(s);
}

bool protothread_run(state_t const s) {
    pt_assert(s->running == NULL);
    if (s->ready == NULL) {
        return false;
    }

    /* unlink the oldest ready thread */
    s->running = pt_unlink_oldest(&s->ready);

    /* run the thread */
    s->running->func(s->running->env);
    s->running = NULL;

    /* return true if there are more threads to run */
    return s->ready != NULL;
}

/* Set a function to call when a protothread becomes ready. 
 * This is optional.  The passed function will generally
 * schedule a function that will call prothread_run() repeatedly
 * until it returns FALSE (or, if it limits the number of calls
 * and the last call to protothread_run() returned TRUE, it
 * must reschedule itself).
 */
void protothread_set_ready_function(
        state_t const s, void (*f)(env_t), env_t env) {
    s->ready_function = f;
    s->ready_env = env;
}

/* Make the thread or threads that are waiting on the given
 * channel (if any) runnable.
 */
void pt_wake(state_t const s, void * const channel, bool const wake_one) {
    pt_thread_t ** const wq = pt_get_wait_list(s, channel);
    pt_thread_t * prev = *wq;  /* one before the oldest waiting thread */

    while (*wq) {
        pt_thread_t * const t = prev->next;
        if (t->channel != channel) {
            /* advance to next thread on wait list */
            prev = t;
            /* looped back to start? done */
            if (prev == *wq) {
                break;
            }
        } else {
            /* wake up this thread (link to the ready list) */
            pt_unlink(wq, prev);
            pt_add_ready(s, t);
            if (wake_one) {
                /* wake only the first found thread */
                break;
            }
        }
    }
}

/* This is used to prevent a thread from scheduling again.  This can be
 * very dangerous if the thread in question isn't written to expect this
 * operation.
 */
bool pt_kill(pt_thread_t * const t) {
    state_t const s = t->s;
    pt_assert(s->running != t);

    if (!pt_find_and_unlink(&s->ready, t)) {
        pt_thread_t ** const wq = pt_get_wait_list(s, t->channel);
        if (!pt_find_and_unlink(wq, t)) {
            return false;
        }
    }
    if (t->atexit) {
        t->atexit(t->env);
    }
    return true;
}
