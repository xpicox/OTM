#include "OTM.h"
#include "stdlib.h"
#include "assert.h"


// Helper macros for iterating over entries within a transaction
// record

#define FOR_EACH_ENTRY(_t,_x,CODE) do {                                         \
  OTRecHeader *__t = (_t);                                                      \
  while(__t -> forward_trec -> next != __t || __t -> forward_trec != NULL )             \
    __t = __t -> forward_trec -> next;                                                  \
  OTRecChunk *__c = __t -> current_chunk;                                       \
  StgWord __limit = __c -> next_entry_idx;                                      \
  while (__c != NULL) {                                                         \
    StgWord __i;                                                                \
    for (__i = 0; __i < __limit; __i ++) {                                      \
      OTRecEntry *_x = &(__c -> entries[__i]);                                  \
      do { CODE } while (0);                                                    \
    }                                                                           \
    __c = __c -> prev_chunk;                                                    \
    __limit = __c -> next_entry_idx;                                            \
  }                                                                             \
 exit_for_each:                                                                 \
  if (0) goto exit_for_each;                                                    \
} while (0)

#define BREAK_FOR_EACH goto exit_for_each

// returns 1 if the 2 stableptrs refer to the same haskell object
HsBool compareStablePtr(HsStablePtr s1, HsStablePtr s2) {
    return deRefStablePtr(s1) == deRefStablePtr(s2);
}

OtmStablePtr new_otm_stableptr (HsStablePtr sp) {
    OtmStablePtr osp = (OtmStablePtr) malloc(sizeof(struct OtmStablePtr_));
    osp -> ptr = sp;
    osp -> count = 1;
    return osp;
}

OtmStablePtr acquire_otm_stable_ptr(OtmStablePtr osp){
    atomic_inc((StgVolatilePtr)&osp -> count, 1);
    return osp;
}

void release_otm_stable_ptr(OtmStablePtr osp) {
    atomic_dec((StgVolatilePtr)&osp -> count);
    if(osp -> count == 0) {
        hs_free_stable_ptr(osp -> ptr);
        free(osp);
    }
}

/* Initialaze a Cond struct */
void init_condition_variable(Cond* variable){
    /* defined in OSThread.h */
    initMutex(&variable -> mutex);
    /* defined in OSThread.h */
    initCondition(&variable -> cond);
    variable -> value = 0;
    variable -> generation = 0; // Probably not the right place to init the variable
};

void waitOn(OTRecHeader* trec) {
    Cond* cond_var = &trec -> condition_variable;
    ACQUIRE_LOCK(&cond_var -> mutex);
    while(cond_var -> value == 0) {
        waitCondition(&cond_var -> cond, &cond_var -> mutex);
    }
    if (cond_var -> value != 0) {
        cond_var -> value = 0;
    }
    RELEASE_LOCK(&cond_var -> mutex);
}

void broadcastOn(OTRecHeader* trec) {
    Cond* cond_var = &trec -> condition_variable;
    ACQUIRE_LOCK(&cond_var -> mutex);
    cond_var -> value = 1;
    broadcastCondition(&cond_var -> cond);
    RELEASE_LOCK(&cond_var -> mutex);
}

void init_OTRecState(OTRecState volatile* state) {
    state -> state = OTREC_RUNNING;
    state -> running = 1;
    state -> threads = 1;
}

OTRecChunk* get_new_otrec_chunck();
OTRecNode* new_union_node();
// Use a foreign ptr to handle this
OTRecHeader* otmStartTransaction(HsStablePtr thread_id, OTRecHeader* outer) {
    OTRecHeader* trec = (OTRecHeader*) malloc(sizeof(OTRecHeader));
    trec -> enclosing_trec = outer;
    if (outer == NULL) {
        // Starting an Open Transaction
        printf("Open Transaction!\n");
        trec -> forward_trec = new_union_node();
        trec -> forward_trec -> next = trec;
        trec -> forward_trec -> rank = 0;
    } else {
        // starting an Isolated Transaction
        trec -> forward_trec = NULL;
    }
    init_OTRecState(&trec -> state);
    init_condition_variable(&trec -> condition_variable);
    trec -> current_chunk = get_new_otrec_chunck();
    return trec;
}

