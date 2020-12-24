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

int randrange(int i) {
    return random() % i;
}

double current_time;

typedef struct block_s {
    u64 parent; // first block is the only block with parent = zero
    u64 height; // more than one block can have the same height
    int miner;  // which miner found this block
    int active; // number of miners actively mining directly on this block
} block_t;

block_t *block;     // blockchain, oldest first
int block_nalloc;   // number of allocated blocks
int nblock;         // len(block)
u64 baseblockid;    // blocks[0] corresponds to this block id
int ntips;          // number of blocks being actively mined on
int maxreorg;       // greatest depth reorg
double totalhash;   // sum of miners' hashrates

void block_init(void) {
    block_nalloc = 1;
    block = calloc(block_nalloc, sizeof(block_t));
    block[0] = (block_t) { 0, 0, -1, 0 };
    nblock = 1;
    baseblockid = 1000; // arbitrary but helps distinguish ids from heights
    ntips = 0;
}

// allocate one new block, return its index.
int block_alloc(void) {
    if (nblock == block_nalloc) {
        block_nalloc *= 2;
        block = realloc(block, block_nalloc*sizeof(block_t));
        if (!block) fail("out of memory!");
        memset(&block[nblock], 0, nblock*sizeof(block_t));
    }
    return nblock++;
}

bool validblock(u64 blockid) {
    return blockid >= baseblockid &&
        blockid - baseblockid < (u64)nblock;
}
block_t *getblock(u64 blockid) {
    assert(blockid >= baseblockid);
    assert(blockid < baseblockid + nblock);
    return &block[blockid - baseblockid];
}
u64 getheight(u64 blockid) {
    return getblock(blockid)->height;
}

typedef struct event_s {
    double time;        // when (absolute time) the event should fire
    void (*notify)(protothread_t, int);
    int next;           // for freelist or node input queue
    union {
        struct {
            int ni;             // node index
        } delay;
        struct {
            int ni;             // index of receiving node
            bool mining;        // block arrival from mining or peer
            u64 blockid;        // parent of new block, or block from peer
        } new_block;
    } u;
} event_t;

// unordered
int event_nalloc;       // event[0..event_nalloc-1]
event_t *event;
int free_events;        // head of list of free event

void event_init(void) {
    event_nalloc = 1;
    event = calloc(1, sizeof(event_t));
    event[0].next = 1;
    free_events = 0;
}

bool event_pending(int e) {
    return event[e].time > current_time;
}

// time-ordered priority queue, entries are indices into events[]
int *heap;  // heap[0..event_nalloc-1]
int nheap;  // number of valid items currently in the heap

void heap_init(void) {
    heap = calloc(1, sizeof(int));
    nheap = 0;
}

// append to the end of the array, then "bubble" it upwards
void heap_add(int n) {
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

int heap_pop(void) {
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
            event[i].next = i+1;
        }
        event_nalloc = new_nalloc;
    }
    int r = free_events;
    free_events = event[free_events].next;
    return r;
}

void event_post(int e, double time) {
    event[e].time = time;
    heap_add(e);
}

void event_free(int i) {
    memset(&event[i], 0, sizeof(event_t));
    event[i].next = free_events;
    free_events = i;
}

// Return a random value with Poisson distribution with the given average.
// Useful for block intervals and also network message timings.
double poisson(double average) {
    // It may be worth using a better RNG here than the default.
    return -log(1.0-(double)random()/RAND_MAX)*average;
}

typedef struct peer_s {
    int ni;
    double delay;
} peer_t;

#define NPEER 100

typedef struct node_s {
    pt_thread_t pt_thread;
    pt_func_t pt_func;
    int ni;             // my node index
    int qhead;          // event input message queue, -1 means empty
    int delay_event;    // event index
    u64 current;        // blockid being mined on
    double hashrate;
    u64 mined;          // how many total blocks we've mined (including reorg)
    u64 credit;         // how many best-chain blocks we've mined
    peer_t peer[NPEER]; // maybe make this variable-length?
} node_t;

// later there will be a dynamic set of nodes
int nnode = 1 << 15; // must be even, 32k for now
node_t *node;

void stop_mining(int ni) {
    node_t *np = &node[ni];
    block_t *bp = getblock(np->current);
    if (--bp->active == 0) ntips--;
}

