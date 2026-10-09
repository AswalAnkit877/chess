/*
 cross-platform matchmaking.c
 Simplified queue: global sorted list (by rating, then arrival_seq).
 Keeps hashmap, pool, async logger, python callback.
*/

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <pthread.h>
    #include <unistd.h>
#endif

#ifndef EXPORT
#  ifdef _WIN32
#    define EXPORT __declspec(dllexport)
#  else
#    define EXPORT __attribute__((visibility("default")))
#  endif
#endif

/* -------------------- Config -------------------- */
#define UUID_LEN 64
#define DEFAULT_HASH_CAP 32768
#define POOL_CHUNK 65536
#define MAX_RATING 4000
#define MATCH_HISTORY_DEFAULT 262144
#define LOG_PATH "matches.log"

/* ---------------- Mutex abstraction ---------------- */
#ifdef _WIN32
    typedef CRITICAL_SECTION Mutex;
    typedef SRWLOCK RwLock;
    typedef CONDITION_VARIABLE CondVar;
    typedef HANDLE Thread;
    #define mutex_init(m) InitializeCriticalSection(m)
    #define mutex_lock(m) EnterCriticalSection(m)
    #define mutex_unlock(m) LeaveCriticalSection(m)
    #define mutex_destroy(m) DeleteCriticalSection(m)
    #define condvar_init(c) InitializeConditionVariable(c)
    #define condvar_wait(c,m) SleepConditionVariableCS(c,m,INFINITE)
    #define condvar_signal(c) WakeConditionVariable(c)
#else
    typedef pthread_mutex_t Mutex;
    typedef pthread_rwlock_t RwLock;
    typedef pthread_cond_t CondVar;
    typedef pthread_t Thread;
    #define mutex_init(m) pthread_mutex_init(m, NULL)
    #define mutex_lock(m) pthread_mutex_lock(m)
    #define mutex_unlock(m) pthread_mutex_unlock(m)
    #define mutex_destroy(m) pthread_mutex_destroy(m)
    #define condvar_init(c) pthread_cond_init(c, NULL)
    #define condvar_wait(c,m) pthread_cond_wait(c,m)
    #define condvar_signal(c) pthread_cond_signal(c)
#endif

/* -------------------- Types -------------------- */
typedef struct PlayerNode {
    char uuid[UUID_LEN];
    int rating;
    struct PlayerNode* prev;
    struct PlayerNode* next;
    struct PlayerNode* hashmap_next;
    uint64_t arrival_seq;
} PlayerNode;

typedef struct {
    char player1[UUID_LEN];
    char player2[UUID_LEN];
    int match_id;
} Match;

typedef struct {
    char p1[UUID_LEN];
    char p2[UUID_LEN];
    int match_id;
    int r1;
    int r2;
} MatchLog;

typedef struct LogNode {
    MatchLog log;
    struct LogNode* next;
} LogNode;

/* -------------------- Globals -------------------- */
/* Global sorted list head/tail (sorted by rating asc, tie by arrival_seq asc) */
static PlayerNode* list_head = NULL;
static PlayerNode* list_tail = NULL;
/* Lock protecting the global sorted list */
static Mutex list_lock;

static PlayerNode** hashmap = NULL;
static size_t hashmap_capacity = 0;
static size_t hashmap_count = 0;
static Mutex hashmap_lock;

typedef struct PoolChunk {
    PlayerNode* nodes;
    struct PoolChunk* next;
} PoolChunk;
static PoolChunk* pool_chunks = NULL;
static PlayerNode* free_list = NULL;

static uint64_t global_arrival_seq = 1;

static Match* match_history = NULL;
static size_t match_history_cap = MATCH_HISTORY_DEFAULT;
static size_t match_history_head = 0;
static uint64_t match_counter = 0;
static Mutex history_lock;

/* async logger */
static Mutex logger_lock;
static CondVar logger_cv;
static LogNode* logger_queue_head = NULL;
static LogNode* logger_queue_tail = NULL;
static Thread logger_thread = 0;
static volatile int logger_thread_running = 0;

/* config */
static volatile int engine_inited = 0;
static int max_rating_diff = 200;