// EXTERN_INLINE StgInfoTable *reallyLockClosure(StgClosure *p)
// {
//     StgWord info;
//     do {
//         nat i = 0;
//         do {
//             info = xchg((P_)(void *)&p -> header.info, (W_)&stg_WHITEHOLE_info);
//             if (info != (W_)&stg_WHITEHOLE_info) return (StgInfoTable *)info;
//         } while (++i < SPIN_COUNT);
//         yieldThread();
//     } while (1);
// }

// EXTERN_INLINE void unlockClosure(StgClosure *p, const StgInfoTable *info)
// {
//     // This is a strictly ordered write, so we need a write_barrier():
//     write_barrier();
//     p -> header.info = info;
// }

OTState lock_OTRec(OTRecHeader* trec) {
    StgWord state;
    do {    // SpinLock
        state = xchg((StgPtr)(void*)&(trec -> state.state),(StgWord64)OTREC_LOCKED);
        if (state != (StgWord)OTREC_LOCKED) return (OTState)state;
    } while(1);
}

// OTState lock_OTRec(OTRecHeader* trec) {
//     StgWord state;
//     do {    // SpinLock
        
//     }
//     while(cas((void*)&(trec -> state.state), (StgWord)OTREC_ACTIVE, (StgWord)OTREC_LOCKED));
// }

void unlock_OTRec(OTRecHeader *trec, OTState s){
    write_barrier();
    trec -> state.state = s;
}


HsStablePtr lock_otvar(OTVar* otvar) {
    StgWord state;
    do {    // SpinLock
        state = xchg((StgPtr)(void*)&otvar -> locked, (StgWord)1);
        if (state != (StgWord)1) return otvar -> current_value;
    } while(1);
}

void unlock_otvar(OTVar* otvar){
    write_barrier();
    otvar -> locked = 0;
}

OtmStablePtr read_current_value(OTVar* otvar) {
    OtmStablePtr result = otvar -> current_value;
    while (otvar -> locked) {
        result = otvar -> current_value;
    }
    return result;
}
/* When i call this function otvar -> delta -> trec cannot commit
   otvar -> delta != NULL because i found a trec such that otvar is in trec
   otvar -> delta cannot become NULL because delta -> trec cannot commit until
   the transaction who called read_delta_value commits */
OtmStablePtr read_delta_value(OTVar* otvar) {
    return otvar -> delta -> new_value;
}

// OTRecHeader* find(OTRecHeader* trec) {
//     OTRecHeader *result = trec;
//     while(result -> forward_trec != result){
//         result = result -> forward_trec;
//     }
//     return result;
// }

// OTRecHeader* find_path_compression(OTRecHeader* trec) {
//     OTState s = lock_OTRec(trec);
//     if(trec -> forward_trec != trec) {
//         trec -> forward_trec = find(trec -> forward_trec);
//     }
//     unlock_OTRec(trec, s);
//     return trec -> enclosing_trec;
// }

/* Find with Path Compression
   returns a locked trec_header */
// OTRecHeader* find_and_lock(OTRecHeader* trec, OTState* s_out) {
//     OTState s = *s_out = lock_OTRec(trec);
//     if (trec -> forward_trec != trec) {
//         trec -> forward_trec = find_and_lock(trec -> forward_trec, s_out);
//         unlock_OTRec(trec, s);
//     }
//     return trec -> forward_trec;
// }

/*  Wait Free Union-Find with path-compression and union by rank
    http://courses.cs.washington.edu/courses/cse524/95sp/uf.ps */

OTRecNode* new_union_node() {
    return (OTRecNode*)malloc(sizeof(OTRecNode));
}

