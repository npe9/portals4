#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#if defined(HAVE_GETTIME_TIMER)
# define _POSIX_C_SOURCE 199309L
#endif

/* The API definition */
#include <portals4.h>

/* System headers */
#include <stdlib.h>                    /* for calloc() */
#include <string.h>                    /* for memset() */
#if defined(HAVE_MALLOC_H)
# include <malloc.h>                   /* for memalign() */
#endif

/* Internals */
#include "ptl_visibility.h"
#include "ptl_internal_ints.h"
#include "ptl_internal_assert.h"
#include "ptl_internal_commpad.h"
#include "ptl_internal_atomic.h"
#include "ptl_internal_nit.h"
#include "ptl_internal_handles.h"
#include "ptl_internal_CT.h"
#include "ptl_internal_trigger.h"
#include "ptl_internal_alignment.h"
#include "ptl_internal_orderednemesis.h"
#ifndef NO_ARG_VALIDATION
# include "ptl_internal_error.h"
#endif
#include "ptl_internal_timer.h"
/* for command packets */
#include "ptl_internal_locks.h"
#include "ptl_internal_fragments.h"

#define CT_FREE    0
#define CT_BUSY    1
#define CT_READY   2
#define CT_ERR_VAL 0xffffffffffffffffULL

static ptl_ct_event_t *restrict         ct_events[4] = { NULL, NULL, NULL, NULL };
static volatile uint_fast64_t *restrict ct_event_refcounts[4] = { NULL, NULL, NULL, NULL };

/* ct_event_triggers is the triggers for a given CT. The ct_triggers_alloc is
 * the allocation (per NI) of trigger structures, but ct_triggers is the
 * interface for the pool of them (i.e. ct_triggers_alloc allows easy freeing)
 */
static ordered_NEMESIS_queue *restrict ct_event_triggers[4] = { NULL, NULL, NULL, NULL };
static ptl_internal_trigger_t         *ct_triggers_alloc[4] = { NULL, NULL, NULL, NULL };
static ptl_internal_trigger_t         *ct_triggers[4] = { NULL, NULL, NULL, NULL };
static const ptl_ct_event_t            CTERR = { CT_ERR_VAL, CT_ERR_VAL };

#define CT_NOT_EQUAL(a, b) (a.success != b.success || a.failure != b.failure)
#define CT_EQUAL(a, b)     (a.success == b.success && a.failure == b.failure)

#if 0
/* 128-bit Atomics */
static inline int PtlInternalAtomicCasCT(volatile ptl_ct_event_t *addr,
                                         const ptl_ct_event_t     oldval,
                                         const ptl_ct_event_t     newval)
{                                      /*{{{ */
# ifdef HAVE_CMPXCHG16B
    register unsigned char ret;
    assert(((uintptr_t)addr & 0xf) == 0);
    __asm__ __volatile__ ("lock cmpxchg16b %1\n\t"
                          "sete           %0"
                          : "=q" (ret),
                          "+m"    (*addr)
                          : "a"    (oldval.success),
                          "d"     (oldval.failure),
                          "b"     (newval.success),
                          "c"     (newval.failure)
                          : "cc",
                          "memory");
    return ret;

# else /* ifdef HAVE_CMPXCHG16B */
#  error No known 128-bit atomic CAS operations are available
# endif /* ifdef HAVE_CMPXCHG16B */
}                                      /*}}} */

static inline void PtlInternalAtomicWriteCT(volatile ptl_ct_event_t *addr,
                                            const ptl_ct_event_t     newval)
{                                      /*{{{ */
# ifdef HAVE_CMPXCHG16B
    __asm__ __volatile__ ("1:\n\t"
                          "lock cmpxchg16b %0\n\t"
                          "jne 1b"
                          : "+m" (*addr)
                          : "a"    (addr->success),
                          "d"     (addr->failure),
                          "b"     (newval.success),
                          "c"     (newval.failure)
                          : "cc",
                          "memory");
# else /* ifdef HAVE_CMPXCHG16B */
#  error No known 128-bit atomic write operations are available
# endif /* ifdef HAVE_CMPXCHG16B */
}                                      /*}}} */