/* Python callback (Cdecl) */
#ifdef _WIN32
    typedef void (__cdecl *match_callback_t)(const char*, const char*, int, int, int);
#else
    typedef void (*match_callback_t)(const char*, const char*, int, int, int);
#endif
static match_callback_t py_match_callback = NULL;

/* -------------------- Utils -------------------- */
static unsigned long fnv1a_hash(const char* s) {
    unsigned long h = 2166136261u;
    while (*s) { h ^= (unsigned char)(*s++); h *= 16777619u; }
    return h;
}
static int clamp_rating(int r) {
    if (r < 0) return 0;
    if (r > MAX_RATING*2) return MAX_RATING*2;
    return r;
}

/* -------------------- Memory pool -------------------- */
static PlayerNode* pool_alloc_nodes(size_t count) {
    PlayerNode* nodes = (PlayerNode*)malloc(sizeof(PlayerNode)*count);
    if (!nodes) return NULL;
    PoolChunk* pc = (PoolChunk*)malloc(sizeof(PoolChunk));
    if (!pc) { free(nodes); return NULL; }
    pc->nodes = nodes;
    pc->next = pool_chunks;
    pool_chunks = pc;
    for (size_t i=0;i<count;++i) {
        nodes[i].next = free_list;
        free_list = &nodes[i];
    }
    return nodes;
}
static PlayerNode* node_alloc() {
    if (!free_list) {
        if (!pool_alloc_nodes(POOL_CHUNK)) return NULL;
    }
    PlayerNode* n = free_list;
    free_list = free_list->next;
    memset(n,0,sizeof(PlayerNode));
    return n;
}
static void node_free(PlayerNode* n) {
    if (!n) return;
    n->next = free_list;
    free_list = n;
}

/* -------------------- Hashmap -------------------- */
static int hashmap_init(size_t capacity) {
    hashmap = (PlayerNode**)calloc(capacity, sizeof(PlayerNode*));
    if (!hashmap) return 0;
    hashmap_capacity = capacity;
    hashmap_count = 0;
    return 1;
}
static void hashmap_free_all() {
    if (hashmap) free(hashmap);
    hashmap = NULL;
    hashmap_capacity = 0;
    hashmap_count = 0;
}
static void hashmap_insert(PlayerNode* node) {
    if (!node) return;
    mutex_lock(&hashmap_lock);
    if (hashmap_capacity == 0) hashmap_init(DEFAULT_HASH_CAP);
    unsigned long h = fnv1a_hash(node->uuid);
    size_t idx = h % hashmap_capacity;
    node->hashmap_next = hashmap[idx];
    hashmap[idx] = node;
    hashmap_count++;
    mutex_unlock(&hashmap_lock);
}
static PlayerNode* hashmap_find(const char* uuid) {
    if (!hashmap) return NULL;
    unsigned long h = fnv1a_hash(uuid);
    size_t idx = h % hashmap_capacity;
    mutex_lock(&hashmap_lock);
    PlayerNode* p = hashmap[idx];
    while (p) {
        if (strcmp(p->uuid, uuid) == 0) { mutex_unlock(&hashmap_lock); return p; }
        p = p->hashmap_next;
    }
    mutex_unlock(&hashmap_lock);
    return NULL;
}
static void hashmap_remove(const char* uuid) {
    if (!hashmap) return;
    unsigned long h = fnv1a_hash(uuid);
    size_t idx = h % hashmap_capacity;
    mutex_lock(&hashmap_lock);
    PlayerNode* p = hashmap[idx];
    PlayerNode* prev = NULL;
    while (p) {
        if (strcmp(p->uuid, uuid) == 0) {
            if (prev) prev->hashmap_next = p->hashmap_next;
            else hashmap[idx] = p->hashmap_next;
            hashmap_count--;
            mutex_unlock(&hashmap_lock);
            return;
        }
        prev = p;
        p = p->hashmap_next;
    }
    mutex_unlock(&hashmap_lock);
}