// Link trec1 to trec2. Updates trec1 not trec2
HsBool update_root(OTRecHeader* trec1, HsWord old_rank,
                   OTRecHeader* trec2, HsWord new_rank) {
    HsBool result;
    // TODO: atomic read ?
    OTRecNode* old = trec1 -> forward_trec;
    if (old -> next != trec1 ||
        old -> rank != old_rank) {
        return 0;
    }
    OTRecNode* new = new_union_node();
    new -> next = trec2;
    new -> rank = new_rank;
    // new -> state = old -> state;
    // new -> threads = old -> threads;
    // new -> running = old -> running;
    result = (cas((StgVolatilePtr)&trec1 -> forward_trec,(StgWord)old,(StgWord)new) == (StgWord)old);
    if (!result) {
        free(new);
    }
    return result;
}

// HsBool update_state(OTRecHeader* trec1, OTRecHeader* trec2){
//     OTRecState* old = trec -> forward_trec;
//     if (old -> threads != old_threads ||
//         old -> running != old_running ) {
//         return 0;
//     }
//     OTRecState* new = new_union_node();
//     new -> next = old -> next;
//     new -> rank = old -> rank;
//     new -> state = old -> state;
//     new -> threads = new_threads;
//     new -> running = new_running;
//     result = (cas((StgVolatilePtr)&trec1 -> forward_trec,(StgWord)old,(StgWord)new) == (StgWord)old);
//     if(!result) {
//         free(new);
//     }
//     return result;
// }

OTRecChunk* find_last_chunck(OTRecChunk* chunck) {
    OTRecChunk* result = chunck;
    while (result -> prev_chunk != NULL) {
        result = result -> prev_chunk;
    }
    return result;
}

/* link chunck1 to the list rapresented by chunck2 */
void link_trec_chuncks(OTRecChunk* chunck1, OTRecChunk* chunck2) {
    OTRecChunk* last = chunck2;
    do {
        last = find_last_chunck(last);
    } while(cas((StgVolatilePtr)&last -> prev_chunk, (StgWord)NULL, (StgWord)chunck1) != (StgWord)NULL);
}
/*  TRUE if t1 < t2
    FALSE otherwise
*/
HsBool is_less_than(OTRecHeader* trec1, OTRecHeader* trec2) {
    HsBool result;
    result = (trec1 -> forward_trec -> rank < trec2 -> forward_trec -> rank) ||
             (trec1 -> forward_trec -> rank == trec2 -> forward_trec -> rank &&
              trec1 < trec2 );
    return result;
}

HsBool is_greater_then(OTRecHeader* trec1, OTRecHeader* trec2) {
    HsBool result;
    result = (trec1 -> forward_trec -> rank > trec2 -> forward_trec -> rank) ||
             (trec1 -> forward_trec -> rank == trec2 -> forward_trec -> rank &&
              trec1 > trec2 );
    return result;
}

/* Fix to long equal ranked chains 
   To call at the end of the Union */
void setRoot(OTRecHeader* trec) {
    OTRecHeader *y = trec, *t;
    while (y -> forward_trec -> next != y) {
        t = y -> forward_trec -> next;
        cas((StgVolatilePtr)&(y -> forward_trec -> next), (StgWord)t, (StgWord)t -> forward_trec -> next);
        y = t -> forward_trec -> next;
    }
    update_root(y, trec -> forward_trec -> rank,
                y, trec -> forward_trec -> rank + 1);
}

OTRecHeader* find(OTRecHeader* trec) {
    OTRecHeader *y;
    OTRecHeader *t;
    y = trec;
    while (trec != trec -> forward_trec -> next) {
        trec = trec -> forward_trec -> next;
    }
    while (is_less_than(y, trec)) {
        t = y -> forward_trec -> next;
        cas((StgVolatilePtr)&(y -> forward_trec -> next), (StgWord)t, (StgWord)trec);
        y = t -> forward_trec -> next;
    }
    return trec;
}

HsBool same_set(OTRecHeader* trec1, OTRecHeader* trec2) {
    OTRecHeader *x, *y;
    while (1) {
        x = find(trec1);
        y = find(trec2);
        if (x == y) {
            return 1;
        }
        else if (x -> forward_trec -> next == x) {
            return 0;
        }
    }
}