#endif /* if 0 */

ptl_internal_trigger_t INTERNAL *PtlInternalFetchTrigger(const uint_fast8_t ni)
{   /*{{{*/
    ptl_internal_trigger_t          *tmp;
    volatile ptl_internal_trigger_t *old, *new;

    tmp = ct_triggers[ni];
    do {
        old = tmp;
        if (tmp == NULL) {
            return NULL;
        }
        new = tmp->next;
    } while ((tmp = PtlInternalAtomicCasPtr(&ct_triggers[ni], old, new)) != old);
    return tmp;
} /*}}}*/

static void PtlInternalCTFreeTrigger(ptl_internal_trigger_t *freeme,
                                     const uint_fast8_t      ni)
{
    volatile ptl_internal_trigger_t *tmpv, *oldv, *newv;

    tmpv = ct_triggers[ni];
    do {
        freeme->next = tmpv;
        oldv         = tmpv;
        newv         = freeme;
        tmpv         = PtlInternalAtomicCasPtr(&ct_triggers[ni], oldv, newv);
    } while (tmpv != oldv);
}

void INTERNAL PtlInternalAddTrigger(ptl_handle_ct_t         ct_handle,
                                    ptl_internal_trigger_t *t)
{   /*{{{*/
    const ptl_internal_handle_converter_t ct = { ct_handle };

    t->next = NULL;
    if (!PtlInternalOrderedNEMESISEnqueue(&ct_event_triggers[ct.s.ni][ct.s.code], t, t->threshold)) {
        ptl_internal_header_t *restrict hdr;
        /* else send control message to self (serial insert) */
        /* step 1: get a local memory fragment */
        hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t) + sizeof(PTL_CMD_LOCK_TYPE));
        /* step 2: fill the op structure */
        hdr->type     = HDR_TYPE_CMD;
        hdr->ni       = ct.s.ni;
        hdr->src      = proc_number;
        hdr->target   = proc_number;
        hdr->pt_index = CMD_TYPE_ENQUEUE;
        hdr->hdr_data = ct_handle;
        hdr->user_ptr = t;

        /* step 3: load up data... */
        PTL_CMD_LOCK_SENDER1(hdr->data);

        /* step 4: enqueue the op structure on the target */
        PtlInternalFragmentToss(hdr, proc_number);
        PTL_CMD_LOCK_SENDER2(hdr->data);
    } else {
        ptl_internal_header_t *restrict hdr;
        /* step 1: get a local memory fragment */
        hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t));
        /* step 2: fill the op structure */
        hdr->type     = HDR_TYPE_CMD;
        hdr->ni       = ct.s.ni;
        hdr->src      = proc_number;
        hdr->target   = proc_number;
        hdr->pt_index = CMD_TYPE_CHECK;
        hdr->hdr_data = ct_handle;

        /* step 3: enqueue the op structure on the target */
        PtlInternalFragmentToss(hdr, proc_number);
    }
} /*}}}*/

void INTERNAL PtlInternalCTNISetup(const uint_fast8_t ni,
                                   const ptl_size_t   limit)
{                                      /*{{{ */
    ptl_ct_event_t *tmp;

    while ((tmp = PtlInternalAtomicCasPtr((void *volatile *)&(ct_events[ni]),
                                          NULL,
                                          (void *)1)) == (void *)1) {
        SPINLOCK_BODY();
    }
    if (tmp == NULL) {
        ALIGNED_CALLOC(tmp, 16, limit, sizeof(ptl_ct_event_t));
        assert((((intptr_t)tmp) & 0x7) == 0);
        assert(ct_event_refcounts[ni] == NULL);
        ct_event_refcounts[ni] = calloc(limit, sizeof(uint_fast64_t));
        assert(ct_event_refcounts[ni] != NULL);
        assert(ct_event_triggers[ni] == NULL);
        assert(ct_triggers[ni] == NULL);
        assert(ct_triggers_alloc[ni] == NULL);
        ct_event_triggers[ni] = calloc(nit_limits[ni].max_cts, sizeof(ordered_NEMESIS_queue));
        assert(ct_event_triggers[ni] != NULL);
        for (size_t i = 0; i < nit_limits[ni].max_cts; ++i) {
            PtlInternalOrderedNEMESISInit(&ct_event_triggers[ni][i]);
        }
        if (nit_limits[ni].max_triggered_ops > 0) {
            ct_triggers_alloc[ni] = calloc(nit_limits[ni].max_triggered_ops,
                                           sizeof(ptl_internal_trigger_t));
            assert(ct_triggers_alloc[ni] != NULL);
            ct_triggers[ni] = ct_triggers_alloc[ni];
            for (size_t t = 0; t < nit_limits[ni].max_triggered_ops - 1; ++t) {
                ct_triggers[ni][t].next = &ct_triggers[ni][t + 1];
            }
        }
        __sync_synchronize();
        ct_events[ni] = tmp;
    }
}                                      /*}}} */