/* -------------------- Match history -------------------- */
static void history_init(size_t cap) {
    match_history_cap = cap;
    match_history = (Match*)malloc(sizeof(Match)*cap);
    if (match_history) memset(match_history,0,sizeof(Match)*cap);
    match_history_head = 0;
    match_counter = 0;
    mutex_init(&history_lock);
}
static void history_push_with_id(const char* p1, const char* p2, int match_id) {
    mutex_lock(&history_lock);
    Match m;
    strncpy(m.player1,p1,UUID_LEN-1); m.player1[UUID_LEN-1]=0;
    strncpy(m.player2,p2,UUID_LEN-1); m.player2[UUID_LEN-1]=0;
    m.match_id = match_id;
    match_history[match_history_head] = m;
    match_history_head = (match_history_head + 1) % match_history_cap;
    mutex_unlock(&history_lock);
}

/* -------------------- Async logger -------------------- */
#ifdef _WIN32
static DWORD WINAPI logger_thread_func(LPVOID arg)
#else
static void* logger_thread_func(void* arg)
#endif
{
    (void)arg;
    while (logger_thread_running) {
        mutex_lock(&logger_lock);
        while (!logger_queue_head && logger_thread_running) {
            condvar_wait(&logger_cv, &logger_lock);
        }
        LogNode* head = logger_queue_head;
        logger_queue_head = NULL;
        logger_queue_tail = NULL;
        mutex_unlock(&logger_lock);

        if (!head) continue;
        FILE* f = fopen(LOG_PATH,"a");
        if (!f) {
            LogNode* n = head;
            while (n) { LogNode* nx = n->next; free(n); n = nx; }
            continue;
        }
        LogNode* cur = head;
        while (cur) {
            MatchLog* L = &cur->log;
            fprintf(f, "%d,%s,%d,%s,%d\n", L->match_id, L->p1, L->r1, L->p2, L->r2);
            if (py_match_callback) {
                py_match_callback(L->p1, L->p2, L->match_id, L->r1, L->r2);
            }
            LogNode* nx = cur->next;
            free(cur);
            cur = nx;
        }
        fflush(f);
        fclose(f);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void logger_enqueue(const char* p1, const char* p2, int match_id, int r1, int r2) {
    LogNode* n = (LogNode*)malloc(sizeof(LogNode));
    if (!n) return;
    memset(n,0,sizeof(LogNode));
    n->next = NULL;
    n->log.match_id = match_id;
    n->log.r1 = r1; n->log.r2 = r2;
    strncpy(n->log.p1, p1, UUID_LEN-1); n->log.p1[UUID_LEN-1]=0;
    strncpy(n->log.p2, p2, UUID_LEN-1); n->log.p2[UUID_LEN-1]=0;

    mutex_lock(&logger_lock);
    if (!logger_queue_head) { logger_queue_head = n; logger_queue_tail = n; }
    else { logger_queue_tail->next = n; logger_queue_tail = n; }
    mutex_unlock(&logger_lock);
    condvar_signal(&logger_cv);
}

/* -------------------- Sorted list helpers -------------------- */

/* Insert node into global sorted list (by rating asc, then arrival_seq asc).
   Caller must hold list_lock.
*/
static void list_insert_sorted(PlayerNode* node) {
    if (!node) return;
    if (!list_head) {
        list_head = list_tail = node;
        node->prev = node->next = NULL;
        return;
    }
    /* find insertion point */
    PlayerNode* cur = list_head;
    while (cur) {
        if (node->rating < cur->rating || (node->rating == cur->rating && node->arrival_seq < cur->arrival_seq)) {
            /* insert before cur */
            node->next = cur;
            node->prev = cur->prev;
            cur->prev = node;
            if (node->prev) node->prev->next = node;
            else list_head = node;
            return;
        }
        cur = cur->next;
    }
    /* append at tail */
    node->prev = list_tail;
    node->next = NULL;
    list_tail->next = node;
    list_tail = node;
}

/* Remove a node from global list. Caller must hold list_lock. */
static void list_remove_node(PlayerNode* n) {
    if (!n) return;
    if (n->prev) n->prev->next = n->next;
    else list_head = n->next;
    if (n->next) n->next->prev = n->prev;
    else list_tail = n->prev;
    n->prev = n->next = NULL;
}

/* Find best adjacent pair whose diff <= max_rating_diff.
   Returns 1 and fills p1/p2 when found (they are pointers into list).
   Caller must hold list_lock while calling (we will remove them while holding lock).
*/
static int list_find_best_pair(PlayerNode** out_p1, PlayerNode** out_p2) {
    if (!list_head || !list_head->next) return 0;
    PlayerNode* cur = list_head;
    while (cur && cur->next) {
        PlayerNode* a = cur;
        PlayerNode* b = cur->next;
        int diff = abs(a->rating - b->rating);
        if (diff <= max_rating_diff) {
            *out_p1 = a;
            *out_p2 = b;
            return 1;
        }
        cur = cur->next;
    }
    return 0;
}

/* -------------------- Engine core ops -------------------- */

/* internal add */
static void engine_add_player_internal(const char* uuid, int rating) {
    if (!uuid || !uuid[0]) return;
    int r = clamp_rating(rating);

    /* quick check if already exists */
    PlayerNode* existing = hashmap_find(uuid);
    if (existing) {
        /* update rating: remove from list and re-insert */
        mutex_lock(&list_lock);
        list_remove_node(existing);
        existing->rating = r;
#ifdef _WIN32
        existing->arrival_seq = (uint64_t)InterlockedIncrement64((volatile LONG64*)&global_arrival_seq);
#else
        existing->arrival_seq = __atomic_add_fetch(&global_arrival_seq, (uint64_t)1, __ATOMIC_SEQ_CST);
#endif
        list_insert_sorted(existing);
        mutex_unlock(&list_lock);
        return;
    }

    /* allocate node */
    PlayerNode* n = node_alloc();
    if (!n) return;
    strncpy(n->uuid, uuid, UUID_LEN-1); n->uuid[UUID_LEN-1]=0;
    n->rating = r;
#ifdef _WIN32
    n->arrival_seq = (uint64_t)InterlockedIncrement64((volatile LONG64*)&global_arrival_seq);
#else
    n->arrival_seq = __atomic_add_fetch(&global_arrival_seq, (uint64_t)1, __ATOMIC_SEQ_CST);
#endif
    n->prev = n->next = n->hashmap_next = NULL;

    /* insert into global sorted list */
    mutex_lock(&list_lock);
    list_insert_sorted(n);
    mutex_unlock(&list_lock);

    /* insert into hashmap */
    hashmap_insert(n);
}

/* exported wrappers */
EXPORT void init_engine() {
    if (engine_inited) return;
    /* init basics */
    mutex_init(&hashmap_lock);
    mutex_init(&logger_lock);
    mutex_init(&list_lock);
    condvar_init(&logger_cv);
    hashmap_init(DEFAULT_HASH_CAP);
    pool_alloc_nodes(POOL_CHUNK);
    history_init(MATCH_HISTORY_DEFAULT);

    /* start logger thread */
    logger_thread_running = 1;
#ifdef _WIN32
    logger_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)logger_thread_func, NULL, 0, NULL);
#else
    pthread_create(&logger_thread, NULL, logger_thread_func, NULL);
#endif

    engine_inited = 1;
}