// Returns 0 if has not benn completed
HsBool otmUnion(OTRecHeader* trec1, OTRecHeader* trec2){
    OTRecHeader *x, *y, *temp;
    OTState x_s, y_s;
    // OTRecState *x_state, *y_state;
    HsWord x_rank, y_rank, t_rank, x_running, y_running, x_threads, y_threads;
    HsBool result;
    do {
        x = find(trec1);
        y = find(trec2);
        if (x == y) {
            return 1;
        }
        if (is_greater_then(x, y)) {
            temp = x;
            x = y;
            y = temp;
        }
        x_rank = x -> forward_trec -> rank;
        y_rank = y -> forward_trec -> rank;
        x_s = lock_OTRec(x);
        y_s = lock_OTRec(y);
        if ((x_s | y_s) & (OTREC_ABORTED | OTREC_COMMITTED | OTREC_RETRY)) {
            unlock_OTRec(y, y_s);
            unlock_OTRec(x, x_s);
            return 0;
        }
        result = update_root(x, x_rank, y, y_rank);
        if (result) {
            if (x_rank == y_rank) {
                update_root(y, y_rank, y, y_rank +1);
            }
            // Fix (long) equal ranked chains
            setRoot(x);
            y -> state.running += x -> state.running;
            y -> state.threads += x -> state.threads;
        }
        unlock_OTRec(y, y_s);
        unlock_OTRec(x, x_s);
    } while (!result);
    // append working memory of x to the working memory of y
    // without locking
    link_trec_chuncks(x -> current_chunk, y -> current_chunk);
    return 1;
}




// MI POSSO FONDERE SOLO CON TRASAZIONI IN STATO DI RUNNING!!!! COSA CHE NON CONTROLLO
// FONDO SEMPRE LOWER A GREATER -> NON POSSO USARE UNION BY RANK
// void merge(OTRecHeader* lower, OTRecHeader* greater){
//     lower -> forward_trec = greater;
//     OTRecChunk* c = greater -> current_chunk;
//     /* Find the last chunk of greater */
//     while(c -> prev_chunk != NULL) {
//         c = c -> prev_chunk;
//     }
//     /* Append the chunks of lower to the chunks of greater */
//     c -> prev_chunk = lower -> current_chunk;
//     // TODO: Update the state of the trecs
//     greater -> state.count++;
// }

// TODO: Update the state of a running transaction
/*
    Regola: lockare sempre prima lower
      <    <    <
    a -> b -> c -> d
    v <    <
    e -> f -> g
    
    h < e < f < g
    v
    a < b < c < d
    
    ipotesi:
    h > a cerco di lockare prima d e poi g

    e < a cerco di lockare prima g e poi d (non può accadere perche e > h > a)
    e < b cerco di lockare prima g e poi d (caso possibile => DEADLOCK)
    a < h < e < b 
    
*/
// HsBool otmUnion(OTRecHeader* trec1, OTRecHeader* trec2) {
//     OTRecHeader *lower;
//     OTRecHeader *greater;
//     OTRecHeader *temp;
//     OTState s1, s2;
//     if((StgWord)trec1 < (StgWord)trec2) {
//         lower = trec1;
//         greater = trec2;
//     } else {
//         lower = trec2;
//         greater = trec1;
//     }
//     lower = find_and_lock(lower, &s1);
//     if(s1 != OTREC_ACTIVE) {
//         unlock_OTRec(lower, s1);
//         return 0;
//     }
//     /*  Neseccesaria una find senza lock perchè se sono sotto la stessa
//         radice creo un deadlock */
//     greater = find(greater);
//     if(lower == greater) {
//         unlock_OTRec(lower, s1);
//         return 1;
//     }
//     /*  Qui probabilmente otterò un nuovo greater, ma non mi interessa 
//         perchè sicuramente non sarà uguale a lower per via dello spinlock */
//     greater = find_and_lock(greater, &s2);
//     if(s2 != OTREC_ACTIVE) {
//         unlock_OTRec(greater, s2);
//         return 0;
//     }
//     if (lower > greater) {
//         temp = greater;
//         greater = lower;
//         lower = temp;
//     }
//     merge(lower, greater);
//     unlock_OTRec(greater, s2);
//     unlock_OTRec(lower, s1);
//     return 1;
// }

