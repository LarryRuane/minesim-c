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

static int randrange(int i) {
    return random() % i;
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

typedef struct peer_s {
    int ni;
    double delay;
} peer_t;

#define NPEER 100
typedef struct node_ctx_s {
    pt_thread_t pt_thread;
    pt_func_t pt_func;
    int i;
    int e;  // current event we're waiting for
    bool is_miner;
    peer_t peer[NPEER]; // make this variable etc.
} node_ctx_t;

int nnode = 1 << 15; // must be even, 32k for now
node_ctx_t *node;

#define delay(_time) do { \
    c->e = event_alloc(); \
    event_post(c->e, current_time + _time); \
    while (!event[c->e].fired) pt_wait(c, &((u64*)0)[c->e]); \
    event_free(c->e); \
} while (false)

static pt_t node_thr(env_t const env) {
    node_ctx_t *c = env;
    pt_resume(c);
    c->is_miner = (randrange(10) == 0);
    // make 10 outbound connections
    int pi = 0;
    for (int i = 0; i < 10; i++) {
        // find an available local slot
        while (pi < NPEER && c->peer[pi].delay > 0) pi++;
        if (pi >= NPEER) break;

        // perfer nodes that are "close" to us
        int d, peer_mi, ppi;
        while (true) {
            d = 1 + randrange(1 << randrange(16));
            peer_mi = (c->i + d) % nnode;

            // see if this peer is already in our peer list
            int j = 0;
            for (j = 0; j < NPEER; j++) {
                if (c->peer[j].delay > 0 && c->peer[j].ni == peer_mi) break;
            }
            if (j < NPEER) continue;

            // find an available peer slot
            for (ppi = 0; ppi < NPEER; ppi++) {
                if (node[peer_mi].peer[ppi].delay == 0) break;
            }
            if (ppi < NPEER) break;
        }
        c->peer[pi].ni = peer_mi;
        // one hop away is 1 ms
        c->peer[pi].delay = (double)d / 1000;
        // make it bidirectional
        node[peer_mi].peer[ppi].ni = c->i;
        node[peer_mi].peer[ppi].delay = c->peer[pi].delay;
    }

    while (true) {
        double delay_time = poisson(300);
        if(0) printf("thr %i e %i sleep time %f wakeat %f\n",
            c->i, c->e, current_time, current_time+delay_time);
        delay(delay_time);
    }
    return PT_DONE;
}

int main(void) {
    {
        static char rngstate[256];
        initstate(0 /*time(0)*/, rngstate, sizeof(rngstate));
    }

    event_nalloc = 1;
    event = calloc(1, sizeof(event_t));
    event[0].blockid = 1;
    heap = calloc(1, sizeof(int));
    node = calloc(nnode, sizeof(node_ctx_t));

    protothread_t pt = protothread_create();
    for (int i = 0; i < nnode; i++) {
        node_ctx_t *c = &node[i];
        c->i = i;
        pt_create(pt, &c->pt_thread, node_thr, c);
    }
    for (int i = 0; i < 8*1000; i++) {
        while (protothread_run(pt));
        if (!nheap) break;
        int e = heap_pop();
        current_time = event[e].time;
        event[e].fired = true;
        pt_broadcast(pt, &((u64*)0)[e]);
    }
    for (int i = 0; i < nnode; i++) {
        printf("%d: ", i);
        for (int j = 0; j < NPEER; j++) {
            if (node[i].peer[j].delay == 0) continue;
            printf("[%d %f], ", node[i].peer[j].ni, node[i].peer[j].delay);
        }
        printf("\n");
    }

    return 0;
}