void INTERNAL PtlInternalCTNITeardown(const uint_fast8_t ni)
{                                      /*{{{ */
    ptl_ct_event_t *restrict         tmp;
    volatile uint_fast64_t *restrict rc;
    ptl_internal_trigger_t *restrict ctt;

    while (ct_events[ni] == (void *)1) SPINLOCK_BODY();        // in case its in the middle of being allocated (this should never happen in sane code)
    tmp = PtlInternalAtomicSwapPtr((void *volatile *)&ct_events[ni], NULL);
    rc  = PtlInternalAtomicSwapPtr((void *volatile *)&ct_event_refcounts[ni], NULL);
    assert(tmp != NULL);
    assert(tmp != (void *)1);
    assert(rc != NULL);
    free(PtlInternalAtomicSwapPtr((void *volatile *)&ct_event_triggers[ni], NULL));
    if (nit_limits[ni].max_triggered_ops > 0) {
        PtlInternalAtomicSwapPtr((void *volatile *)&ct_triggers[ni], NULL);
        ctt = PtlInternalAtomicSwapPtr((void *volatile *)&ct_triggers_alloc[ni], NULL);
        assert(ctt != NULL);
        free(ctt);
    }

    for (size_t i = 0; i < nit_limits[ni].max_cts; ++i) {
        if (rc[i] != 0) {
            tmp[i] = CTERR;
            __sync_synchronize();
            PtlInternalAtomicInc(&(rc[i]), -1);
        }
    }
    for (size_t i = 0; i < nit_limits[ni].max_cts; ++i) {
        while (rc[i] != 0) SPINLOCK_BODY();
    }
    ALIGNED_FREE(tmp, 16);
    free((void *)rc);
}                                      /*}}} */

int INTERNAL PtlInternalCTHandleValidator(ptl_handle_ct_t handle,
                                          uint_fast8_t    none_ok)
{                                      /*{{{ */
#ifndef NO_ARG_VALIDATION
    const ptl_internal_handle_converter_t ct = { handle };
    if (ct.s.selector != HANDLE_CT_CODE) {
        VERBOSE_ERROR("Expected CT handle, but it's not a CT handle\n");
        return PTL_ARG_INVALID;
    }
    if (handle == PTL_CT_NONE) {
        if (none_ok == 1) {
            return PTL_OK;
        } else {
            VERBOSE_ERROR("PTL_CT_NONE not allowed here\n");
            return PTL_ARG_INVALID;
        }
    }
    if ((ct.s.ni > 3) || (ct.s.code > nit_limits[ct.s.ni].max_cts) ||
        (nit.refcount[ct.s.ni] == 0)) {
        VERBOSE_ERROR("CT NI too large (%u > 3) or code is wrong (%u > %u) or nit table is uninitialized\n",
                      ct.s.ni, ct.s.code, nit_limits[ct.s.ni].max_cts);
        return PTL_ARG_INVALID;
    }
    if (ct_events[ct.s.ni] == NULL) {
        VERBOSE_ERROR("CT table for NI uninitialized\n");
        return PTL_ARG_INVALID;
    }
    if (ct_event_refcounts[ct.s.ni][ct.s.code] == 0) {
        VERBOSE_ERROR("CT appears to be deallocated\n");
        return PTL_ARG_INVALID;
    }
#endif /* ifndef NO_ARG_VALIDATION */
    return PTL_OK;
}                                      /*}}} */

