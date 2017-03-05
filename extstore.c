/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

// FIXME: config.h?
#include <stdint.h>
#include <stdbool.h>
// end FIXME
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h> // FIXME: only when DEBUG compiled?
#include <string.h>
#include <assert.h>
#include "extstore.h"

/* TODO: manage the page refcount */

/* TODO: Most entries here should be configurable */
#define WBUF_COUNT 4
#define IO_THREADS 2
#define IO_DEPTH 1 /* only really matters for aio modes */

/* TODO: Embed obj_io for internal wbuf's. change extstore_read to
 * extstore_submit.
 */
typedef struct __store_wbuf {
    struct __store_wbuf *next;
    char *buf;
    char *buf_pos;
    unsigned int free;
    unsigned int size;
    unsigned int page_id; /* page owner of this write buffer */
    unsigned int offset; /* offset into page this write starts at */
} _store_wbuf;

typedef struct _store_page {
    pthread_mutex_t mutex; /* Need to be held for most operations */
    uint64_t cas;
    uint64_t obj_count;
    uint64_t offset; /* starting address of page within fd */
    unsigned int refcount;
    unsigned int id;
    unsigned int allocated;
    unsigned int written; /* item offsets can be past written if wbuf not flushed */
    int fd;
    bool active;
    _store_wbuf *wbuf;
    struct _store_page *next;
    uint64_t histo[61];
} store_page;

typedef struct store_engine store_engine;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_mutex_t queue_mutex;
    obj_io *queue;
    _store_wbuf *wbuf_queue;
    store_engine *e;
} store_io_thread;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    store_engine *e;
} store_maint_thread;

/* TODO: Array of FDs for JBOD support */
struct store_engine {
    pthread_mutex_t mutex; /* Need to hold to find active write page */
    pthread_mutex_t io_mutex; /* separate mutex for IO submissions */
    store_page *pages;
    _store_wbuf *wbuf_stack;
    store_io_thread *io_threads;
    store_maint_thread *maint_thread;
    store_page *page_stack;
    size_t page_size;
    unsigned int last_io_thread; /* round robin the IO threads */
    unsigned int page_count;
    unsigned int page_free; /* unallocated pages */
    unsigned int low_ttl_page;
    unsigned int high_ttl_page;
    unsigned int max_io_depth; /* for AIO engines */
};

static _store_wbuf *wbuf_new(size_t size) {
    _store_wbuf *b = calloc(1, sizeof(_store_wbuf));
    if (b == NULL)
        return NULL;
    b->buf = malloc(size);
    if (b->buf == NULL) {
        free(b);
        return NULL;
    }
    b->buf_pos = b->buf;
    b->free = size;
    b->size = size;
    return b;
}

static store_io_thread *_get_io_thread(store_engine *e) {
    int tid;
    pthread_mutex_lock(&e->mutex);
    tid = (e->last_io_thread + 1) % IO_THREADS;
    e->last_io_thread = tid;
    pthread_mutex_unlock(&e->mutex);

    return &e->io_threads[tid];
}

static void *extstore_io_thread(void *arg);
static void *extstore_maint_thread(void *arg);

/* TODO: engine init function; takes file, page size, total size arguments */
/* also directio/aio optional? thread pool size? options struct? ttl
 * threshold? */

/* Open file.
 * fill **pages
 * error if page size > 4g
 * (fallocate?)
 * allocate write buffers
 * set initial low/high pages
 * spawn threads
 * return void pointer to engine context.
 */

/* TODO: debug mode with prints? error code? */
void *extstore_init(char *fn, size_t pgsize, size_t pgcount, size_t wbufsize) {
    int i;
    int fd;
    uint64_t offset = 0;
    pthread_t thread;
    store_engine *e = calloc(1, sizeof(store_engine));
    if (e == NULL) {
        return NULL;
    }

    if (pgsize > UINT_MAX) {
        return NULL;
    }

    e->page_size = pgsize;
    fd = open(fn, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(e);
        return NULL;
    }

    e->pages = calloc(pgcount, sizeof(store_page));
    if (e->pages == NULL) {
        close(fd);
        free(e);
        return NULL;
    }

    for (i = 0; i < pgcount; i++) {
        e->pages[i].id = i;
        e->pages[i].fd = fd;
        e->pages[i].offset = offset;
        offset += pgsize;
    }

    for (i = pgcount; i > 1; i--) {
        e->pages[i].next = e->page_stack;
        e->page_stack = &e->pages[i];
        e->page_free++;
    }

    e->page_count = pgcount;
    /* TODO: Lazy allocate page categories */
    e->low_ttl_page = 0;
    e->pages[0].active = true;
    e->high_ttl_page = 1;
    e->pages[1].active = true;

    /* allocate write buffers */
    for (i = 0; i < WBUF_COUNT; i++) {
        _store_wbuf *w = wbuf_new(wbufsize);
        /* TODO: on error, loop again and free stack. */
        w->next = e->wbuf_stack;
        e->wbuf_stack = w;
    }

    pthread_mutex_init(&e->mutex, NULL);
    pthread_mutex_init(&e->io_mutex, NULL);

    /* spawn threads */
    e->io_threads = calloc(IO_THREADS, sizeof(store_io_thread));
    for (i = 0; i < IO_THREADS; i++) {
        pthread_mutex_init(&e->io_threads[i].mutex, NULL);
        pthread_mutex_init(&e->io_threads[i].queue_mutex, NULL);
        pthread_cond_init(&e->io_threads[i].cond, NULL);
        e->io_threads[i].e = e;
        // FIXME: error handling
        pthread_create(&thread, NULL, extstore_io_thread, &e->io_threads[i]);
    }

    e->maint_thread = calloc(1, sizeof(store_maint_thread));
    e->maint_thread->e = e;
    // FIXME: error handling
    pthread_create(&thread, NULL, extstore_maint_thread, &e->maint_thread);

    return (void *)e;
}