OTVarDelta* get_new_delta() {
    return (OTVarDelta*) malloc(sizeof(OTVarDelta));
}

OTRecEntry* get_new_entry(OTRecHeader* trec) {
    OTRecEntry *result;
    OTRecChunk *c;
    int i;
    c = trec -> current_chunk;
    i = c -> next_entry_idx;

    if (i < OTREC_CHUNK_NUM_ENTRIES) {
        // Continue to use current chunk
        result = &(c -> entries[i]);
        c -> next_entry_idx ++;
    } else {
        // Current chunk is full: allocate a fresh one
        OTRecChunk *nc;
        nc = (OTRecChunk*) malloc(sizeof(OTRecChunk));
        nc -> prev_chunk = c;
        nc -> next_entry_idx = 1;
        trec -> current_chunk = nc;
        result = &(nc -> entries[0]);
    }

    return result;
}

OTRecChunk* get_new_otrec_chunck(){
    OTRecChunk *result;
    result = (OTRecChunk*)malloc(sizeof(OTRecChunk));
    result -> prev_chunk = NULL;
    result -> next_entry_idx = 0;
    return result;
};

OTRecEntry* get_entry_for(OTRecHeader* trec, OTVar* otvar, OTRecHeader** in) {
    OTRecEntry *result = NULL;
    assert(trec != NULL && otvar != NULL);
    do {
        FOR_EACH_ENTRY(trec, e, {
            if (e -> otvar == otvar) {
                result = e;
                if (in != NULL) {
                    *in = trec;
                }
                BREAK_FOR_EACH;
            }
        });
        trec = trec -> enclosing_trec;
    } while (result == NULL && trec != NULL);
    return result;
}

OTRecHeader* find_first_open_transaction(OTRecHeader* trec) {
    assert(trec -> forward_trec == NULL); // trec is isolated
    while(trec -> forward_trec == NULL) {
        trec = trec -> enclosing_trec;
    }
    return trec;
}

OTVar* otmNewOTVar(HsStablePtr value){
    OTVar* otvar = (OTVar*)malloc(sizeof(OTVar));
    otvar -> current_value = new_otm_stableptr(value);
    otvar -> first_watch_queue_entry = NULL;
    otvar -> locked = 0;
    otvar -> delta = NULL;
    return otvar;
}

void otmDeleteOTVar(OTVar* otvar) {
    release_otm_stable_ptr(otvar -> delta -> new_value);
    release_otm_stable_ptr(otvar -> current_value);
    // TODO: free the watch queue!!
    free(otvar -> delta);
    free(otvar);
}

OTState get_state(OTRecHeader* trec) {
    assert(trec -> forward_trec != NULL);
    OTRecHeader* x = find(trec);
    OTState state = lock_OTRec(x);
    unlock_OTRec(x, state);
    return state;
}

HsBool isValid(OTRecHeader* trec, OTState* state) {
    assert(trec -> forward_trec != NULL);
    *state = get_state(trec);
    assert(!(*state & (OTREC_COMMITTED | OTREC_RETRYED | OTREC_ABORTED)));
    return !(*state & (OTREC_RETRY | OTREC_ABORT));
}

/*  Inits the working memory of an OTVar if it's empty
    returns FALSE if the memory was already initialized
*/
HsBool otm_acquire_OTVar_read(OTRecHeader* trec, OTVar* otvar) {
    if (hs_atomicread64((StgWord64*)&(otvar -> delta)) == (StgWord64)NULL) {
        OTVarDelta *d = get_new_delta(); // se si tiene un pool di working memory non è male
        d -> new_value = acquire_otm_stable_ptr(otvar -> current_value);
        d -> trec = trec;
        // TODO : required a barrier?
        if(cas((StgVolatilePtr)&otvar->delta, (StgWord)NULL, (StgWord)d) == (StgWord)NULL) {
            OTRecEntry * ne = get_new_entry(trec);
            ne -> otvar = otvar;
            return 1;
        } else {
            release_otm_stable_ptr(d -> new_value);
            free(d);
        }
    }
    return 0;
}