int API_FUNC PtlCTAlloc(ptl_handle_ni_t  ni_handle,
                        ptl_handle_ct_t *ct_handle)
{                                      /*{{{ */
    ptl_ct_event_t                       *cts;
    ptl_size_t                            offset;
    volatile uint_fast64_t               *rc;
    const ptl_internal_handle_converter_t ni = { ni_handle };
    ptl_internal_handle_converter_t       ct = { .s.selector = HANDLE_CT_CODE };

#ifndef NO_ARG_VALIDATION
    if (PtlInternalLibraryInitialized() == PTL_FAIL) {
        VERBOSE_ERROR("communication pad not initialized\n");
        return PTL_NO_INIT;
    }
    if (PtlInternalNIValidator(ni)) {
        VERBOSE_ERROR("ni code wrong\n");
        return PTL_ARG_INVALID;
    }
    if (ct_events[ni.s.ni] == NULL) {
        assert(ct_events[ni.s.ni] != NULL);
        return PTL_ARG_INVALID;
    }
    if (ct_handle == NULL) {
        VERBOSE_ERROR("passed in a NULL for ct_handle\n");
        return PTL_ARG_INVALID;
    }
#endif /* ifndef NO_ARG_VALIDATION */
    ct.s.ni = ni.s.ni;
    cts     = ct_events[ni.s.ni];
    rc      = ct_event_refcounts[ni.s.ni];
    for (offset = 0; offset < nit_limits[ct.s.ni].max_cts; ++offset) {
        if (rc[offset] == 0) {
            if (PtlInternalAtomicCas64(&(rc[offset]), 0, 1) == 0) {
                cts[offset].success = 0;
                cts[offset].failure = 0;
                ct.s.code           = offset;
                *ct_handle          = ct.a;
                return PTL_OK;
            }
        }
    }
    *ct_handle = PTL_INVALID_HANDLE;
    return PTL_NO_SPACE;
}                                      /*}}} */

int API_FUNC PtlCTFree(ptl_handle_ct_t ct_handle)
{                                      /*{{{ */
    const ptl_internal_handle_converter_t ct = { ct_handle };
    ptl_internal_header_t *restrict       hdr;

#ifndef NO_ARG_VALIDATION
    if (PtlInternalLibraryInitialized() == PTL_FAIL) {
        return PTL_NO_INIT;
    }
    if (PtlInternalCTHandleValidator(ct_handle, 0)) {
        return PTL_ARG_INVALID;
    }
#endif
    if ((nit_limits[ct.s.ni].max_triggered_ops > 0) && (nit.refcount[ct.s.ni] > 0)) {
        /* step 1: get a local memory fragment */
        hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t) + sizeof(PTL_CMD_LOCK_TYPE));
        /* step 2: fill the op structure */
        hdr->type     = HDR_TYPE_CMD;
        hdr->ni       = ct.s.ni;
        hdr->src      = proc_number;
        hdr->target   = proc_number;
        hdr->pt_index = CMD_TYPE_CTFREE;
        hdr->hdr_data = ct_handle;

        /* step 3: load up data... */
        PTL_CMD_LOCK_SENDER1(hdr->data);

        /* step 4: enqueue the op structure on the target */
        PtlInternalFragmentToss(hdr, proc_number);
        PTL_CMD_LOCK_SENDER2(hdr->data);

        PtlInternalAtomicInc(&(ct_event_refcounts[ct.s.ni][ct.s.code]), -1);
        while (ct_event_refcounts[ct.s.ni][ct.s.code] != 0) SPINLOCK_BODY();
    } else {
        ct_events[ct.s.ni][ct.s.code] = CTERR;
        __sync_synchronize();
    }
    return PTL_OK;
}                                      /*}}} */