/* engine write function; takes engine, item_io.
 * fast fail if no available write buffer (flushing)
 * lock engine context, find active page, unlock
 * rotate buffers? active/passive
 * if full and rotated, submit page/buffer to io thread.
 * return status code
 */

int extstore_write(void *ptr, obj_io *io) {
    store_engine *e = (store_engine *)ptr;
    store_page *p;
    int ret = -1;

    /* This is probably a loop; we continue if the output page had to be
     * replaced
     */
    pthread_mutex_lock(&e->mutex);
    p = &e->pages[e->low_ttl_page];
    pthread_mutex_unlock(&e->mutex);
    /* FIXME: Is it safe to lock here? Need to double check the flag and loop
     * or lock from within e->mutex
     */

    pthread_mutex_lock(&p->mutex);
    if (!p->active) {
        pthread_mutex_unlock(&p->mutex);
        fprintf(stderr, "EXTSTORE: WRITE PAGE INACTIVE!\n");
        return -1;
    }

    /* memcpy into wbuf */
    if (p->wbuf && p->wbuf->free < io->len) {
        /* Submit to IO thread */
        /* FIXME: enqueue_io command, use an obj_io? */
        store_io_thread *t = _get_io_thread(e);
        pthread_mutex_lock(&t->queue_mutex);
        /* FIXME: Track tail and do FIFO instead of LIFO */
        p->wbuf->next = t->wbuf_queue;
        t->wbuf_queue = p->wbuf;
        pthread_mutex_unlock(&t->queue_mutex);
        pthread_cond_signal(&t->cond);
        p->wbuf = NULL;
        // Flushed buffer to now-full page, assign a new one.
        if (p->allocated >= e->page_size) {
            fprintf(stderr, "EXTSTORE: allocating new page\n");
            // TODO: Pages assigned inline or from bg thread?
            pthread_mutex_lock(&e->mutex);
            if (e->page_free > 0) {
                assert(e->page_stack != NULL);
                e->low_ttl_page = e->page_stack->id;
                e->page_stack->active = true;
                e->page_stack = e->page_stack->next;
                e->page_free--;
            }
            pthread_mutex_unlock(&e->mutex);
            // FIXME: This falls through and fails.
        }
    }

    // TODO: e->page_size safe for dirty reads? "cache" into page object?
    if (!p->wbuf) {
        if (p->allocated < e->page_size) {
            /* TODO: give the engine specific mutexes around things?
             * would have to ensure struct is padded to avoid false-sharing
             */
            pthread_mutex_lock(&e->mutex);
            if (e->wbuf_stack) {
                p->wbuf = e->wbuf_stack;
                e->wbuf_stack = p->wbuf->next;
                p->wbuf->next = 0;
            }
            pthread_mutex_unlock(&e->mutex);
            if (p->wbuf) {
                p->wbuf->page_id = p->id;

                p->wbuf->offset = p->allocated;
                p->allocated += p->wbuf->size;
                p->wbuf->free = p->wbuf->size;
                p->wbuf->buf_pos = p->wbuf->buf;
            }
        }
    }

    if (p->wbuf && p->wbuf->free > io->len) {
        memcpy(p->wbuf->buf_pos, io->buf, io->len);
        io->page_id = p->id;
        io->offset = p->wbuf->offset + (p->wbuf->size - p->wbuf->free);
        // TODO: The page CAS goes here.
        p->wbuf->buf_pos += io->len;
        p->wbuf->free -= io->len;
        p->obj_count++;
        ret = 0;
    }

    pthread_mutex_unlock(&p->mutex);
    /* p->written is incremented post-wbuf flush */
    return ret;
}

/* allocate new pages in here or another buffer? */

/* engine read function; takes engine, item_io stack.
 * lock io_thread context and add stack?
 * signal io thread to wake.
 * return sucess.
 */
