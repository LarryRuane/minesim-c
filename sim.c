#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "protothread.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

void fail(char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

u32 randrange(u32 i) {
    return random() % i;
}

double current_time;

typedef struct block_s {
    u64 parent; // first block is the only block with parent = zero
    u64 height; // more than one block can have the same height
    u32 miner;  // which miner found this block
    u32 active; // number of miners actively mining directly on this block
} block_t;

block_t *block;     // blockchain, oldest first
u32 block_nalloc;   // number of allocated blocks
u32 nblock;         // len(block)
u64 baseblockid;    // blocks[0] corresponds to this block id
u32 ntips;          // number of blocks being actively mined on
u32 maxreorg;       // greatest depth reorg
double totalhash;   // sum of miners' hashrates

void block_init(void) {
    block_nalloc = 1;
    block = calloc(block_nalloc, sizeof(block_t));
    block[0] = (block_t) { 0, 0, 0, 0 };
    nblock = 1;
    baseblockid = 1000; // arbitrary but helps distinguish ids from heights
    ntips = 0;
}

// allocate one new block, return its index.
u32 block_alloc(void) {
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
    void (*notify)(protothread_t, u32);
    u32 next;           // for freelist or node input queue
    union {
        struct {
            u32 ni;             // node index
        } delay;
        struct {
            u32 ni;             // index of receiving node
            bool mining;        // block arrival from mining or peer
            u64 blockid;        // parent of new block, or block from peer
        } new_block;
    } u;
} event_t;

// unordered
u32 event_nalloc;       // event[0..event_nalloc-1]
event_t *event;
u32 free_events;        // head of list of free event

void event_init(void) {
    event_nalloc = 1;
    event = calloc(1, sizeof(event_t));
    event[0].next = 1;
    free_events = 0;
}

bool event_pending(u32 e) {
    return event[e].time > current_time;
}

// time-ordered priority queue, entries are indices into events[]
u32 *heap;  // heap[0..event_nalloc-1]
u32 nheap;  // number of valid items currently in the heap

void heap_init(void) {
    heap = calloc(1, sizeof(u32));
    nheap = 0;
}

// append to the end of the array, then "bubble" it upwards
void heap_add(u32 n) {
    assert(nheap < event_nalloc);
    u32 i = nheap++;
    while (i) {
        u32 parent = (i-1)/2;
        if (event[heap[parent]].time < event[n].time) {
            break;
        }
        heap[i] = heap[parent];
        i = parent;
    }
    heap[i] = n;
}

u32 heap_pop(void) {
    u32 const r = heap[0];
    if (--nheap == 0) {
        return r;
    }
    // logically we're first moving this last value to a[0]
    u32 p = heap[nheap];
    u32 i = 0;
    while (true) {
        u32 lchild = (i*2)+1;
        if (lchild >= nheap) {
            break;
        }
        u32 rchild = lchild+1;
        u32 next_i;
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

u32 event_alloc(void) {
    if (free_events == event_nalloc) {
        u32 new_nalloc = event_nalloc * 2;
        event = realloc(event, new_nalloc*sizeof(event_t));
        if (!event) fail("out of memory!");
        heap = realloc(heap, new_nalloc*sizeof(u32));
        if (!heap) fail("out of memory!");
        for (u32 i = event_nalloc; i < new_nalloc; i++) {
            memset(&event[i], 0, sizeof(event_t));
            event[i].next = i+1;
        }
        event_nalloc = new_nalloc;
    }
    u32 r = free_events;
    free_events = event[free_events].next;
    return r;
}

void event_post(u32 e, double time) {
    event[e].time = time;
    if (time > current_time) heap_add(e);
}

void event_free(u32 i) {
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
    u32 ni;
    double delay;
} peer_t;

#define NPEER 100

typedef struct node_s {
    pt_thread_t pt_thread;
    pt_func_t pt_func;
    u32 ni;             // my node index
    u32 qhead;          // event input message queue, -1 means empty
    u32 delay_event;    // event index
    u64 tip;            // blockid of best block *we* know about
    double hashrate;
    u64 mined;          // how many total blocks we've mined (including reorg)
    u64 credit;         // how many best-chain blocks we've mined
    peer_t peer[NPEER]; // maybe make this variable-length?
} node_t;

#define QHEAD_EMPTY 0xffffffff

// later there will be a dynamic set of nodes
u32 node_shift; // for now only 8 nodes
u32 nnode;      // must be even, 32k for now
node_t *node;
u32 nminer;
u32 *miner;

void node_init(void) {
    node_shift = 15; // for now 32k nodes
    nnode = 1 << node_shift;
    node = calloc(nnode, sizeof(node_t));
    miner = calloc(nnode, sizeof(u32));
}

// Relay a newly-discovered block (either we mined or relayed to us).
// This sends a message to the peer we received the block from (if it's one
// of our peers), but that's okay, it will be ignored.
void relay_notify(protothread_t pt, u32 e) {
    event_t *ep = &event[e];
    node_t *np = &node[ep->u.new_block.ni];

    // link to list of incoming block notify messages
    ep->next = np->qhead;
    np->qhead = e;

    pt_signal(pt, &np->qhead);
}

void relay(u32 ni) {
    node_t *np = &node[ni];
    for (u32 pi = 0; pi < NPEER; pi++) {
        peer_t *pp = &np->peer[pi];
        if (pp->delay == 0) continue;
        // Improve simulator efficiency by not relaying blocks
        // that are certain to be ignored.
        node_t *ppn = &node[pp->ni];
        if (!validblock(ppn->tip) ||
                getheight(ppn->tip) < getheight(np->tip)) {
            u32 e = event_alloc();
            event_t *ep = &event[e];
            ep->u.new_block.ni = ppn->ni;
            ep->u.new_block.mining = false;
            ep->u.new_block.blockid = np->tip;
            ep->notify = relay_notify;
            // TODO jitter this delay, or sometimes fail to forward?
            event_post(e, current_time + pp->delay);
        }
    }
}

// Start mining on top of the given existing block
void start_mining(node_t *np) {
    block_t *bp = getblock(np->tip);
    if (bp->active++ == 0) ntips++;

    // Schedule an event for when our "mining" will be done.
    double solvetime = poisson(300 * totalhash / np->hashrate);

    u32 e = event_alloc();
    event_t *ep = &event[e];
    ep->u.new_block.ni = np->ni;
    ep->u.new_block.mining = true;
    ep->u.new_block.blockid = np->tip;
    ep->notify = relay_notify;
    // TODO jitter this delay, or sometimes fail to forward?
    event_post(e, current_time + solvetime);
    if(0) printf("%.3f %03d start-on %llu height %llu "
            "mined %lld credit %lld solve %.2f\n",
        current_time, np->ni, np->tip, getheight(np->tip),
        np->mined, np->credit, solvetime);
}

void stop_mining(node_t *np) {
    block_t *bp = getblock(np->tip);
    if (--bp->active == 0) ntips--;
}

void delay_notify(protothread_t pt, u32 e) {
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
    node_t * const np = env;
    u32 const ni = np->ni;
    pt_resume(np);
    //np->is_miner = (randrange(10) == 0);
    // make 10 outbound connections
    u32 pi = 0;
    for (u32 i = 0; i < 2; i++) {
        // find an available local slot
        while (pi < NPEER && np->peer[pi].delay > 0) pi++;
        if (pi >= NPEER) break;

        // perfer nodes that are "close" to us
        u32 d, peer_mi, ppi;
        while (true) {
            d = 1 + randrange(1 << randrange(node_shift + 1));
            peer_mi = (ni + d) % nnode;

            // see if this peer is already in our peer list
            u32 j = 0;
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
        // one hop away is 100 ms
        np->peer[pi].delay = (double)d * 100 / 1000;
        // make it bidirectional
        node[peer_mi].peer[ppi].ni = ni;
        node[peer_mi].peer[ppi].delay = np->peer[pi].delay;
    }
    totalhash += np->hashrate;
    np->tip = baseblockid;
    if (np->hashrate > 0) start_mining(np);
    while (true) {
        double delay_time = ni*20;
        if(0) printf("thr %i time %f wakeat %f\n",
            ni, current_time, current_time+delay_time);
        if(0) delay(np, delay_time);
        // wait for a block to arrive
        while (np->qhead == QHEAD_EMPTY) pt_wait(np, &np->qhead);
        u32 const ei = np->qhead;
        event_t *ep = &event[np->qhead];
        np->qhead = ep->next;
        u64 blockid = ep->u.new_block.blockid;
        bool mining = ep->u.new_block.mining;
        event_free(ei);
        if (mining) {
            assert(np->hashrate > 0);
            // We mined a block (unless this is a stale event).
            if (blockid != np->tip) {
                // This is a stale mining event, ignore it (we should
                // still have an active mining event outstanding).
                continue;
            }
            np->mined++;
            stop_mining(np);
            blockid = baseblockid + nblock;
            u32 bi = block_alloc();
            block_t *bp = &block[bi];
            bp->parent = np->tip;
            bp->height = getheight(np->tip) + 1;
            bp->miner = ni;
        } else {
            // Block received from a peer (but could be a stale message).
            if (!validblock(blockid)) {
                // We're already mining on a block that's at least as good.
                continue;
            }
            if (validblock(np->tip) &&
                    getheight(blockid) <= getheight(np->tip)) {
                // We're already mining on a block that's at least as good.
                continue;
            }
            // This block is better, switch to it, first compute reorg depth.
            if(np->hashrate > 0) if(0) printf("%.3f %i received-switch-to %llu\n",
                current_time, ni, blockid);
            if (np->hashrate > 0) stop_mining(np);

            // update reorg statistics
            if (np->hashrate > 0) {
                block_t *c = getblock(np->tip);
                block_t *t = getblock(blockid); // to block (switching to)
                // Move back on the "to" (better) chain until even with tip.
                while (t->height > c->height) {
                    t = getblock(t->parent);
                }
                // From the same height, count blocks until these branches meet.
                u32 reorg = 0;
                while (t != c) {
                    reorg++;
                    t = getblock(t->parent);
                    c = getblock(c->parent);
                }
                if (reorg > 0) {
                    if(0) printf("%.3f %i reorg %d maxreorg %d\n",
                        current_time, ni, reorg, maxreorg);
                }
                if (maxreorg < reorg) {
                    maxreorg = reorg;
                }
            }
        }
        np->tip = blockid;
        relay(ni);
        if (np->hashrate > 0) start_mining(np);
    }
    return PT_DONE;
}

// Remove unneded blocks, give credits to miners.
void clean_blocks(void) {
    u64 minheight = 0;
    for (u32 i = 0; i < nminer; i++) {
        u64 h = getheight(node[miner[i]].tip);
        if (i == 0 || minheight > h) minheight = h;
    }

    // move down all tips until they're at the same (minimum) height
    u64 *tip = calloc(nminer, sizeof(u64));
    for (u32 i = 0; i < nminer; i++) {
        node_t *np = &node[miner[i]];
        tip[i] = np->tip;
        while (getheight(tip[i]) > minheight) {
            tip[i] = getblock(tip[i])->parent;
        }
    }
    // Find the block that all tips are based on (oldest branch point).
    while (true) {
        u32 i;
        for (i = 1; i < nminer; i++) {
            if (tip[i] != tip[0]) break;
        }
        if (i >= nminer) break;
        for (i = 0; i < nminer; i++) {
            tip[i] = getblock(tip[i])->parent;
        }
    }
    u64 newbaseblockid = tip[0];

    // Give credits to miners (these blocks can't be reorged away).
    block_t *bp = getblock(newbaseblockid);
    while (bp != &block[0]) {
        node[bp->miner].credit++;
        bp = getblock(bp->parent);
    }

    // Remove older blocks that are no longer relevant.
    nblock -= (newbaseblockid - baseblockid);
    block_nalloc = nblock;
    while (block_nalloc & (block_nalloc-1)) block_nalloc++;
    block_t *newblock = calloc(block_nalloc, sizeof(block_t));
    for (u32 i = 0; i < nblock; i++) {
        newblock[i] = block[i + newbaseblockid - baseblockid];
    }
    free(block);
    block = newblock;
    baseblockid = newbaseblockid;
    free(tip);
}


int main(void) {
    {
        static char rngstate[256];
        initstate(0 /*time(0)*/, rngstate, sizeof(rngstate));
    }
    block_init();
    event_init();
    heap_init();
    node_init();

    protothread_t pt = protothread_create();
    for (u32 ni = 0; ni < nnode; ni++) {
        node_t *np = &node[ni];
        np->qhead = QHEAD_EMPTY;
        np->ni = ni;
        if (ni == 0 || !randrange(3000)) {
            // let's make this node a miner (must have at least one)
            np->hashrate = 1.0; // should be variable
            miner[nminer++] = ni;
        }
        pt_create(pt, &np->pt_thread, node_thr, np);
    }
    miner = realloc(miner, nminer * sizeof(u32));
    for (u32 i = 0; i < 80*1000*1000; i++) {
        while (protothread_run(pt));
        if (!nheap) break;
        if (nblock > 1000) clean_blocks();
        u32 e = heap_pop();
        event_t *ep = &event[e];
        current_time = ep->time;
        ep->notify(pt, e); // should make a thread runnable
    }
    clean_blocks();
    if(0) for (u32 ni = 0; ni < nnode; ni++) {
        printf("%d: ", ni);
        for (u32 j = 0; j < NPEER; j++) {
            if (node[ni].peer[j].delay == 0) continue;
            printf("[%d %f], ", node[ni].peer[j].ni, node[ni].peer[j].delay);
        }
        printf("\n");
    }

    return 0;
}