void INTERNAL PtlInternalCTFree(ptl_internal_header_t *restrict hdr)
{   /*{{{*/
    const ptl_internal_handle_converter_t ct = { hdr->hdr_data };
    ordered_NEMESIS_queue                *q  = &(ct_event_triggers[hdr->ni][ct.s.code]);
    ptl_internal_trigger_t               *trigger;

    PtlInternalCTPullTriggers((ptl_handle_ct_t)(hdr->hdr_data));
    trigger = PtlInternalOrderedNEMESISDequeue(q, CT_ERR_VAL);
    while (trigger != NULL) {
        trigger = PtlInternalOrderedNEMESISDequeue(q, CT_ERR_VAL);
    }
    ct_events[hdr->ni][ct.s.code] = CTERR;
    __sync_synchronize();
    PTL_CMD_LOCK_PROGRESS(hdr->data);
} /*}}}*/

void INTERNAL PtlInternalCTPullTriggers(ptl_handle_ct_t ct_handle)
{   /*{{{*/
    const ptl_internal_handle_converter_t ct = { ct_handle };
    ordered_NEMESIS_queue                *q  = &(ct_event_triggers[ct.s.ni][ct.s.code]);
    ptl_internal_trigger_t               *trigger;
    ptl_size_t                            cur_val = ct_events[ct.s.ni][ct.s.code].success + ct_events[ct.s.ni][ct.s.code].failure;

    trigger = PtlInternalOrderedNEMESISDequeue(q, cur_val);
    while (trigger != NULL) {
        PtlInternalTriggerPull(trigger);
        PtlInternalCTFreeTrigger(trigger, ct.s.ni);
        trigger = PtlInternalOrderedNEMESISDequeue(q, cur_val);
    }
} /*}}}*/

void INTERNAL PtlInternalCTCancelTriggers(ptl_handle_ct_t ct_handle)
{   /*{{{*/
    const ptl_internal_handle_converter_t ct = { ct_handle };
    ordered_NEMESIS_queue                *q  = &(ct_event_triggers[ct.s.ni][ct.s.code]);
    ptl_internal_trigger_t               *trigger;

    trigger = PtlInternalOrderedNEMESISDequeue(q, CT_ERR_VAL);
    while (trigger != NULL) {
        PtlInternalCTFreeTrigger(trigger, ct.s.ni);
        trigger = PtlInternalOrderedNEMESISDequeue(q, CT_ERR_VAL);
    }
} /*}}}*/

int API_FUNC PtlCTCancelTriggered(ptl_handle_ct_t ct_handle)
{   /*{{{*/
    const ptl_internal_handle_converter_t ct = { ct_handle };

#ifndef NO_ARG_VALIDATION
    if (PtlInternalLibraryInitialized() == PTL_FAIL) {
        return PTL_NO_INIT;
    }
    if (PtlInternalCTHandleValidator(ct_handle, 0)) {
        return PTL_ARG_INVALID;
    }
#endif

    if (ct_event_triggers[ct.s.ni][ct.s.code].head.ptr != NULL) {
        ptl_internal_header_t *restrict hdr;
        /* step 1: get a local memory fragment */
        hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t));
        /* step 2: fill the op structure */
        hdr->type     = HDR_TYPE_CMD;
        hdr->ni       = ct.s.ni;
        hdr->src      = proc_number;
        hdr->target   = proc_number;
        hdr->pt_index = CMD_TYPE_CANCEL;
        hdr->hdr_data = ct_handle;

        /* step 3: prepare to wait for the progress thread... */
        PTL_CMD_LOCK_SENDER1(hdr->data);

        /* step 4: enqueue the op structure on the target */
        PtlInternalFragmentToss(hdr, proc_number);

        /* step 5: wait for the progress thread to touch my fragment */
        PTL_CMD_LOCK_SENDER2(hdr->data);
    }
    return PTL_OK;
} /*}}}*/

int API_FUNC PtlCTGet(ptl_handle_ct_t ct_handle,
                      ptl_ct_event_t *event)
{                                      /*{{{ */
    const ptl_internal_handle_converter_t ct = { ct_handle };

#ifndef NO_ARG_VALIDATION
    if (PtlInternalLibraryInitialized() == PTL_FAIL) {
        return PTL_NO_INIT;
    }
    if (PtlInternalCTHandleValidator(ct_handle, 0)) {
        return PTL_ARG_INVALID;
    }
    if (event == NULL) {
        return PTL_ARG_INVALID;
    }
#endif
    *event = ct_events[ct.s.ni][ct.s.code];
    return PTL_OK;
}                                      /*}}} */

