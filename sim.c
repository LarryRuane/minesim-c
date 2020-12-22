#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>

#include "protothread.h"

typedef unsigned char u8;
typedef unsigned long long u64;

void fail(char *message) {
    fprintf(stderr, "%s", message);
    exit(1);
}

struct event_s {
    double time;        // when (absolute time) the event should fire
    int miner;          // which miner gets the block
    bool mining;        // block arrival from mining (true) or peer (false)
    u64 blockid;        // block being mined on top of, or block from peer
};
// unordered
int event_alloc;        // event[0..event_alloc-1]
struct event_s *event;
int free_events;        // first free event (linked by blockid)

// time-ordered priority queue, entries are indices into events[]
int *heap;              // heap[0..event_alloc-1]
int nheap = 0;          // number of valid items currently in the heap

int alloc_event(void) {
    if (free_events == event_alloc) {
        int new_alloc = event_alloc * 2;
        event = realloc(event, new_alloc*sizeof(struct event_s));
        if (!event) fail("out of memory!");
        heap = realloc(heap, new_alloc*sizeof(int));
        if (!heap) fail("out of memory!");
        for (int i = event_alloc; i < new_alloc; i++) {
            event[i].blockid = i+1;
        }
        event_alloc = new_alloc;
    }
    int r = free_events;
    free_events = event[free_events].blockid;
    return r;
}

// append to the end of the array, then "bubble" it upwards
static void heap_add(int n) {
    assert(nheap < event_alloc);
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

double currenttime;

struct miner_ctx_s {
};


int main(void) {
    if(1) srandom(time(0));

    event_alloc = 1;
    event = malloc(sizeof(struct event_s));
    event[0].blockid = 1;
    heap = malloc(sizeof(int));

    int n = 110;
    for (int i = 0; i < n; i++) {
        int e = alloc_event();
        event[e].time = -log((double)1.0-(double)random()/RAND_MAX)*300;
        heap_add(e);
    }
    for (int i = 0; i < n; i++) {
        printf("%f ", event[heap_pop()].time);
    }
    printf("\n");
    return 0;
}