HsBool otm_acquire_OTVar_write(OTRecHeader* trec, OTVar* otvar, HsStablePtr new_value){
    if (hs_atomicread64((StgWord64*)&(otvar -> delta)) == (StgWord64)NULL) {
        OTVarDelta *d = get_new_delta();
        d -> new_value = new_otm_stableptr(new_value);
        d -> trec = trec;
        // TODO : required a barrier?
        if(cas((StgVolatilePtr)&otvar->delta, (StgWord)NULL, (StgWord)d) == (StgWord)NULL) {
            OTRecEntry * ne = get_new_entry(trec);
            ne -> otvar = otvar;
            return 1;
        } else {
            release_otm_stable_ptr(d -> new_value);
            free(d);
        }
    }
    return 0;
}

HsStablePtr otmReadOTVar(OTRecHeader* trec, OTVar* otvar) {
    assert(trec -> forward_trec != NULL);
    // OTState *state;
    // if (!isValid(trec, state)) {
    //     return NULL;
    // }
    if(!otm_acquire_OTVar_read(trec, otvar)) {
        if(!otmUnion(trec, otvar -> delta -> trec)){
            // Retry the read
            // otvar's trec is OTREC_ABORTED || OTREC_COMMITTED || OTREC_RETRY
            return otmReadOTVar(trec, otvar);
        }
    }
    return (HsStablePtr)hs_atomicread64((StgWord64*)&(otvar -> delta -> new_value -> ptr));
}

/*  Da rivedere perchè non sembra funzionare molto bene,
    molto probabilmente devo lockare anche l'otvar perchè se inizia un commit
    durante la fusione sballo tutto. Da rivedere dopo il codice del commit */
void otmWriteOTVar(OTRecHeader *trec, OTVar *otvar, HsStablePtr new_value) {
    assert(trec -> forward_trec != NULL);
    if(!otm_acquire_OTVar_write(trec, otvar, new_value)) {
        // if the OTVar is already acquired by someone else
        if(!otmUnion(trec, otvar -> delta -> trec)) {
            return otmWriteOTVar(trec, otvar, new_value);
        } else {
            // the Union was successfull or i was already joined
            OtmStablePtr n_value = new_otm_stableptr(new_value);
            OtmStablePtr old =(OtmStablePtr)xchg((StgPtr)(void*)&(otvar -> delta -> new_value),(StgWord)n_value);
            release_otm_stable_ptr(old);
        }
    }
}


HsStablePtr itmReadOTVar(OTRecHeader* trec, OTVar* otvar) {
    OtmStablePtr result = NULL;
    OTRecHeader *entry_in = NULL;
    OTRecEntry *entry = NULL;
    assert(trec -> forward_trec == NULL); // trec is isolated
    OTRecHeader* otrec = find_first_open_transaction(trec);
    otmReadOTVar(otrec, otvar);
    entry = get_entry_for(trec, otvar, &entry_in);
    assert(entry != NULL);
    if(entry != NULL) {
        if (entry_in == trec) {
            // Entry found in our trec
            result = entry -> new_value;
        } else {
            // Entry found in another trec
            OTRecEntry *new_entry = get_new_entry(trec);
            if (trec -> forward_trec == NULL) {
                // Entry found in an Isolated Transaction
                new_entry -> otvar = otvar;
                new_entry -> expected_value = acquire_otm_stable_ptr(entry -> expected_value);
                new_entry -> new_value = acquire_otm_stable_ptr(entry -> new_value);
                result = new_entry -> new_value;
            } else {
                // Entry found in an Open Tranaction -> Read From Delta Memory
                OtmStablePtr current_value = read_delta_value(otvar);
                new_entry -> otvar = otvar;
                new_entry -> expected_value = acquire_otm_stable_ptr(current_value);
                new_entry -> new_value = acquire_otm_stable_ptr(current_value);
                result = new_entry -> new_value;
            }
        }
    } else { // Not possible anymore
        OtmStablePtr current_value = read_current_value(otvar);
        OTRecEntry *new_entry = get_new_entry(trec);
        new_entry -> otvar = otvar;
        new_entry -> expected_value = acquire_otm_stable_ptr(current_value);
        new_entry -> new_value = acquire_otm_stable_ptr(current_value);
        result = current_value;
    }

    return result -> ptr;
}