int API_FUNC PtlCTWait(ptl_handle_ct_t ct_handle,
                       ptl_size_t      test,
                       ptl_ct_event_t *event)
{                                      /*{{{ */
    const ptl_internal_handle_converter_t ct = { ct_handle };
    volatile ptl_ct_event_t              *cte;
    volatile uint_fast64_t               *rc;

#ifndef NO_ARG_VALIDATION
    if (PtlInternalLibraryInitialized() == PTL_FAIL) {
        return PTL_NO_INIT;
    }
    if (PtlInternalCTHandleValidator(ct_handle, 0)) {
        return PTL_ARG_INVALID;
    }
    if (event == NULL) {
        return PTL_ARG_INVALID;
    }
#endif
    cte = &(ct_events[ct.s.ni][ct.s.code]);
    rc  = &(ct_event_refcounts[ct.s.ni][ct.s.code]);
    // printf("waiting for CT(%llu) sum to reach %llu\n", (unsigned long
    // long)ct.i, (unsigned long long)test);
    PtlInternalAtomicInc(rc, 1);
    do {
        ptl_ct_event_t tmpread = *cte;
        if (__builtin_expect((tmpread.success == CT_ERR_VAL), 0) ||
            __builtin_expect((tmpread.failure == CT_ERR_VAL), 0)) {
            PtlInternalAtomicInc(rc, -1);
            return PTL_INTERRUPTED;
        } else if ((tmpread.failure != 0) ||
                   ((tmpread.success + tmpread.failure) >= test)) {
            if (event != NULL) {
                *event = tmpread;
            }
            PtlInternalAtomicInc(rc, -1);
            return PTL_OK;
        }
        while (tmpread.success == cte->success &&
               tmpread.failure == cte->failure) SPINLOCK_BODY();
    } while (1);
}                                      /*}}} */