EXPORT void set_max_rating_diff(int diff) {
    if (diff < 0) diff = 0;
    max_rating_diff = diff;
}

/* register python callback */
EXPORT void register_match_callback(match_callback_t cb) {
    py_match_callback = cb;
}

/* add player / bot */
EXPORT void add_player(const char* uuid, int rating) {
    if (!engine_inited) init_engine();
    engine_add_player_internal(uuid, rating);
}

EXPORT void add_bot(const char* uuid, int rating) {
    add_player(uuid, rating);
}

/* remove */
EXPORT void remove_player(const char* uuid) {
    if (!engine_inited || !uuid) return;
    PlayerNode* p = hashmap_find(uuid);
    if (!p) return;
    /* remove from list */
    mutex_lock(&list_lock);
    list_remove_node(p);
    mutex_unlock(&list_lock);
    /* remove from hashmap */
    hashmap_remove(uuid);
    /* free node */
    node_free(p);
}

/* get_match: returns 1 if match found (fills out), 0 otherwise
   Strategy:
     - lock list
     - find first adjacent pair (lowest rating diff) with diff <= max_rating_diff
     - remove them atomically and release lock
*/
EXPORT int get_match(Match* out) {
    if (!engine_inited || !out) return 0;

    mutex_lock(&list_lock);
    PlayerNode* p1 = NULL;
    PlayerNode* p2 = NULL;
    int found = list_find_best_pair(&p1, &p2);
    if (!found) {
        mutex_unlock(&list_lock);
        return 0;
    }

    /* remove them from list */
    list_remove_node(p1);
    list_remove_node(p2);
    mutex_unlock(&list_lock);

    /* remove from hashmap */
    hashmap_remove(p1->uuid);
    hashmap_remove(p2->uuid);

    /* allocate match id atomically */
#ifdef _WIN32
    uint64_t mid = (uint64_t)InterlockedIncrement64((volatile LONG64*)&match_counter);
#else
    uint64_t mid = __atomic_add_fetch(&match_counter, (uint64_t)1, __ATOMIC_SEQ_CST);
#endif

    /* fill out match */
    strncpy(out->player1, p1->uuid, UUID_LEN-1); out->player1[UUID_LEN-1]=0;
    strncpy(out->player2, p2->uuid, UUID_LEN-1); out->player2[UUID_LEN-1]=0;
    out->match_id = (int)mid;

    /* push history */
    history_push_with_id(out->player1, out->player2, (int)mid);

    /* logger enqueue */
    logger_enqueue(out->player1, out->player2, (int)mid, p1->rating, p2->rating);

    /* return nodes to pool */
    node_free(p1);
    node_free(p2);

    return 1;
}