void itmWriteOTVar(OTRecHeader* trec, OTVar* otvar, HsStablePtr new_value) {
    OTRecHeader *entry_in = NULL;
    OTRecEntry *entry = NULL;
    OtmStablePtr n_value;
    assert(trec -> forward_trec == NULL); // trec is isolated
    entry = get_entry_for(trec, otvar, &entry_in);
    if(entry != NULL) {
        if (entry_in == trec) {
            // Entry found in our trec
            if (compareStablePtr(entry -> expected_value -> ptr, new_value)) {
                // reuse the OtmStablePtr of expected_value
                release_otm_stable_ptr(entry -> new_value);
                entry -> new_value = acquire_otm_stable_ptr(entry -> expected_value);
            } else if (!compareStablePtr(entry -> new_value -> ptr, new_value)) {
                // create a new OtmStablePtr for new_value
                release_otm_stable_ptr(entry -> new_value);
                entry -> new_value = new_otm_stableptr(new_value);
            }
        } else {
            // Entry found in another trec
            OTRecEntry *new_entry = get_new_entry(trec);
            if (trec -> forward_trec == NULL) {
                // Entry found in an Isolated Transaction (enclosing)

                new_entry -> otvar = otvar;
                new_entry -> expected_value = acquire_otm_stable_ptr(entry -> expected_value);

                if (compareStablePtr(entry -> expected_value -> ptr, new_value)) {
                    // reuse the OtmStablePtr of expected_value
                    new_entry -> new_value = acquire_otm_stable_ptr(entry -> expected_value);
                } else if (compareStablePtr(entry -> new_value -> ptr, new_value)) {
                    // reuse the OtmStablePtr of new_value
                    new_entry -> new_value = acquire_otm_stable_ptr(entry -> new_value);
                } else {
                    // create a new OtmStablePtr for new_value
                    new_entry -> new_value = new_otm_stableptr(new_value);
                }

            } else {
                // Entry found in an Open Tranaction -> Read From Delta Memory
                // TODO: Link outstanding open transactions
                OtmStablePtr current_value = read_delta_value(otvar);
                new_entry -> otvar = otvar;
                new_entry -> expected_value = acquire_otm_stable_ptr(current_value);

                if (compareStablePtr(new_entry -> expected_value -> ptr, new_value)) {
                    // reuse the OtmStablePtr of expected_value
                    new_entry -> new_value = acquire_otm_stable_ptr(new_entry -> expected_value);
                } else {
                    // create a new OtmStablePtr for new_value
                    new_entry -> new_value = new_otm_stableptr(new_value);
                }
            }
        }
    } else {
        // No entry found
        OtmStablePtr current_value = read_current_value(otvar);
        OTRecEntry *new_entry = get_new_entry(trec);
        new_entry -> otvar = otvar;
        new_entry -> expected_value = acquire_otm_stable_ptr(current_value);
        if (compareStablePtr(new_entry -> expected_value -> ptr, new_value)) {
            // reuse the OtmStablePtr of expected_value
            new_entry -> new_value = acquire_otm_stable_ptr(new_entry -> expected_value);
        } else {
            // create a new OtmStablePtr for new_value
            new_entry -> new_value = new_otm_stableptr(new_value);
        }
    }
}