int extstore_read(void *ptr, obj_io *io) {
    store_engine *e = (store_engine *)ptr;
    store_io_thread *t = _get_io_thread(e);

    pthread_mutex_lock(&t->queue_mutex);
    if (t->queue == NULL) {
        t->queue = io;
    } else {
        /* Have to put the *io stack at the end of current queue.
         * Optimize by tracking tail.
         */
        obj_io *tmp = t->queue;
        while (tmp->next != NULL)
            tmp = tmp->next;

        tmp->next = io;
    }
    pthread_mutex_unlock(&t->queue_mutex);

    //pthread_mutex_lock(&t->mutex);
    pthread_cond_signal(&t->cond);
    //pthread_mutex_unlock(&t->mutex);
    return 0;
}

/* engine note delete function: takes engine, page id, size?
 * note that an item in this page is no longer valid
 */

/* engine IO thread; takes engine context
 * manage writes/reads :P
 * run callback any necessary callback commands?
 */
static void *extstore_io_thread(void *arg) {
    store_io_thread *me = (store_io_thread *)arg;
    store_engine *e = me->e;
    pthread_mutex_lock(&me->mutex);
    while (1) {
        obj_io *io_stack = NULL;
        _store_wbuf *wbuf_stack = NULL;
        // TODO: lock/check queue before going into wait
        pthread_cond_wait(&me->cond, &me->mutex);

        pthread_mutex_lock(&me->queue_mutex);
        if (me->wbuf_queue != NULL) {
            int i;
            _store_wbuf *end = NULL;
            wbuf_stack = me->wbuf_queue;
            end = wbuf_stack;
            /* Pull and disconnect a batch from the queue */
            for (i = 1; i < IO_DEPTH; i++) {
                if (end->next) {
                    end = end->next;
                } else {
                    break;
                }
            }
            me->wbuf_queue = end->next;
            end->next = NULL;
        } else if (me->queue != NULL) {
            int i;
            obj_io *end = NULL;
            io_stack = me->queue;
            end = io_stack;
            /* Pull and disconnect a batch from the queue */
            for (i = 1; i < IO_DEPTH; i++) {
                if (end->next) {
                    end = end->next;
                } else {
                    break;
                }
            }
            me->queue = end->next;
            end->next = NULL;
        }
        pthread_mutex_unlock(&me->queue_mutex);

        /* TODO: Direct IO + libaio mode */
        _store_wbuf *cur_wbuf = wbuf_stack;
        while (cur_wbuf) {
            int ret;
            store_page *p = &e->pages[cur_wbuf->page_id];
            ret = pwrite(p->fd, cur_wbuf->buf, cur_wbuf->size - cur_wbuf->free,
                    p->offset + cur_wbuf->offset);
            // FIXME: Remove.
            if (ret < 0) {
                perror("wbuf write failed");
            }
            cur_wbuf = cur_wbuf->next;
        }
        /* TODO: Lock engine and return stack */
        if (wbuf_stack) {
            _store_wbuf *tmp = wbuf_stack;
            while (tmp->next)
                tmp = tmp->next;
            pthread_mutex_lock(&e->mutex);
            tmp->next = e->wbuf_stack;
            e->wbuf_stack = wbuf_stack;
            pthread_mutex_unlock(&e->mutex);
        }
        obj_io *cur_io = io_stack;
        while (cur_io) {
            int ret = 0;
            store_page *p = &e->pages[cur_io->page_id];
            /* TODO: lock page. validate CAS, increment refcount. */
            /* TODO: if offset is beyond p->written, memcpy back */
            switch (cur_io->mode) {
                case OBJ_IO_READ:
                    ret = pread(p->fd, cur_io->buf, cur_io->len, cur_io->offset);
                    break;
                case OBJ_IO_WRITE:
                    ret = pwrite(p->fd, cur_io->buf, cur_io->len, cur_io->offset);
                    break;
            }
            // FIXME: Remove.
            if (ret < 0) {
                perror("wbuf write failed");
            }
            cur_io->cb(e, cur_io, ret);
            cur_io = cur_io->next;
        }

        // At the end of the loop the extracted obj_io's should be forgotten.
        // Whatever's in CB should handle memory management of the IO.
        // FIXME: Can we be strict about "if the callback is called, the
        // obj_io is no longer touched here" ?
    }

    return NULL;
}

/* engine maint thread; takes engine context
 * if write flips buffer, or if a new page is allocated for use, signal engine
 * maint thread.
 * maint thread sorts pages by estimated freeness, marks inactive best
 * candidate and waits for refcount to hit 0.
 * adds any freed page areas to free pool.
 * stats?
 */

static void *extstore_maint_thread(void *arg) {
    store_maint_thread *me = (store_maint_thread *)arg;
    store_engine *e = me->e;
    pthread_mutex_lock(&me->mutex);
    while (1) {
        pthread_cond_wait(&me->cond, &me->mutex);
        pthread_mutex_lock(&e->mutex);
        pthread_mutex_unlock(&e->mutex);
    }

    return NULL;
}