/* get_all_matches: returns up to max_buf recent matches (newest first) */
EXPORT int get_all_matches(Match* buf, int max_buf) {
    if (!buf || max_buf <= 0) return 0;
    mutex_lock(&history_lock);
    int written = 0;
    if (!match_history) { mutex_unlock(&history_lock); return 0; }
    int idx = (int)((match_history_head + match_history_cap - 1) % match_history_cap);
    for (int i=0; i < (int)match_history_cap && written < max_buf; ++i) {
        Match m = match_history[idx];
        if (m.match_id == 0) break;
        buf[written++] = m;
        idx = (idx + match_history_cap - 1) % match_history_cap;
    }
    mutex_unlock(&history_lock);
    return written;
}

/* free_engine */
EXPORT void free_engine() {
    if (!engine_inited) return;
    logger_thread_running = 0;
    condvar_signal(&logger_cv);
#ifdef _WIN32
    if (logger_thread) {
        WaitForSingleObject(logger_thread, 2000);
        CloseHandle(logger_thread);
        logger_thread = NULL;
    }
#else
    if (logger_thread) {
        pthread_join(logger_thread, NULL);
    }
#endif

    /* clear list */
    mutex_lock(&list_lock);
    PlayerNode* cur = list_head;
    while (cur) {
        PlayerNode* nx = cur->next;
        /* do not free nodes that belong to pool; we free pool later */
        cur = nx;
    }
    list_head = list_tail = NULL;
    mutex_unlock(&list_lock);

    hashmap_free_all();
    if (match_history) { free(match_history); match_history = NULL; }

    mutex_destroy(&history_lock);
    mutex_destroy(&hashmap_lock);
    mutex_destroy(&logger_lock);
    mutex_destroy(&list_lock);

    /* destroy condvar if POSIX */
#ifndef _WIN32
    /* pthread cond destroy */
    /* pthread_cond_destroy(&logger_cv); done by condvar_destroy macro if needed */
#endif

    /* free memory pool */
    PoolChunk* pc = pool_chunks;
    while (pc) {
        PoolChunk* next = pc->next;
        if (pc->nodes) {
            free(pc->nodes);
        }
        free(pc);
        pc = next;
    }
    pool_chunks = NULL;
    free_list = NULL;

    engine_inited = 0;
}

#ifdef _WIN32
/* optional DllMain left as-is for windows builds */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        mutex_init(&hashmap_lock);
        mutex_init(&logger_lock);
        mutex_init(&list_lock);
        condvar_init(&logger_cv);
        break;
    case DLL_PROCESS_DETACH:
        free_engine();
        break;
    }
    return TRUE;
}
#endif