int API_FUNC PtlCTPoll(const ptl_handle_ct_t *ct_handles,
                       const ptl_size_t      *tests,
                       unsigned int           size,
                       ptl_time_t             timeout,
                       ptl_ct_event_t        *event,
                       unsigned int          *which)
{                                      /*{{{ */
    ptl_size_t              ctidx, offset;
    ptl_ct_event_t         *ctes[size];
    volatile uint_fast64_t *rcs[size];
    size_t                  nstart;
    TIMER_TYPE              tp;

#ifndef NO_ARG_VALIDATION
    if (PtlInternalLibraryInitialized() == PTL_FAIL) {
        return PTL_NO_INIT;
    }
    if ((ct_handles == NULL) || (tests == NULL) || (size == 0)) {
        return PTL_ARG_INVALID;
    }
    for (ctidx = 0; ctidx < size; ++ctidx) {
        if (PtlInternalCTHandleValidator(ct_handles[ctidx], 0)) {
            return PTL_ARG_INVALID;
        }
    }
    if (size > UINT32_MAX) {
        return PTL_ARG_INVALID;
    }
    if (event == NULL) {
        return PTL_ARG_INVALID;
    }
    if (which == NULL) {
        return PTL_ARG_INVALID;
    }
#endif /* ifndef NO_ARG_VALIDATION */
    for (ctidx = 0; ctidx < size; ++ctidx) {
        const ptl_internal_handle_converter_t ct = { ct_handles[ctidx] };
        ctes[ctidx] = &(ct_events[ct.s.ni][ct.s.code]);
        rcs[ctidx]  = &(ct_event_refcounts[ct.s.ni][ct.s.code]);
        PtlInternalAtomicInc(rcs[ctidx], 1);
    }
    {
        TIMER_TYPE start;
        MARK_TIMER(start);
        nstart = TIMER_INTS(start);
    }
    if (timeout != PTL_TIME_FOREVER) { // convert from milliseconds to timer units
        MILLI_TO_TIMER_INTS(timeout);
    }
    {
        uint32_t t = size - 1;
        t     |= t >> 1;
        t     |= t >> 2;
        t     |= t >> 4;
        t     |= t >> 8;
        t     |= t >> 16;
        offset = nstart & t;           // pseudo-random, guarantees offset < size
    }
    assert(offset < size);
    do {
        /* these two for loops MUST be identical (except for the bounds);
         * doing two loops to avoid a modulo operation in every iteration */
        for (ctidx = offset; ctidx < size; ++ctidx) {
            ptl_ct_event_t tmpread = *ctes[ctidx];
            if (__builtin_expect((tmpread.success == CT_ERR_VAL), 0) ||
                __builtin_expect((tmpread.failure == CT_ERR_VAL), 0)) {
                for (size_t idx = 0; idx < size; ++idx) PtlInternalAtomicInc(rcs[idx], -1);
                return PTL_INTERRUPTED;
            } else if ((tmpread.success + tmpread.failure) >= tests[ctidx]) {
                *event = tmpread;
                *which = (unsigned int)ctidx;
                for (size_t idx = 0; idx < size; ++idx) PtlInternalAtomicInc(rcs[idx], -1);
                return PTL_OK;
            }
        }
        for (ctidx = 0; ctidx < offset; ++ctidx) {
            ptl_ct_event_t tmpread = *ctes[ctidx];
            if (__builtin_expect((tmpread.success == CT_ERR_VAL), 0) ||
                __builtin_expect((tmpread.failure == CT_ERR_VAL), 0)) {
                for (size_t idx = 0; idx < size; ++idx) PtlInternalAtomicInc(rcs[idx], -1);
                return PTL_INTERRUPTED;
            } else if ((tmpread.success + tmpread.failure) >= tests[ctidx]) {
                *event = tmpread;
                *which = (unsigned int)ctidx;
                for (size_t idx = 0; idx < size; ++idx) PtlInternalAtomicInc(rcs[idx], -1);
                return PTL_OK;
            }
        }
        MARK_TIMER(tp);
    } while (timeout == PTL_TIME_FOREVER ||
             (TIMER_INTS(tp) - nstart) < timeout);
    return PTL_CT_NONE_REACHED;
}                                      /*}}} */

int API_FUNC PtlCTSet(ptl_handle_ct_t ct_handle,
                      ptl_ct_event_t  test)
{                                      /*{{{ */
    const ptl_internal_handle_converter_t ct = { ct_handle };

#ifndef NO_ARG_VALIDATION
    if (PtlInternalLibraryInitialized() == PTL_FAIL) {
        return PTL_NO_INIT;
    }
    if (PtlInternalCTHandleValidator(ct_handle, 0)) {
        return PTL_ARG_INVALID;
    }
#endif
    ct_events[ct.s.ni][ct.s.code] = test;
    __sync_synchronize();
    return PTL_OK;
}                                      /*}}} */

int API_FUNC PtlCTInc(ptl_handle_ct_t ct_handle,
                      ptl_ct_event_t  increment)
{                                      /*{{{ */
    const ptl_internal_handle_converter_t ct = { ct_handle };
    ptl_ct_event_t                       *cte;

#ifndef NO_ARG_VALIDATION
    if (PtlInternalLibraryInitialized() == PTL_FAIL) {
        return PTL_NO_INIT;
    }
    if (PtlInternalCTHandleValidator(ct_handle, 0)) {
        return PTL_ARG_INVALID;
    }
#endif
    cte = &(ct_events[ct.s.ni][ct.s.code]);
    if (increment.failure == 0) {
        /* cheaper than a 128-bit atomic increment */
        PtlInternalAtomicInc(&(cte->success), increment.success);
    } else if (increment.success == 0) {
        /* cheaper than a 128-bit atomic increment */
        PtlInternalAtomicInc(&(cte->failure), increment.failure);
    } else {
        return PTL_ARG_INVALID;
    }
    if (ct_event_triggers[ct.s.ni][ct.s.code].head.ptr != NULL) {
        PtlInternalCTTriggerCheck(ct_handle);
    }
    return PTL_OK;
}                                      /*}}} */