// Relay a newly-discovered block (either we mined or relayed to us).
// This sends a message to the peer we received the block from (if it's one
// of our peers), but that's okay, it will be ignored.
void relay_notify(protothread_t pt, int e) {
    event_t *ep = &event[e];
    node_t *np = &node[ep->u.new_block.ni];

    // link to list of incoming block notify messages
    ep->next = np->qhead;
    np->qhead = e;

    pt_signal(pt, &np->qhead);
}

void relay(int ni, u64 newblockid) {
    node_t *np = &node[ni];
    for (int pi = 0; pi < NPEER; pi++) {
        peer_t *pp = &np->peer[pi];
        if (pp->delay == 0) continue;
        // Improve simulator efficiency by not relaying blocks
        // that are certain to be ignored.
        node_t *ppn = &node[pp->ni];
        if (!validblock(ppn->current) ||
                getheight(ppn->current) < getheight(newblockid)) {
            int e = event_alloc();
            event_t *ep = &event[e];
            ep->u.new_block.ni = pi;
            ep->u.new_block.mining = false;
            ep->u.new_block.blockid = newblockid;
            ep->notify = relay_notify;
            // TODO jitter this delay, or sometimes fail to forward?
            event_post(e, current_time + pp->delay);
        }
    }
}

// Start mining on top of the given existing block
void start_mining(int ni, u64 blockid) {
    node_t *np = &node[ni];
    // We'll mine on top of blockid
    np->current = blockid;
    if (!np->hashrate) return;
    block_t *bp = getblock(blockid);
    if (bp->active++ == 0) ntips++;

    // Schedule an event for when our "mining" will be done.
    double solvetime = poisson(300 * totalhash / np->hashrate);

    int e = event_alloc();
    event_t *ep = &event[e];
    ep->u.new_block.ni = ni;
    ep->u.new_block.mining = true;
    ep->u.new_block.blockid = blockid;
    ep->notify = relay_notify;
    // TODO jitter this delay, or sometimes fail to forward?
    event_post(e, current_time + solvetime);
    printf("%.3f %03d start-on %llu height %llu "
            "mined %lld credit %lld solve %.2f\n",
        current_time, ni, blockid, getheight(blockid),
        np->mined, np->credit, solvetime);
}

void delay_notify(protothread_t pt, int e) {
    pt_signal(pt, &node[event[e].u.delay.ni].delay_event);
}
// This could be a (proto)function, but then it would need its own
// thread context. Not hard, but this is easier for now at least.
#define delay(np, time) do { \
    np->delay_event = event_alloc(); \
    event_t *ep = &event[np->delay_event]; \
    ep->u.delay.ni = np->ni; \
    ep->notify = delay_notify; \
    event_post(np->delay_event, current_time + time); \
    while (event_pending(np->delay_event)) pt_wait(np, &np->delay_event); \
    event_free(np->delay_event); \
} while (false)

