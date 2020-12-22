#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "protothread.h"

typedef unsigned char u8;
typedef unsigned long long u64;

void fail(char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

typedef struct event_s {
    double time;        // when (absolute time) the event should fire
    int miner;          // which miner gets the block
    bool mining;        // block arrival from mining (true) or peer (false)
    bool fired;         // event's time has been reached (taken out of heap)
    u64 blockid;        // block being mined on top of, or block from peer
} event_t;

// unordered
int event_nalloc;       // event[0..event_nalloc-1]
event_t *event;
int free_events;        // head of list of free event (linked by blockid)

// time-ordered priority queue, entries are indices into events[]
int *heap;              // heap[0..event_nalloc-1]
int nheap = 0;          // number of valid items currently in the heap

// append to the end of the array, then "bubble" it upwards
static void heap_add(int n) {
    assert(nheap < event_nalloc);
    int i = nheap++;
    while (i) {
        int parent = (i-1)/2;
        if (event[heap[parent]].time < event[n].time) {
            break;
        }
        heap[i] = heap[parent];
        i = parent;
    }
    heap[i] = n;
}

static int heap_pop(void) {
    int const r = heap[0];
    if (--nheap == 0) {
        return r;
    }
    // logically we're first moving this last value to a[0]
    int p = heap[nheap];
    int i = 0;
    while (true) {
        int lchild = (i*2)+1;
        if (lchild >= nheap) {
            break;
        }
        int rchild = lchild+1;
        int next_i;
        if (rchild >= nheap ||
                event[heap[lchild]].time < event[heap[rchild]].time) {
            next_i = lchild;
        } else {
            next_i = rchild;
        }
        if (event[p].time < event[heap[next_i]].time) {
            break;
        }
        heap[i] = heap[next_i];
        i = next_i;
    }
    heap[i] = p;
    return r;
}

int event_alloc(void) {
    if (free_events == event_nalloc) {
        int new_nalloc = event_nalloc * 2;
        event = realloc(event, new_nalloc*sizeof(event_t));
        if (!event) fail("out of memory!");
        heap = realloc(heap, new_nalloc*sizeof(int));
        if (!heap) fail("out of memory!");
        for (int i = event_nalloc; i < new_nalloc; i++) {
            memset(&event[i], 0, sizeof(event_t));
            event[i].blockid = i+1;
        }
        event_nalloc = new_nalloc;
    }
    int r = free_events;
    free_events = event[free_events].blockid;
    return r;
}

void event_post(int e, double time) {
    event[e].time = time;
    heap_add(e);
}

void event_free(int i) {
    memset(&event[i], 0, sizeof(event_t));
    event[i].blockid = free_events;
    free_events = i;
}

// Return a random value with Poisson distribution with the given average.
// Useful for block intervals and also network message timings.
static double poisson(double average) {
    // It may be worth using a better RNG here than the default.
    return -log(1.0-(double)random()/RAND_MAX)*average;
}

double current_time;

typedef struct miner_ctx_s {
    pt_thread_t pt_thread;
    pt_func_t pt_func;
    int i;
    int e;  // current event we're waiting for
} miner_ctx_t;

static pt_t miner_thr(env_t const env) {
    miner_ctx_t *c = env;
    pt_resume(c);
    while (true) {
        c->e = event_alloc();
        event_post(c->e, current_time+poisson(300));
        printf("thr %i e %i sleep time %f wakeat %f\n", c->i, c->e, current_time, event[c->e].time);
        while (!event[c->e].fired) pt_wait(c, &((u64*)0)[c->e]);
        printf("thr %i e %i awake time %f\n", c->i, c->e, current_time);
        event_free(c->e);
    }
    return PT_DONE;
}


int main(void) {
    if(0) srandom(time(0));

    event_nalloc = 1;
    event = calloc(1, sizeof(event_t));
    event[0].blockid = 1;
    heap = calloc(1, sizeof(int));

    protothread_t pt = protothread_create();
    for (int i = 0; i < 10; i++) {
        miner_ctx_t * const c = calloc(1, sizeof(*c));
        c->i = i;
        pt_create(pt, &c->pt_thread, miner_thr, c);
    }
    for (int i = 0; i < 10000; i++) {
        while (protothread_run(pt));
        if (!nheap) break;
        int e = heap_pop();
        current_time = event[e].time;
        event[e].fired = true;
        pt_broadcast(pt, &((u64*)0)[e]);
    }

    return 0;
}