void INTERNAL PtlInternalCTTriggerCheck(ptl_handle_ct_t ct_handle)
{   /*{{{*/
    const ptl_internal_handle_converter_t ct = { ct_handle };

    if (ct_event_triggers[ct.s.ni][ct.s.code].head.ptr != NULL) {
        ptl_internal_header_t *restrict hdr;
        /* step 1: get a local memory fragment */
        hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t));
        /* step 2: fill the op structure */
        hdr->type     = HDR_TYPE_CMD;
        hdr->ni       = ct.s.ni;
        hdr->src      = proc_number;
        hdr->target   = proc_number;
        hdr->pt_index = CMD_TYPE_CHECK;
        hdr->hdr_data = ct_handle;

        /* step 3: enqueue the op structure on the target */
        PtlInternalFragmentToss(hdr, proc_number);
    }
} /*}}}*/

void INTERNAL PtlInternalCTUnorderedEnqueue(ptl_internal_header_t *restrict hdr)
{   /*{{{*/
    const ptl_internal_handle_converter_t ct = { hdr->hdr_data };
    ordered_NEMESIS_queue *restrict       q  = &(ct_event_triggers[hdr->ni][ct.s.code]);
    ordered_NEMESIS_ptr                   cursor, prev;
    ptl_internal_trigger_t *restrict      t = (ptl_internal_trigger_t *)hdr->user_ptr;
    ordered_NEMESIS_ptr                   f = { .ptr = (void *)t, .val = t->threshold };

    PtlInternalCTPullTriggers((ptl_handle_ct_t)(hdr->hdr_data));
    PtlInternalAtomicRead128(&cursor, &q->head);
    if ((cursor.ptr == NULL) || (cursor.val > t->threshold)) {
        /* insert at head */
        prev = PtlInternalAtomicSwap128(&(q->tail), f);
        if (prev.val > f.val) { // someone else has appended, so prepending is easy
            while (q->head.ptr == NULL) SPINLOCK_BODY();
            f.ptr->next = q->head;
            q->head     = f;
        } else {
            if (prev.ptr == NULL) {
                PtlInternalAtomicWrite128(&q->head, f);
            } else {
                PtlInternalAtomicWrite128(&prev.ptr->next, f);
            }
        }
    } else {
        do {
            prev = cursor;
            PtlInternalAtomicRead128(&cursor, &cursor.ptr->next);
        } while (cursor.ptr != NULL && cursor.val < t->threshold);
        /* insert after prev */
        prev.ptr->next = f;         // this does NOT need to be atomic
        f.ptr->next    = cursor;    // this does NOT need to be atomic
        assert(cursor.ptr != NULL); // otherwise this would have been an append operation
    }
    PtlInternalCTPullTriggers((ptl_handle_ct_t)(hdr->hdr_data)); // just in case
    __sync_synchronize();
    PTL_CMD_LOCK_PROGRESS(hdr->data);
} /*}}}*/

void INTERNAL PtlInternalCTSuccessInc(ptl_handle_ct_t ct_handle,
                                      ptl_size_t      increment)
{                                      /*{{{ */
    const ptl_internal_handle_converter_t ct = { ct_handle };
    ptl_ct_event_t                       *cte;

#ifndef NO_ARG_VALIDATION
    assert(PtlInternalCTHandleValidator(ct_handle, 0) == 0);
#endif
    cte = &(ct_events[ct.s.ni][ct.s.code]);
    PtlInternalAtomicInc(&(cte->success), increment);
}                                      /*}}} */

void INTERNAL PtlInternalCTFailureInc(ptl_handle_ct_t ct_handle)
{                                      /*{{{ */
    const ptl_internal_handle_converter_t ct = { ct_handle };
    ptl_ct_event_t                       *cte;

#ifndef NO_ARG_VALIDATION
    assert(PtlInternalCTHandleValidator(ct_handle, 0) == 0);
#endif
    cte = &(ct_events[ct.s.ni][ct.s.code]);
    PtlInternalAtomicInc(&(cte->failure), 1);
}                                      /*}}} */

/* vim:set expandtab: */
