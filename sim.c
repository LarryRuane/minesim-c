#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>

typedef unsigned char u8;
typedef unsigned long long u64;
typedef u8 bool;
#define false 0
#define true 1

#define NEVENT 4000

struct event_s {
    double time;        // when (absolute time) the event should fire
    int miner;          // which miner gets the block
    bool mining;        // block arrival from mining (true) or peer (false)
    u64 blockid;   // block being mined on top of, or block from peer
};
// unordered
struct event_s event[NEVENT];
u64 free_events;   // linked by blockid

// time-ordered priority queue, entries are indices into events[]
int nheap = 0;  // number of items currently in the heap
int heap[NEVENT];

// append to the end of the array, then "bubble" it upwards
static void heap_add(int n) {
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

int main(void) {
    if(1) srandom(time(0));
    int n = 110;
    for (int i = 0; i < n; i++) {
        event[i].time = -log((double)1.0-(double)random()/RAND_MAX)*300;
    }
    for (int i = 0; i < n; i++) {
        heap_add(i);
    }
    for (int i = 0; i < n; i++) {
        printf("%f ", event[heap_pop()].time);
    }
    printf("\n");
    return 0;
}