pt_t node_thr(env_t const env) {
    node_t *np = env;
    const int ni = np->ni;
    pt_resume(np);
    //np->is_miner = (randrange(10) == 0);
    // make 10 outbound connections
    int pi = 0;
    for (int i = 0; i < 10; i++) {
        // find an available local slot
        while (pi < NPEER && np->peer[pi].delay > 0) pi++;
        if (pi >= NPEER) break;

        // perfer nodes that are "close" to us
        int d, peer_mi, ppi;
        while (true) {
            d = 1 + randrange(1 << randrange(16));
            peer_mi = (ni + d) % nnode;

            // see if this peer is already in our peer list
            int j = 0;
            for (j = 0; j < NPEER; j++) {
                if (np->peer[j].delay > 0 && np->peer[j].ni == peer_mi) break;
            }
            if (j < NPEER) continue;

            // find an available peer slot
            for (ppi = 0; ppi < NPEER; ppi++) {
                if (node[peer_mi].peer[ppi].delay == 0) break;
            }
            if (ppi < NPEER) break;
        }
        np->peer[pi].ni = peer_mi;
        // one hop away is 1 ms
        np->peer[pi].delay = (double)d / 1000;
        // make it bidirectional
        node[peer_mi].peer[ppi].ni = ni;
        node[peer_mi].peer[ppi].delay = np->peer[pi].delay;
    }
    if (ni < 4) np->hashrate = 1.0;
    totalhash += np->hashrate;
    start_mining(ni, baseblockid);
    while (true) {
        double delay_time = 0.2;
        if(1) printf("thr %i time %f wakeat %f\n",
            ni, current_time, current_time+delay_time);
        if(0) delay(np, delay_time);
        // wait for a block to arrive
        while (np->qhead < 0) pt_wait(np, &np->qhead);
        int e = np->qhead;
        event_t *ep = &event[e];
        np->qhead = ep->next;
        u64 blockid = ep->u.new_block.blockid;
        if (ep->u.new_block.mining) {
            // We mined a block (unless this is a stale event).
            if (blockid != np->current) {
                // This is a stale mining event, ignore it (we should
                // still have an active mining event outstanding).
                continue;
            }
            np->mined++;
            stop_mining(ni);
            blockid = baseblockid + nblock;
            int bi = block_alloc();
            block_t *bp = &block[bi];
            bp->parent = np->current;
            bp->height = getheight(np->current) + 1;
            bp->miner = ni;
        } else {
            // Block received from a peer (but could be a stale message).
            if (!validblock(blockid)) {
                // We're already mining on a block that's at least as good.
                continue;
            }
            if (validblock(np->current) &&
                    getheight(blockid) <= getheight(np->current)) {
                // We're already mining on a block that's at least as good.
                continue;
            }
            // This block is better, switch to it, first compute reorg depth.
            printf("%.3f %i received-switch-to %llu\n",
                current_time, ni, blockid);
            if (np->hashrate > 0) {
                block_t *c = getblock(np->current);
                block_t *t = getblock(blockid); // to block (switching to)
                // Move back on the "to" (better) chain until even with current.
                while (t->height > c->height) {
                    t = getblock(t->parent);
                }
                // From the same height, count blocks until these branches meet.
                int reorg = 0;
                while (t != c) {
                    reorg++;
                    t = getblock(t->parent);
                    c = getblock(c->parent);
                }
                if (reorg > 0) {
                    printf("%.3f %i reorg %d maxreorg %d\n",
                        current_time, ni, reorg, maxreorg);
                }
                if (maxreorg < reorg) {
                    maxreorg = reorg;
                }
                stop_mining(ni);
            }
        }
        relay(ni, blockid);
        start_mining(ni, blockid);
    }
    return PT_DONE;
}

// Remove unneded blocks, give credits to miners.
void clean_blocks(void) {
    if (ntips > 1 || nblock <= 1) return;

    // Since all miners are building on the same tip, the blocks from
    // the tip to the base can't be reorged away, so we can remove
    // them, but give credit for these mined blocks as we do.
    u64 newbaseblockid = node[0].current;
    block_t *bp = getblock(newbaseblockid);
    while (bp != &block[0]) {
        node[bp->miner].credit++;
        bp = getblock(bp->parent);
    }
    // Clean up (prune) unneeded blocks.
    block_t b = *getblock(newbaseblockid);
    block = realloc(block, sizeof(block_t));
    block[0] = b;
    baseblockid = newbaseblockid;
}


int main(void) {
    {
        static char rngstate[256];
        initstate(0 /*time(0)*/, rngstate, sizeof(rngstate));
    }
    block_init();
    event_init();
    heap_init();
    node = calloc(nnode, sizeof(node_t));

    protothread_t pt = protothread_create();
    for (int ni = 0; ni < nnode; ni++) {
        node_t *n = &node[ni];
        n->qhead = -1;  // empty event list
        n->ni = ni;
        pt_create(pt, &n->pt_thread, node_thr, n);
    }
    for (int i = 0; i < 800*1000*1000; i++) {
        while (protothread_run(pt));
        if (!nheap) break;
        clean_blocks();
        int e = heap_pop();
        event_t *ep = &event[e];
        current_time = ep->time;
        ep->notify(pt, e); // should make a thread runnable
    }
    if(0) for (int ni = 0; ni < nnode; ni++) {
        printf("%d: ", ni);
        for (int j = 0; j < NPEER; j++) {
            if (node[ni].peer[j].delay == 0) continue;
            printf("[%d %f], ", node[ni].peer[j].ni, node[ni].peer[j].delay);
        }
        printf("\n");
    }

    return 0;
}