// TODO: Commit without locks
// Return values: OTREC_COMMITTED, OTREC_RETRY, OTREC_ABORTED
// OTState otmCommit(OTRecHeader* trec) {
//     assert(trec -> forward_trec != NULL);
//     OTRecHeader* root = find(trec);
//     assert( s && (OTREC_COMMITTED
//             s != OTREC_ABORTED
//             s  OTREC_RETRY  );

//     // root -> state.waiting--;
//     // atomic_dec((StgVolatilePtr)&root -> state.waiting);
//     // switch (s) {
//     //     case OTREC_ACTIVE:
//     //         if(root -> state.waiting == 0) {
//     //             root -> state.state = OTREC_COMMITTED;
//     //         } else {
//     //             root -> state.state = OTREC_COMMITTING;
//     //         }
//     //     case OTREC_COMMITTING:
//     //     case OTREC_RETRYING:
//     //     case OTREC_ABORTING:
//     //         if(root -> state.waiting == 0) {
//     //             // unlock_OTRec(root, s);
//     //             broadcastOn(root);
//     //         } else {
//     //             // unlock_OTRec(root, s);
//     //             waitOn(root);
//     //         }
//     //     case OTREC_CONDEMNED:
//     //     case OTREC_LOCKED:
//     //     case OTREC_COMMITTED:
//     //     case OTREC_RETRY:
//     //     case OTREC_ABORTED:
//     //     case OTREC_WAITING:
//     //         ;
//     // }
//     return root -> state.state;
// }

// OTState otmRetry(OTRecHeader* trec) {
//     assert(trec -> forward_trec != NULL);
//     OTState s;
//     OTRecHeader* root = find(trec);
//     assert( s != OTREC_COMMITTED &&
//             s != OTREC_ABORTED   &&
//             s != OTREC_RETRY  );
//     root -> state.waiting--;
//     switch (s) {
//         case OTREC_ACTIVE:
//         case OTREC_COMMITTING:
//             if(root -> state.waiting == 0) {
//                 root -> state.state = OTREC_RETRY;
//             } else {
//                 root -> state.state = OTREC_RETRYING;
//             }
//         case OTREC_RETRYING:
//         case OTREC_ABORTING:
//             if(root -> state.waiting == 0) {
//                 // unlock_OTRec(root, s);
//                 broadcastOn(root);
//             } else {
//                 // unlock_OTRec(root, s);
//                 waitOn(root);
//             }
//         case OTREC_CONDEMNED:
//         case OTREC_LOCKED:
//         case OTREC_COMMITTED:
//         case OTREC_RETRY:
//         case OTREC_ABORTED:
//         case OTREC_WAITING:
//             ;
//     }
//     return root -> state.state;
// }

// OTState otmAbort(OTRecHeader* trec) {
//     assert(trec -> forward_trec != NULL);
//     OTState s;
//     OTRecHeader* root = find(trec);
    
//     assert( s != OTREC_COMMITTED &&
//             s != OTREC_ABORTED   &&
//             s != OTREC_RETRY  );
//     root -> state.waiting--;
//     switch (s) {
//         case OTREC_ACTIVE:
//         case OTREC_COMMITTING:
//         case OTREC_RETRYING:
//             if(root -> state.waiting == 0) {
//                 root -> state.state = OTREC_ABORTED;
//             } else {
//                 root -> state.state = OTREC_ABORTING;
//             }
//         case OTREC_ABORTING:
//             if(root -> state.waiting == 0) {
//                 // unlock_OTRec(root, s);
//                 broadcastOn(root);
//             } else {
//                 // unlock_OTRec(root, s);
//                 waitOn(root);
//             }
//         case OTREC_CONDEMNED:
//         case OTREC_LOCKED:
//         case OTREC_COMMITTED:
//         case OTREC_RETRY:
//         case OTREC_ABORTED:
//         case OTREC_WAITING:
//             ;
//     }
//     assert(root -> state.state == OTREC_ABORTED);
//     return OTREC_ABORTED;
// }
