/**********************************************************************

  rtl_gen.c - Code for Generation RTL from stack insns.

  Copyright (C) 2017 Vladimir Makarov vmakarov@redhat.com

**********************************************************************/

/* To generate RTL insns we passes stack insns twice.  On the 1st pass
   we define VM stack depth at the start of each stack insn.  Then
   using this info we actually generate RTL insns on the 2nd pass.

   We emulate VM stack generating RTL insns for another RTL insn
   operands in a lazy way.  Therefore the order of RTL insns for
   calculating some simple operands can be different from
   corresponding stack insns.  */

#include "internal.h"
#include "encindex.h"
#include <math.h>

/* We will use insn_stack_increase from insns_info.inc.  */
#define USE_INSN_STACK_INCREASE 1

#include "vm_core.h"
#include "iseq.h"

#include "insns.inc"
#include "insns_info.inc"
#include "gc.h"

#define RTL_GEN_DEBUG 0

/* Type used for label relative displacement during RTL
   generation.  */
typedef long REL_PC;

/*---------- Variable length arrays (VARR) --------------------------*/
/*------- This implementaion supports static type checking. -------- */

/* Asserts for VARR:  */
#if ENABLE_CHECKING
#define VARR_ASSERT_FAIL(OP,VARR) varr_assert_fail(OP,#VARR)

static inline void varr_assert_fail(const char *op, const char *var) {
    fprintf(stderr, "wrong %s for %s", op, var);
    assert(0);
}

#define VARR_ASSERT(EXPR,OP,T)					\
  (void)((EXPR) ? 0 : (VARR_ASSERT_FAIL(OP,VARR(T)), 0))

#else
#define VARR_ASSERT(EXPR,OP,T) ((void)(EXPR))
#endif

/* Name of type for VARR of elements of type T.  */
#define VARR(T) VARR_##T
/* Name of function implementing OP for VARR of elements of type
   T.  */
#define VARR_OP(T, OP) VARR_##T##_##OP

/* Definition of type of VAR of elements of type T.  */
#define VARR_T(T)							      \
typedef struct VARR(T) {                                                      \
    unsigned els_num; /* number of elements currently in VARR */  	      \
    unsigned size;    /* curr size of container VARR in elements */           \
    T *varr;          /* the elements container */			      \
} VARR(T)

/* Default initial size for variable array elements container.  */
#define VARR_DEFAULT_SIZE 64

/* Definition of VARR of elements of type T and all its functions:  */
#define DEF_VARR(T)                                                            \
VARR_T(T);								       \
									       \
static inline void VARR_OP(T, create)(VARR(T) **varr_, unsigned size_) {       \
    VARR(T) *va;						       	       \
    if (size_ == 0)							       \
        size_ = VARR_DEFAULT_SIZE;					       \
    *varr_ = va = (VARR(T) *) xmalloc(sizeof(VARR(T)));			       \
    va->els_num = 0; va->size = size_;				       	       \
    va->varr = (T *) xmalloc(size_ * sizeof(T));			       \
}                                                                              \
                                                                               \
static inline void VARR_OP(T, destroy)(VARR(T) **varr_) {	               \
    VARR(T) *va = *varr_;						       \
    VARR_ASSERT(va && va->varr, "destroy", T);				       \
    free(va->varr);							       \
    free(va); 								       \
    *varr_ = NULL;							       \
}									       \
                                                                               \
static inline unsigned VARR_OP(T, length)(const VARR(T) *varr_) {	       \
    VARR_ASSERT(varr_, "length", T);					       \
    return varr_->els_num;						       \
}                                                                              \
                                                                               \
static inline T *VARR_OP(T, addr)(const VARR(T) *varr_) {	               \
    VARR_ASSERT(varr_, "addr", T);					       \
    return &varr_->varr[0];						       \
}                                                                              \
                                                                               \
static inline T VARR_OP(T, last)(const VARR(T) *varr_) {                       \
    VARR_ASSERT(varr_ && varr_->varr && varr_->els_num, "last", T);	       \
    return varr_->varr[varr_->els_num - 1];				       \
}                                                                              \
                                                                               \
static inline T VARR_OP(T, get)(const VARR(T) *varr_, unsigned ix_) { 	       \
    VARR_ASSERT(varr_ && varr_->varr && ix_ < varr_->els_num, "get", T);       \
    return varr_->varr[ix_];						       \
}                                                                              \
                                                                               \
static inline T VARR_OP(T, set)(const VARR(T) *varr_, unsigned ix_, T obj_) {  \
    T old_obj_;								       \
    VARR_ASSERT(varr_ && varr_->varr && ix_ < varr_->els_num, "set", T);       \
    old_obj_ = varr_->varr[ix_];					       \
    varr_->varr[ix_] = obj_;						       \
    return old_obj_;							       \
}                                                                              \
                                                                               \
static inline void VARR_OP(T,trunc)(VARR(T) *varr_, unsigned size_) {          \
    VARR_ASSERT(varr_ && varr_->varr && varr_->els_num >= size_, "trunc", T);  \
    varr_->els_num = size_;						       \
}									       \
                                                                               \
static inline void VARR_OP(T,expand)(VARR(T) *varr_, unsigned size_) {	       \
    VARR_ASSERT(varr_  && varr_->varr, "expand", T);			       \
    if (varr_->size < size_) {						       \
	varr_->varr = (T *) xrealloc(varr_->varr, sizeof(T) * 3 * size_ / 2);  \
    }                                                                          \
    varr_->size = size_;						       \
}									       \
									       \
static inline void VARR_OP(T, push)(VARR(T) *varr_, T obj_) {	               \
    T *slot_;								       \
    VARR_OP(T, expand)(varr_, varr_->els_num + 1);			       \
    slot_ = &varr_->varr[varr_->els_num++];				       \
    *slot_ = obj_;							       \
}                                                                              \
                                                                               \
static inline T VARR_OP(T, pop)(VARR(T) *varr_) {			       \
    T obj_;								       \
    VARR_ASSERT(varr_ && varr_->varr && varr_->els_num, "pop", T);	       \
    obj_ = varr_->varr[--varr_->els_num];				       \
    return obj_;							       \
}

/* Macros implementing operations for VARR V of elements of type T: */
#define VARR_CREATE(T, V, L) (VARR_OP(T, create)(&(V), L))
#define VARR_DESTROY(T, V) (VARR_OP(T, destroy)(&(V)))
#define VARR_LENGTH(T, V) (VARR_OP(T, length)(V))
#define VARR_ADDR(T, V) (VARR_OP(T, addr)(V))
#define VARR_LAST(T, V) (VARR_OP(T, last)(V))
#define VARR_GET(T, V, I) (VARR_OP(T, get)(V, I))
#define VARR_SET(T, V, I, O) (VARR_OP(T, set)(V, I, O))
#define VARR_TRUNC(T, V, S) (VARR_OP(T, trunc)(V, S))
#define VARR_EXPAND(T, V, S) (VARR_OP(T, expand)(V, S))
#define VARR_PUSH(T, V, O) (VARR_OP(T, push)(V, O))
#define VARR_POP(T, V) (VARR_OP(T, pop)(V))

/* Definition of VARR of size_t elements.  */
DEF_VARR(size_t);

/* A stack of label positions in stack insn sequence. */
static VARR(size_t) *label_pos_stack;
/* Map: position in stack insn sequence -> index of first free slot in
   emulated VM stack.  */
static VARR(size_t) *pos_stack_free;

/* Definition of VARR of char elements.  */
DEF_VARR(char);

/* Label types: */
#define NO_LABEL 0
#define UNREACHED_LABEL 1 /* for position right after JUMP.  */
#define CONT_LABEL 2      /* Continuation label from the catch table  */
#define BRANCH_LABEL 3    /* Label from conditional branches */

/* Map: position in stack insn sequence -> type of label at given
   position.  */
static VARR(char) *pos_label_type;
/* Map: position in stack insn sequence -> flag of that we already
   processed an insn at the position during first pass of processing
   stack insns.  */
static VARR(char) *pos_processed_p;
/* Map: position in stack insn sequence -> flag of that the position is
   present in the catch table as a bound of the exception region. */
static VARR(char) *catch_bound_pos_p;

/* Process a new LABEL of TYPE with stack DEPTH at the label.  */
static void
process_label(int type, size_t label, size_t depth) {
    int prev_type = VARR_ADDR(char, pos_label_type)[label];

#if RTL_GEN_DEBUG
    fprintf(stderr, "Label %lu, type=%d, depth=%d\n", label, type, depth);
#endif
    assert(type != NO_LABEL);
    if (prev_type < type) {
	VARR_ADDR(char, pos_label_type)[label] = type;
	assert(prev_type != BRANCH_LABEL || type != CONT_LABEL
	       || VARR_ADDR(size_t, pos_stack_free)[label] == depth + 1);
	VARR_ADDR(size_t, pos_stack_free)[label] = depth + 1;
#if RTL_GEN_DEBUG
	fprintf(stderr, " Set up Label %lu\n", label);
#endif
	if (! VARR_ADDR(char, pos_processed_p)[label]) {
	    VARR_PUSH(size_t, label_pos_stack, label);
#if RTL_GEN_DEBUG
	    fprintf(stderr, " Push Label %lu\n", label);
#endif
	}
    }
}

/* Argument for mark_labe_from_hash.  */
struct label_arg {
    REL_PC incr;  /* base for pc relative label value */ 
    size_t depth; /* Stack depth at the label */
};

/* Process a label given by value VAL whose additional characteristics
   are in ARG.  The label is from opt_case_dispatch hash.  Return
   ST_CONTINUE to process other labels from the hash.  */
static int
mark_label_from_hash(VALUE key, VALUE val, VALUE arg) {
    struct label_arg *label_arg = (struct label_arg *) arg;
    
    process_label(BRANCH_LABEL, FIX2INT(val) + label_arg->incr, label_arg->depth);
    return ST_CONTINUE;
}

/* Process continuation labels from ISEQ catch table.  Set up
   CATCH_BOUND_POS_P too.  */
static void
setup_labels_from_catch_table(rb_iseq_t *iseq) {
    size_t i, size;
    const struct iseq_catch_table *table;
    const struct iseq_catch_table_entry *entries;
    char *bound_addr;

    VARR_TRUNC(char, catch_bound_pos_p, 0);
    size = iseq->body->iseq_size;
    for (i = 0; i < size; i++) {
	VARR_PUSH(char, catch_bound_pos_p, FALSE);
    }
    table = iseq->body->catch_table;
    if (table == NULL)
	return;
    size = table->size;
    entries = table->entries;
    bound_addr = VARR_ADDR(char, catch_bound_pos_p);
    for (i = 0; i < size; i++) {
	bound_addr[entries[i].start] = TRUE;
	bound_addr[entries[i].end] = TRUE;
#if RTL_GEN_DEBUG
	fprintf(stderr, "start=%d, end=%d", entries[i].start, entries[i].end);
	fprintf(stderr, "CATCH_TYPE=%d, ", entries[i].type);
#endif
	/* See hack for these catch types in compile.c.  */
	process_label(CONT_LABEL, entries[i].cont,
		      entries[i].sp + (entries[i].type == CATCH_TYPE_RESCUE
				       || entries[i].type == CATCH_TYPE_NEXT
				       || entries[i].type == CATCH_TYPE_BREAK));
    }
}

/* Remove already processed labels from LABEL_POS_STACK.  It can
   happen when we achieve a label by fall through path.  */
static void
update_label_pos_stack(void) {
    size_t i, len, curr, bound, pos;
    size_t *label_pos_stack_addr;
    char *pos_label_type_addr = VARR_ADDR(char, pos_label_type);

    len = VARR_LENGTH(size_t, label_pos_stack);
    if (len == 0)
	return;
    label_pos_stack_addr = VARR_ADDR(size_t, label_pos_stack);
    bound = 0;
    for (curr = 0; curr < len; curr++) {
	pos = label_pos_stack_addr[curr];
	if (VARR_ADDR(char, pos_processed_p)[pos])
	    continue;
	for (i = bound; i > 0; i--) {
	    if (pos_label_type_addr[label_pos_stack_addr[i - 1]] <= pos_label_type_addr[pos])
		break;
	    label_pos_stack_addr[i] = label_pos_stack_addr[i - 1];
	}
	label_pos_stack_addr[i] = pos;
	bound++;
    }
    VARR_TRUNC(size_t, label_pos_stack, bound);
#if RTL_GEN_DEBUG
    fprintf(stderr, "Stack");
    for (i = 0; i < bound; i++) {
	pos = label_pos_stack_addr[i];
	fprintf(stderr, " %lu:t%d", pos, pos_label_type_addr[pos]);
    }
    fprintf(stderr, "\n");
#endif
}

/* Calculate the emulated VM stack depth at each position in stack insn
   sequence.  */
static void
calculate_stack_depth(rb_iseq_t *iseq) {
    const VALUE *code = iseq->body->iseq_encoded;
    size_t size = iseq->body->iseq_size;
    VALUE insn;
    size_t pos, stack_insn_len, depth;
    int type;
    rb_num_t last_adjust_stack;
    
#if RTL_GEN_DEBUG
    for (pos = 0; pos < size; pos += insn_len(code[pos]))
	rb_iseq_disasm_insn(0, code, pos, iseq, 0);
#endif
    VARR_TRUNC(size_t, pos_stack_free, 0);
    VARR_TRUNC(size_t, label_pos_stack, 0);
    for (pos = 0; pos < size; pos++) {
	VARR_PUSH(size_t, pos_stack_free, 0); /* undefined */
    }
    VARR_TRUNC(char, pos_label_type, 0);
    VARR_TRUNC(char, pos_processed_p, 0);
    for (pos = 0; pos < size; pos++) {
	VARR_PUSH(char, pos_label_type, NO_LABEL);
	VARR_PUSH(char, pos_processed_p, FALSE);
    }
    setup_labels_from_catch_table(iseq);
    pos = 0;
    depth = 0;
    type = BRANCH_LABEL;
    while (TRUE) {
#if RTL_GEN_DEBUG
	{
	    size_t i;
	    
	    fprintf(stderr, "Start at %lu(%d) stack=", pos, type, VARR_LENGTH(size_t, label_pos_stack));
	    for (i = 0; i < VARR_LENGTH(size_t, label_pos_stack); i++) {
		size_t p = VARR_ADDR(size_t, label_pos_stack)[i];
		fprintf(stderr, " %lu(%d)", p, VARR_ADDR(char, pos_label_type)[p]);
	    }
	    fprintf(stderr, "\n");
	}
#endif
	last_adjust_stack = 0;
	for (; pos < size;) {
	    if (VARR_ADDR(char, pos_processed_p)[pos]) {
		assert(type == UNREACHED_LABEL || depth + 1 == VARR_ADDR(size_t, pos_stack_free)[pos]);
		break;
	    }
	    VARR_ADDR(char, pos_processed_p)[pos] = TRUE;
	    VARR_ADDR(size_t, pos_stack_free)[pos] = depth + 1;
	    insn = code[pos];
#if RTL_GEN_DEBUG
	    fprintf(stderr, "+%s=%lu (%lu)\n", insn_name(insn), depth, pos);
#endif
	    stack_insn_len = insn_len(insn);
	    depth = insn_stack_increase(depth, insn, TRUE, &code[pos + 1]);
	    switch (insn) {
	    case BIN(branchif):
	    case BIN(branchunless):
	    case BIN(branchnil):
	    case BIN(getinlinecache):
		last_adjust_stack = 0;
	    case BIN(jump): {
		process_label(BRANCH_LABEL, code[pos + 1] + pos + stack_insn_len, depth);
		break;
	    }
	    case BIN(adjuststack): {
		last_adjust_stack = code[pos + 1];
		break;
	    }
	    case BIN(opt_case_dispatch): {
		CDHASH hash = code[pos + 1];
		REL_PC incr = pos + stack_insn_len;
		struct label_arg arg;
		
		arg.incr = incr;
		arg.depth = depth;
		rb_hash_foreach(hash, mark_label_from_hash, (VALUE) &arg);
		process_label(BRANCH_LABEL, code[pos + 2] + incr, depth); /* else label */
		last_adjust_stack = 0;
		break;
	    }
	    default:
		break;
	    }
	    if (insn == BIN(jump) || insn == BIN(leave) || insn == BIN(throw)) {
		if (insn == BIN(leave))
		    depth--;
		else if (insn == BIN(jump)
			 && code[code[pos + 1] + pos + stack_insn_len] == BIN(pop))
		    depth--;
		if (pos + stack_insn_len < size)
		    process_label(UNREACHED_LABEL, pos + stack_insn_len, depth + last_adjust_stack);
		break;
	    }
	    pos += stack_insn_len;
	}
	update_label_pos_stack();
	if (VARR_LENGTH(size_t, label_pos_stack) == 0)
	    break;
	pos = VARR_POP(size_t, label_pos_stack);
	type = VARR_ADDR(char, pos_label_type)[pos];
	depth = VARR_ADDR(size_t, pos_stack_free)[pos];
	assert(depth > 0);
	depth--;
    }
}

/* Map: position of stack insn -> position of corresponding RTL
   insns.  */
static VARR(size_t) *new_insn_offsets;

/* Location of a label param in RTL insn sequence.  */
struct branch_target_loc {
    /* Position the next RTL insn.  */
    size_t next_insn_pc;
    /* Offset the label parameter relative to the next RTL insn
       position.  */
    REL_PC offset;
};

typedef struct branch_target_loc branch_target_loc;

/* Definition of VARR of elements with type branch_target_loc.  */
DEF_VARR(branch_target_loc);

/* Locations of label params in RTL insns.  We need to keep this to
   modify labels in generated RTL insns.  */
static VARR(branch_target_loc) *branch_target_locs;

/* Type of slot of the emulated VM stack.  */
enum slot_type {SELF, VAL, STR, LOC, TEMP};

/* Stack slot for emulated VM stack. */
struct stack_slot {
    enum slot_type mode;
    union {
	VALUE val;     /* value */
	VALUE str;     /* string */
	vindex_t loc;  /* local var */
	vindex_t temp; /* temp (or VM stack) var */
    } u;
};

typedef struct stack_slot stack_slot;

/* Definition of VARR of elements with type stack_slot.  */
DEF_VARR(stack_slot);

/* The emulated VM stack.  */
static VARR(stack_slot) *stack;

/* Map: var location index -> the current number of stack slots with
   given location in the emulated VM stack.  We need this map to
   process complicated stack insns generated for a multiple
   assignment.  In this case we might need temporary variables to
   implement the assignemnt. */
static VARR(size_t) *loc_stack_count;

/* Definition of VARR of elements with type VALUE.  */
DEF_VARR(VALUE);

/* A sequence of generated RTL insns.  */
static VARR(VALUE) *iseq_rtl;

/* Append ARGC values to the RTL insn sequence.  */
static void
append_vals(int argc, ...) {
    va_list argv;

    if (argc > 0) {
	int i;
	
	va_start(argv, argc);
	for (i = 0; i < argc; i++) {
	    VALUE v = va_arg(argv, VALUE);
	    VARR_PUSH(VALUE, iseq_rtl, v);
	}
	va_end(argv);
    }
}

/* Auxiliary append macros.  */
#define APPEND1(op1) append_vals(1, (VALUE) (op1))
#define APPEND2(op1, op2) append_vals(2, (VALUE) (op1), (VALUE) (op2))
#define APPEND3(op1, op2, op3) \
    append_vals(3, (VALUE) (op1), (VALUE) (op2), (VALUE) (op3))
#define APPEND4(op1, op2, op3, op4) \
    append_vals(4, (VALUE) (op1), (VALUE) (op2), (VALUE) (op3), (VALUE) (op4))
#define APPEND5(op1, op2, op3, op4, op5) \
    append_vals(5, (VALUE) (op1), (VALUE) (op2), (VALUE) (op3), (VALUE) (op4), (VALUE) (op5))
#define APPEND6(op1, op2, op3, op4, op5, op6) \
    append_vals(6, (VALUE) (op1), (VALUE) (op2), (VALUE) (op3), (VALUE) (op4), (VALUE) (op5), (VALUE) (op6))
#define APPEND7(op1, op2, op3, op4, op5, op6, op7) \
    append_vals(7, (VALUE) (op1), (VALUE) (op2), (VALUE) (op3), (VALUE) (op4), (VALUE) (op5), (VALUE) (op6), (VALUE) (op7))

/* Max stack depth of the emulated VM stack.  */
static size_t max_stack_depth;

/* Pop and return a slot from the emulated VM stack.  */
static stack_slot
pop_stack_slot(void) {
    stack_slot slot;
    
    slot = VARR_POP(stack_slot, stack);
    if (slot.mode == LOC) {
	assert(VARR_ADDR(size_t, loc_stack_count)[slot.u.loc] > 0);
	VARR_ADDR(size_t, loc_stack_count)[slot.u.loc]--;
    }
    return slot;
}

/* Update loc_stack_count for a local variable in SLOT.  */
static void
prepare_loc_var_push(stack_slot *slot) {
    if (slot->mode != LOC)
	return;
    VARR_ADDR(size_t, loc_stack_count)[slot->u.loc]++;
}

/* Push SLOT to the emulated VM stack.  */
static void
push_stack_slot(stack_slot slot) {
    size_t len;
    vindex_t op;
    
    len = VARR_LENGTH(stack_slot, stack) + 1;
    op = -(vindex_t) len;
    if (slot.mode == TEMP && slot.u.temp != op) {
	
	APPEND3(BIN(temp2temp), op, slot.u.temp);
	slot.u.temp = op;
    } else if (slot.mode == LOC) {
	prepare_loc_var_push(&slot);
    }
    VARR_PUSH(stack_slot, stack, slot);
    if (max_stack_depth < len)
	max_stack_depth = len;
}

/* Push slot describing top VM stack temporary var.  Return the
   temporary var index.  */
static vindex_t
new_top_stack_temp_var(void) {
    stack_slot slot;
    vindex_t res;
    
    slot.mode = TEMP;
    res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
    slot.u.temp = res;
    push_stack_slot(slot);
    return res;
}

/* Generate (zero or more) RTL insns to move value described by stack
   SLOT to a temporary or local var.  TOP is index of the emulated
   stack slot.  Return the index temporary or local var where the
   value of SLOT will be after that.  */
static vindex_t
to_var(stack_slot slot, vindex_t top) {
    if (slot.mode == TEMP) {
	return slot.u.temp;
    } else if (slot.mode == LOC) {
	return slot.u.loc;
    } else if (slot.mode == SELF) {
	APPEND2(BIN(self2var), top);
    } else if (slot.mode == VAL) {
	APPEND3(BIN(val2temp), top, slot.u.val);
    } else {
	assert(slot.mode == STR);
	APPEND3(BIN(str2var), top, slot.u.str);
    }
    return top;
}

/* Update loc_stack_count for local var described by SLOT.  */
static void
prepare_stack_rewrite(stack_slot *slot) {
    if (slot->mode == LOC) {
	assert(VARR_ADDR(size_t, loc_stack_count)[slot->u.loc] > 0);
	VARR_ADDR(size_t, loc_stack_count)[slot->u.loc]--;
    }
}

/* Generate (zero or more) RTL insns to move value described by SLOT
   to a temporary var with index RES.  STACK_P is TRUE if the slot is
   in the emulated VM stack.  */
static void
to_temp(stack_slot *slot, vindex_t res, int stack_p) {
    if (slot->mode == TEMP) {
	if(slot->u.temp != res)
	    APPEND3(BIN(temp2temp), res, slot->u.temp);
    } else if (slot->mode == LOC) {
	APPEND3(BIN(loc2temp), res, slot->u.loc);
	if (stack_p) {
	    prepare_stack_rewrite(slot);
	}
    } else if (slot->mode == SELF) {
	APPEND2(BIN(self2var), res);
    } else if (slot->mode == VAL) {
	APPEND3(BIN(val2temp), res, slot->u.val);
    } else {
	assert(slot->mode == STR);
	APPEND3(BIN(str2var), res, slot->u.str);
    }
    slot->mode = TEMP;
    slot->u.temp = res;
}

/* We are going to assign a value to local var with index RES.  Check
   there is no slot with such var on the emulated VM stack.  Generate
   RTL insns to move the var value to temp vars in the opposite case.
   Update the stack slots of the emulated VM stack.  */
static void
prepare_local_assign(vindex_t res) {
    size_t i, len = VARR_LENGTH(stack_slot, stack);
    stack_slot *curr_slot;
    
    assert(res > 0);
    if (VARR_ADDR(size_t, loc_stack_count)[res] == 0)
	return;
    for (i = 0; i < len; i++) {
	curr_slot = &VARR_ADDR(stack_slot, stack)[i];
	if (curr_slot->mode == LOC) {
	    to_temp(curr_slot, -(vindex_t) i - 1, TRUE);
	}
    }
    assert(VARR_ADDR(size_t, loc_stack_count)[res] == 0);
}

/* Push slot describing local or temp var with index RES to the
   emulated VM stack.  */
static void
push_result(vindex_t res) {
    stack_slot slot;
    
    if (res < 0) {
	slot.mode = TEMP;
	slot.u.temp = res;
    } else {
	prepare_local_assign(res);
	slot.mode = LOC;
	slot.u.loc = res;
    }
    push_stack_slot(slot);
}

/* Pop slot from the emulated VM stack.  Generate RTL insns to place
   the correponding value in temporary var if the value is not there
   or in local vars.  Return index of the local or temporary
   variable. */
static vindex_t
get_var(void) {
    stack_slot slot;
    
    slot = pop_stack_slot();
    return to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
}

/* As get_var but for two top slots. Return indexes of the local or
   temporary variables through OP1 and OP2. */
static void
get_2vars(vindex_t *op1, vindex_t *op2) {
    stack_slot slot;
    
    slot = pop_stack_slot();
    *op2 = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
    slot = pop_stack_slot();
    *op1 = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
}

/* Generate (zero or more) RTL insns for stack insn getlocal with args
   IDX and LEVEL.  */
static void
get_local(lindex_t idx, rb_num_t level) {
    stack_slot slot;
    
    if (level == 0) {
	slot.mode = LOC;
	slot.u.loc = idx;
	push_stack_slot(slot);
    } else {
	vindex_t res = new_top_stack_temp_var();
	
	APPEND4(BIN(uploc2temp), res, idx, level);
    }
}

/* Generate (one or more) RTL insns for stack insn setlocal with args
   IDX and LEVEL.  */
static void
set_local(lindex_t idx, rb_num_t level) {
    stack_slot slot;

    slot = pop_stack_slot();
    if (level == 0)
	prepare_local_assign(idx);
    if (slot.mode == SELF) {
	if (level == 0)
	    APPEND2(BIN(self2var), idx);
	else {
	    vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	    
	    APPEND2(BIN(self2var), op);
	    APPEND4(BIN(var2uploc), idx, op, level);
	}
    } else if (slot.mode == VAL) {
	if (level == 0)
	    APPEND3(BIN(val2loc), idx, slot.u.val);
	else
	    APPEND4(BIN(val2uploc), idx, slot.u.val, level);
    } else if (slot.mode == STR) {
	if (level == 0) 
	    APPEND3(BIN(str2var), idx, slot.u.str);
	else {
	    vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	    
	    APPEND3(BIN(str2var), op, slot.u.str);
	    APPEND4(BIN(var2uploc), idx, op, level);
	}
    } else if (slot.mode == TEMP) {
	if (level == 0)
	    APPEND3(BIN(temp2loc), idx, slot.u.temp);
	else
	    APPEND4(BIN(var2uploc), idx, slot.u.temp, level);
    } else {
	assert(slot.mode == LOC);
	if (level == 0)
	    APPEND3(BIN(loc2loc), idx, slot.u.loc);
	else
	    APPEND4(BIN(var2uploc), idx, slot.u.loc, level);
    }
}

/* Update the emulated VM stack for for stack insn putobject with arg
   V.  */
static void
putobject(VALUE v) {
    stack_slot slot;

    slot.mode = VAL;
    slot.u.val = v;
    push_stack_slot(slot);
}

/* Generate RTL insns to put values on the VM stack.  The values are
   described by ARGS_NUM top slots on the emulated VM stack.  */
static void
put_on_stack(vindex_t args_num) {
    vindex_t op, i;
    vindex_t len = VARR_LENGTH(stack_slot, stack);
    
    assert(args_num <= len);
    for (i = len - args_num; i < len; i++) {
	op = -i - 1;
	to_temp(&VARR_ADDR(stack_slot, stack)[i], op, TRUE);
    }
}

/* As above plus truncate the emulated VM stack correspondingly.  */
static void
put_args_on_stack(vindex_t args_num) {
    vindex_t len = VARR_LENGTH(stack_slot, stack);

    put_on_stack(args_num);
    VARR_TRUNC(stack_slot, stack, len - args_num);
}

/* put_on_stack for one top value if the emulated VM stack is not
   empty.  */
static void
put_top_on_stack(void) {
    if (VARR_LENGTH(stack_slot, stack) >= 1)
	put_on_stack(1);
}

/* Generate RTL insn RES_INSN for special load stack insn with ARGS.
   ARG2_P is true if the stack insn has two args.  */
static void
specialized_load(enum ruby_vminsn_type res_insn, const VALUE *args, int arg2_p) {
    vindex_t res = new_top_stack_temp_var();
    
    if (arg2_p)
	APPEND4(res_insn, res, args[0], args[1]);
    else
	APPEND3(res_insn, res, args[0]);
}

/* Generate (one or more) RTL insns for special store stack insn with
   ARGS.  The corresponding RTL insn which actuall does a store has
   code RES_INSN.  */
static void
specialized_store(enum ruby_vminsn_type res_insn, const VALUE *args) {
    stack_slot slot;
    vindex_t op;
    
    slot = pop_stack_slot();
    op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
    op = to_var(slot, op);
    APPEND3(res_insn, args[0], op);
}

/* Return call data without kwarg corresponding to call info CI of
   ISEQ.  Return NULL if CI is not for a call without kwarg.  */
static struct rb_call_data *
get_cd_data(rb_iseq_t *iseq, CALL_INFO ci) {
    size_t ci_size = iseq->body->ci_size;
    struct rb_call_info *ci_entries = iseq->body->ci_entries;
    
    if (ci < ci_entries || (ci_entries + ci_size) <= ci)
	return NULL;
    return &iseq->body->cd_entries[ci - ci_entries];
}

/* Return call data with kwarg corresponding to call info CI of ISEQ.
   Return NULL if CI is not for call with kwarg.  */
static struct rb_call_data_with_kwarg *
get_cd_data_with_kw_arg(rb_iseq_t *iseq, CALL_INFO ci) {
    struct rb_call_info_with_kwarg * cikw = (struct rb_call_info_with_kwarg *) ci;
    size_t ci_size = iseq->body->ci_size;
    size_t cikw_size = iseq->body->ci_kw_size;
    struct rb_call_info_with_kwarg *cikw_entries
	= (struct rb_call_info_with_kwarg *) (iseq->body->ci_entries + ci_size);
    struct rb_call_data_with_kwarg *cdkw_entries
	= (struct rb_call_data_with_kwarg *) (iseq->body->cd_entries + ci_size);
    
    if (cikw < cikw_entries || (cikw_entries + cikw_size) <= cikw)
	return NULL;
    return &cdkw_entries[cikw - cikw_entries];
}

/* Return call data corresponding to call info CI of ISEQ.  */
static struct rb_call_data *
get_cd(rb_iseq_t *iseq, CALL_INFO ci, CALL_CACHE cc, vindex_t call_start) {
    struct rb_call_data *cd;
    struct rb_call_data_with_kwarg *cdkw;

    if ((cd = get_cd_data(iseq, ci)) != NULL) {
	cd->call_info = *ci;
	if (cc != NULL)
	    cd->call_cache = *cc;
	cd->call_start = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	return cd;
    }
    if ((cdkw = get_cd_data_with_kw_arg(iseq, ci)) != NULL) {
	cdkw->call_info = *ci;
	if (cc != NULL)
	    cdkw->call_cache = *cc;
	cdkw->call_start = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	cdkw->kw_arg = ((struct rb_call_info_with_kwarg *) ci)->kw_arg;
	return (struct rb_call_data *)cdkw;
    }
    assert(FALSE);
}

/* Generate RTL insns from ISEQ stack insn call whose operands are in
   ARGS.  BLOCK is a block in the call.  Zero means no block in the
   call.  */
static void
generate_call(rb_iseq_t *iseq, const VALUE *args, VALUE block) {
    CALL_INFO ci = (CALL_INFO) args[0];
    CALL_CACHE cc = (CALL_CACHE) args[1];
    struct rb_call_data *cd;
    vindex_t args_num;
    stack_slot slot;
    int stack_block_p = ci->flag & VM_CALL_ARGS_BLOCKARG;
    
    args_num = ci->orig_argc + (stack_block_p ? 1: 0);
    put_args_on_stack(args_num);
    slot = pop_stack_slot();
    cd = get_cd(iseq, ci, cc, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
    if (slot.mode == SELF) {
	if (block == 0 && ! stack_block_p)
	    APPEND3(BIN(simple_call_self), cd, cd->call_start);
	else
	    APPEND4(BIN(call_self), cd, cd->call_start, block);
    } else {
	to_temp(&slot, cd->call_start, FALSE);
	if (block == 0 && ! stack_block_p)
	    APPEND3(BIN(simple_call), cd, cd->call_start);
	else
	    APPEND4(BIN(call), cd, cd->call_start, block);
    }
    new_top_stack_temp_var();
}

/* Generate RTL insns from an ISEQ unary operator insn whose operands
   are in ARGS.  The corresponding RTL insn code is RES_INSN. */
static void
generate_unary_op(rb_iseq_t *iseq, const VALUE *args, enum ruby_vminsn_type res_insn) {
    CALL_INFO ci = (CALL_INFO) args[0];
    CALL_CACHE cc = (CALL_CACHE) args[1];
    struct rb_call_data *cd;
    vindex_t op, res;
    stack_slot slot;
    
    slot = pop_stack_slot();
    cd = get_cd(iseq, ci, cc, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
    if (slot.mode == SELF) {
	op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	APPEND2(BIN(self2var), op);
    } else if (slot.mode == VAL) {
	op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	APPEND3(BIN(val2temp), op, slot.u.val);
    } else if (slot.mode == STR) {
	op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	APPEND3(BIN(str2var), op, slot.u.str);
    } else {
	assert(slot.mode == LOC || slot.mode == TEMP);
	op = slot.mode == LOC ? slot.u.loc : slot.u.temp;
    }
    res = new_top_stack_temp_var();
    APPEND5(res_insn, BIN(cont_op1), cd, res, op);
}

/* Return a variant of insn INSN_ID with an immediate operand (fixnum
   if FIXNUM_P or flonum if FLONUM_P or string otherwise).  Return NOP
   if there is no such insn.  */
static enum ruby_vminsn_type
make_imm_id(enum ruby_vminsn_type insn_id, int fixnum_p, int flonum_p)
{
    if (!fixnum_p && !flonum_p)
	return insn_id == BIN(ind) ? BIN(inds) : insn_id == BIN(ind) ? BIN(indsets): BIN(nop);
    assert(fixnum_p && !flonum_p || !fixnum_p && flonum_p);
    switch (insn_id) {
    case BIN(plus): return fixnum_p ? BIN(plusi) : BIN(plusf);
    case BIN(minus): return fixnum_p ? BIN(minusi) : BIN(minusf);
    case BIN(mult): return fixnum_p ? BIN(multi) : BIN(multf);
    case BIN(eq): return fixnum_p ? BIN(eqi) : BIN(eqf);
    case BIN(ne): return fixnum_p ? BIN(nei) : BIN(nef);
    case BIN(lt): return fixnum_p ? BIN(lti) : BIN(ltf);
    case BIN(gt): return fixnum_p ? BIN(gti) : BIN(gtf);
    case BIN(le): return fixnum_p ? BIN(lei) : BIN(lef);
    case BIN(ge): return fixnum_p ? BIN(gei) : BIN(gef);
    case BIN(div): return fixnum_p ? BIN(divi) : BIN(divf);
    case BIN(mod): return fixnum_p ? BIN(modi) : BIN(modf);
    case BIN(ltlt): return fixnum_p ? BIN(ltlti) : BIN(nop);
    case BIN(ind): return fixnum_p ? BIN(indi) : BIN(nop);
    case BIN(indset): return fixnum_p ? BIN(indseti) : BIN(nop);
    default: return BIN(nop);
    }
}

/* Generate RTL insns for operands of RTL insn RES_INSN from an ISEQ
   binary operator insn whose operands are in ARGS.  Return RTL insn
   operands and call data through RES, OP, OP2, and CD.  Return RTL
   insn code which will be actually analog of the stack insn.  */
static enum ruby_vminsn_type
get_binary_ops(rb_iseq_t *iseq, enum ruby_vminsn_type res_insn, const VALUE *args,
	       vindex_t *res, vindex_t *op, VALUE *op2, struct rb_call_data **cd) {
    CALL_INFO ci = (CALL_INFO) args[0];
    CALL_CACHE cc = (CALL_CACHE) args[1];
    enum ruby_vminsn_type imm_insn;
    stack_slot slot, slot2;
    
    slot2 = pop_stack_slot();
    slot = pop_stack_slot();
    *res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
    *cd = get_cd(iseq, ci, cc, *res);
    *op = to_var(slot, *res);
    if (slot2.mode == SELF) {
	*op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	APPEND2(BIN(self2var), *op2);
    } else if (slot2.mode == VAL) {
	imm_insn = BIN(nop);
	if (FIXNUM_P(slot2.u.val))
	    imm_insn = make_imm_id(res_insn, TRUE, FALSE);
	else if (FLONUM_P(slot2.u.val))
	    imm_insn = make_imm_id(res_insn, FALSE, TRUE); 
	if (imm_insn != BIN(nop)) {
	    *op2 = slot2.u.val;
	    res_insn = imm_insn;
	} else {
	    *op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	    APPEND3(BIN(val2temp), *op2, slot2.u.val);
	}
    } else if (slot2.mode == STR) {
	imm_insn = make_imm_id(res_insn, FALSE, FALSE);
	if (imm_insn != BIN(nop)) {
	    *op2 = slot2.u.str;
	    res_insn = imm_insn;
	} else {
	    *op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	    APPEND3(BIN(str2var), *op2, slot2.u.str);
	}
    } else {
	assert(slot2.mode == LOC || slot2.mode == TEMP);
	*op2 = slot2.mode == LOC ? slot2.u.loc : slot2.u.temp;
    }
    return res_insn;
}

/* Generate RTL insns from an ISEQ binary operator insn whose operands
   are in ARGS.  The corresponding RTL insn code is RES_INSN. */
static void
generate_bin_op(rb_iseq_t *iseq, const VALUE *args, enum ruby_vminsn_type res_insn) {
    struct rb_call_data *cd;
    vindex_t op, res;
    VALUE op2;
    
    res_insn = get_binary_ops(iseq, res_insn, args, &res, &op, &op2, &cd);
    push_result(res);
    APPEND6(res_insn, BIN(cont_op2), cd, res, op, op2);
}

/* Return RTL compare branch insn.  The original RTL compare insn is
   CMP_INSN.  BT_P is true if we are combining with branch on
   true.  */
static enum ruby_vminsn_type
get_bcmp_insn(enum ruby_vminsn_type cmp_insn, int bt_p) {
    switch (cmp_insn) {
    case BIN(eq): return bt_p ? BIN(bteq): BIN(bfeq);
    case BIN(ne): return bt_p ? BIN(btne): BIN(bfne);
    case BIN(lt): return bt_p ? BIN(btlt): BIN(bflt);
    case BIN(gt): return bt_p ? BIN(btgt): BIN(bfgt);
    case BIN(le): return bt_p ? BIN(btle): BIN(bfle);
    case BIN(ge): return bt_p ? BIN(btge): BIN(bfge);
    case BIN(eqi): return bt_p ? BIN(bteqi): BIN(bfeqi);
    case BIN(nei): return bt_p ? BIN(btnei): BIN(bfnei);
    case BIN(lti): return bt_p ? BIN(btlti): BIN(bflti);
    case BIN(gti): return bt_p ? BIN(btgti): BIN(bfgti);
    case BIN(lei): return bt_p ? BIN(btlei): BIN(bflei);
    case BIN(gei): return bt_p ? BIN(btgei): BIN(bfgei);
    case BIN(eqf): return bt_p ? BIN(bteqf): BIN(bfeqf);
    case BIN(nef): return bt_p ? BIN(btnef): BIN(bfnef);
    case BIN(ltf): return bt_p ? BIN(btltf): BIN(bfltf);
    case BIN(gtf): return bt_p ? BIN(btgtf): BIN(bfgtf);
    case BIN(lef): return bt_p ? BIN(btlef): BIN(bflef);
    case BIN(gef): return bt_p ? BIN(btgef): BIN(bfgef);
    default:
	assert(FALSE);
    }
}

/* Generate RTL insns from ISEQ stack comparison insn in CODE at
   position POS.  The corresponding RTL comparison insn code is
   RES_INSN.  Combine with the next branch insn if possible.  Return
   position of the next stack insn should be processed after that.  */
static size_t
generate_rel_op(rb_iseq_t *iseq, const VALUE *code, size_t pos, enum ruby_vminsn_type res_insn) {
    int bt_p;
    size_t len = insn_len(code[pos]);
    const VALUE *args = &code[pos + 1];
    enum ruby_vminsn_type next_insn = code[pos + len];
    size_t next_insn_len = insn_len(next_insn);
    vindex_t op, res;
    VALUE op2;
    struct rb_call_data *cd;
    branch_target_loc loc;

    if (VARR_ADDR(char, pos_label_type)[pos] != NO_LABEL
	|| (next_insn != BIN(branchif) && next_insn != BIN(branchunless))) {
	generate_bin_op(iseq, args, res_insn);
	return len;
    }
    res_insn = get_binary_ops(iseq, res_insn, args, &res, &op, &op2, &cd);
    bt_p = next_insn == BIN(branchif);
    res_insn = get_bcmp_insn(res_insn, bt_p);
    put_top_on_stack();
    APPEND7(res_insn, bt_p ? BIN(cont_btcmp) : BIN(cont_bfcmp),
	    code[pos + len + 1] + pos + len + next_insn_len, cd, res, op, op2);
    loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
    loc.offset = 5;
    VARR_PUSH(branch_target_loc, branch_target_locs, loc);
    return len + next_insn_len;
}

/* Generate RTL insns from ISEQ stack insn opt_aref or opt_aref_with
   whose operands are in ARGS.  */
static void
generate_aset_op(rb_iseq_t *iseq, const VALUE *args) {
    CALL_INFO ci = (CALL_INFO) args[0];
    CALL_CACHE cc = (CALL_CACHE) args[1];
    struct rb_call_data *cd;
    vindex_t op, op3, res;
    VALUE op2;
    enum ruby_vminsn_type res_insn = BIN(indset);
    enum ruby_vminsn_type imm_insn;
    stack_slot slot, slot2, slot3;
    
    slot3 = pop_stack_slot();
    slot2 = pop_stack_slot();
    slot = pop_stack_slot();
    res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
    cd = get_cd(iseq, ci, cc, res);
    op = to_var(slot, res);
    if (slot2.mode == SELF) {
	op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	APPEND2(BIN(self2var), op2);
    } else if (slot2.mode == VAL) {
	imm_insn = BIN(nop);
	if (FIXNUM_P(slot2.u.val))
	    imm_insn = make_imm_id(res_insn, TRUE, FALSE);
	else if (FLONUM_P(slot2.u.val))
	    imm_insn = make_imm_id(res_insn, FALSE, TRUE); 
	if (imm_insn != BIN(nop)) {
	    op2 = slot2.u.val;
	    res_insn = imm_insn;
	} else {
	    op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	    APPEND3(BIN(val2temp), op2, slot2.u.val);
	}
    } else if (slot2.mode == STR) {
	imm_insn = make_imm_id(res_insn, FALSE, FALSE);
	if (imm_insn != BIN(nop)) {
	    op2 = slot2.u.str;
	    res_insn = imm_insn;
	} else {
	    op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 2;
	    APPEND3(BIN(str2var), op2, slot2.u.str);
	}
    } else {
	assert(slot2.mode == LOC || slot2.mode == TEMP);
	op2 = slot2.mode == LOC ? slot2.u.loc : slot2.u.temp;
    }
    op3 = to_var(slot3, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 3);
    APPEND5(res_insn, cd, op, op2, op3);
    push_stack_slot(slot3);
}

/* Info how to update label offsets in hash of opt_case_dispatch
   insn.  */
struct hash_label_transform_arg {
    VALUE hash;  /* hash from opt_case_dispatch insn  */
    int map_p;   /* if true use new_insn_offsets firstly */
    REL_PC decr; /* decrement the label offset by it secondly*/
};

/* Return updated label OFFSET using info in ARG.  */
static REL_PC
update_case_hash(REL_PC offset, struct hash_label_transform_arg *arg) {
    if (arg->map_p) {
	offset = VARR_ADDR(size_t, new_insn_offsets)[offset];
    }
    return offset - arg->decr;
}

/* Change label value VAL in HASH using info in ARG_PTR.  Return flag
   to continue updates of labels in the hash table.  */
static int
transform_hash_offset(VALUE key, VALUE val, void *arg_ptr) {
    struct hash_label_transform_arg *arg
	= (struct hash_label_transform_arg *) arg_ptr;

    rb_hash_aset(arg->hash, key, INT2FIX(update_case_hash(FIX2INT(val), arg)));
    return ST_CONTINUE;
}

/* Change label values in HASH using info in ARG.  */
static void
change_hash_values(VALUE hash, struct hash_label_transform_arg *arg) {
    rb_hash_foreach(hash, transform_hash_offset, (VALUE)arg);
    rb_hash_rehash(hash);
    OBJ_FREEZE(hash);
    RBASIC_CLEAR_CLASS(hash);
}

/* Adjust the emulated VM stack depth to a depth stored in
   POS_STACK_FREE for label position POS in stack insn sequence.  */
static void
adjust_stack(size_t pos) {
    size_t depth_on_label, slot_depth;
    stack_slot slot;
    
    if (VARR_ADDR(char, pos_label_type)[pos] == NO_LABEL)
	return;
    depth_on_label = VARR_ADDR(size_t, pos_stack_free)[pos];
    if (depth_on_label == 0)
	return; /* Unknown depth */
    depth_on_label--;
    slot_depth = VARR_LENGTH(stack_slot, stack);
    if (slot_depth != 0)
	put_on_stack(1);
    if (depth_on_label < slot_depth) {
	VARR_TRUNC(stack_slot, stack, depth_on_label);
    } else {
	for (; depth_on_label > slot_depth; slot_depth++) {
	    slot.mode = TEMP;
	    slot.u.temp = -(long) slot_depth - 1;
	    push_stack_slot(slot);
	}
    }
}

/* Generate an RTL ret insn from stack insn leave.  */
static void
generate_leave(void) {
    /* ??? Trace field */
    stack_slot slot;
    rb_num_t nf = 0;
    
    slot = pop_stack_slot();
    if (slot.mode == SELF) {
	vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	
	APPEND2(BIN(self2var), op);
	APPEND3(BIN(temp_ret), op, nf);
    } else if (slot.mode == VAL) {
	APPEND3(BIN(val_ret), slot.u.val, nf);
    } else if (slot.mode == STR) {
	vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	
	APPEND3(BIN(str2var), op, slot.u.str);
	APPEND3(BIN(temp_ret), op, nf);
    } else if (slot.mode == TEMP) {
	APPEND3(BIN(temp_ret), slot.u.temp, nf);
    } else {
	assert(slot.mode == LOC);
	APPEND3(BIN(loc_ret), slot.u.loc, nf);
    }
}

/* Generate (zero or more) RTL insns for stack insn of ISEQ at
   position POS in CODE.  The stack insn was not modified for direct
   threading so far.  Return position of the next stack insn should be
   processed after that.  */ 
static size_t
translate_stack_insn(rb_iseq_t *iseq, const VALUE *code, size_t pos) {
    VALUE insn;
    size_t i, stack_insn_len, rtl_insn_start;
    stack_slot slot;
    branch_target_loc loc;
    
    insn = code[pos];
    stack_insn_len = insn_len(insn);
    adjust_stack(pos);
    rtl_insn_start = VARR_LENGTH(VALUE, iseq_rtl);
    VARR_ADDR(size_t, new_insn_offsets)[pos] = rtl_insn_start;
#if RTL_GEN_DEBUG
    fprintf(stderr, "%s(%lu) - %lu\n", insn_name(insn), pos, (long unsigned) stack->els_num);
#endif
    switch (insn) {
    case BIN(getlocal):
	get_local(code[pos + 1], code[pos + 2]);
	break;
    case BIN(setlocal):
	set_local(code[pos + 1], code[pos + 2]);
	break;
    case BIN(getspecial):
	specialized_load(BIN(special2var), &code[pos + 1], TRUE);
	break;
    case BIN(setspecial):
	specialized_store(BIN(var2special), &code[pos + 1]);
	break;
    case BIN(getinstancevariable):
	specialized_load(BIN(ivar2var), &code[pos + 1], TRUE);
	break;
    case BIN(setinstancevariable): {
	vindex_t op;
	
	slot = pop_stack_slot();
	if (slot.mode == VAL) {
	    op = slot.u.val;
	    APPEND4(BIN(val2ivar), code[pos + 1], code[pos + 2], op);
	} else {
	    op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	    op = to_var(slot, op);
	    APPEND4(BIN(var2ivar), code[pos + 1], code[pos + 2], op);
	}
	break;
    }
    case BIN(getclassvariable):
	specialized_load(BIN(cvar2var), &code[pos + 1], FALSE);
	break;
    case BIN(setclassvariable):
	specialized_store(BIN(var2cvar), &code[pos + 1]);
	break;
    case BIN(getconstant): {
	vindex_t res, op;
	
	slot = pop_stack_slot();
	res = new_top_stack_temp_var();
	if (slot.mode == VAL && (slot.u.val == Qnil || slot.u.val == rb_cObject)) {
	    APPEND4(BIN(const_ld_val), code[pos + 1], res, slot.u.val);
	} else {
	    op = to_var(slot, res);
	    APPEND4(BIN(const2var), code[pos + 1], res, op);
	}
	break;
    }
    case BIN(setconstant): {
	vindex_t op1, op2;
	
	slot = pop_stack_slot();
	op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op2 = to_var(slot, op2);
	slot = pop_stack_slot();
	op1 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op1 = to_var(slot, op1);
	APPEND4(BIN(var2const), code[pos + 1], op1, op2);
	break;
    }
    case BIN(getglobal):
	specialized_load(BIN(global2var), &code[pos + 1], FALSE);
	break;
    case BIN(setglobal):
	specialized_store(BIN(var2global), &code[pos + 1]);
	break;
    case BIN(putnil):
	slot.mode = VAL;
	slot.u.val = Qnil;
	push_stack_slot(slot);
	break;
    case BIN(putself):
	slot.mode = SELF;
	push_stack_slot(slot);
	break;
    case BIN(putobject):
	putobject(code[pos + 1]);
	break;
    case BIN(putspecialobject):
	/* Fall through: */
    case BIN(putiseq): {
      	vindex_t res;
	
	res = new_top_stack_temp_var();
	APPEND3(insn == BIN(putspecialobject) ? BIN(specialobj2var) : BIN(iseq2var),
		res, code[pos + 1]);
	break;
    }
    case BIN(putstring):
	slot.mode = STR;
	slot.u.str = code[pos + 1];
	push_stack_slot(slot);
	break;
    case BIN(concatstrings): {
	rb_num_t cnt = code[pos + 1];
	vindex_t res;
	
	put_args_on_stack(cnt);
	res = new_top_stack_temp_var();
	APPEND3(BIN(concat_strings), res, cnt);
	break;
    }
    case BIN(tostring): {
	vindex_t op, res;

	op = get_var();
	res = new_top_stack_temp_var();
	APPEND3(BIN(to_string), res, op);
	break;
    }
    case BIN(freezestring): {
	rb_num_t debug_info = code[pos + 1];
	vindex_t str_op;
	
	slot = pop_stack_slot();
	str_op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	to_temp(&slot, str_op, FALSE);
	push_stack_slot(slot);
	APPEND3(BIN(freeze_string), str_op, debug_info);
	break;
    }
    case BIN(toregexp): {
	rb_num_t opt = code[pos + 1];
	rb_num_t cnt = code[pos + 2];
	vindex_t res;
	
	put_args_on_stack(cnt);
	res = new_top_stack_temp_var();
	APPEND4(BIN(to_regexp), res, opt, cnt);
	break;
    }
    case BIN(newarray):
    case BIN(newhash): {
	rb_num_t cnt = code[pos + 1];
	vindex_t res;
	
	put_args_on_stack(cnt);
	res = new_top_stack_temp_var();
	APPEND4(insn == BIN(newarray) ? BIN(make_array) : BIN(make_hash), res, res, cnt);
	break;
    }
    case BIN(duparray): {
	VALUE ary = code[pos + 1];
	vindex_t res;

	res = new_top_stack_temp_var();
	APPEND3(BIN(clone_array), res, ary);
	break;
    }
    case BIN(expandarray): {
	rb_num_t num = code[pos + 1];
	rb_num_t flag = code[pos + 2];
	long i, cnt = num + (flag & 1 ? 1 : 0);
	vindex_t ary;
	
	slot = pop_stack_slot();
	ary = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	to_temp(&slot, ary, FALSE);
	push_stack_slot(slot);
	ary = get_var();
	for (i = 0; i < cnt; i++)
	  new_top_stack_temp_var();
	APPEND4(BIN(spread_array), ary, num, flag);
	break;
    }
    case BIN(concatarray): {
	vindex_t op1, op2, res;

	get_2vars(&op1, &op2);
	res = new_top_stack_temp_var();
	APPEND4(BIN(concat_array), res, op1, op2);
	break;
    }
    case BIN(splatarray): {
	rb_num_t flag = code[pos + 1];
	vindex_t op, res;

	op = get_var();
	res = new_top_stack_temp_var();
	APPEND4(BIN(splat_array), res, op, flag);
	break;
    }
    case BIN(newrange): {
	rb_num_t flag = code[pos + 1];
	vindex_t op1, op2, res;

	get_2vars(&op1, &op2);
	res = new_top_stack_temp_var();
	APPEND5(BIN(make_range), res, op1, op2, flag);
	break;
    }
    case BIN(pop):
	pop_stack_slot();
	break;
    case BIN(dup):
	slot = VARR_LAST(stack_slot, stack);
	push_stack_slot(slot);
	break;
    case BIN(dupn): {
	size_t len;
	rb_num_t i, n = code[pos + 1];

	len = VARR_LENGTH(stack_slot, stack);
	assert(len >= n);
	for (i = 0; i < n; i++) {
	    slot = VARR_ADDR(stack_slot, stack)[len - n + i];
	    push_stack_slot(slot);
	}
	break;
    }
    case BIN(swap): {
	stack_slot slot2;
	
	slot2 = pop_stack_slot();
	slot = pop_stack_slot();
	if (slot.mode != TEMP || slot2.mode != TEMP) {
	  push_stack_slot(slot2);
	  push_stack_slot(slot);
	} else {
	  vindex_t op = - (vindex_t) VARR_LENGTH(stack_slot, stack) - 1;

	  assert(slot.u.temp == op && slot2.u.temp == op - 1);
	  APPEND3(BIN(var_swap), op, op - 1);
	  push_stack_slot(slot);
	  push_stack_slot(slot2);
	}
	break;
    }
    case BIN(reverse): {
	rb_num_t n = code[pos + 1];

	put_on_stack(n);
	APPEND3(BIN(temp_reverse), n, (vindex_t) (VARR_LENGTH(stack_slot, stack) - n) - 1);
	break;
    }
    case BIN(reput):
	/* just ignore the stack caching for now ??? */
	abort();
	break;
    case BIN(topn): {
	size_t len;
	rb_num_t n = code[pos + 1];
	
	len = VARR_LENGTH(stack_slot, stack);
	assert(len > n);
	slot = VARR_ADDR(stack_slot, stack)[len - n - 1];
	push_stack_slot(slot);
	break;
    }
    case BIN(setn): {
	size_t len;
	rb_num_t n = code[pos + 1];
	long i;
	stack_slot *nth_slot;
	
	assert(n > 0);
	len = VARR_LENGTH(stack_slot, stack);
	assert(len > n);
	slot = VARR_LAST(stack_slot, stack);
	i = len - n - 1;
	nth_slot = &VARR_ADDR(stack_slot, stack)[i];
	prepare_stack_rewrite(nth_slot);
	*nth_slot = slot;
	prepare_loc_var_push(nth_slot);
	to_temp(nth_slot, -i - 1, TRUE);
	break;
    }
    case BIN(adjuststack): {
	rb_num_t n = code[pos + 1];
	vindex_t len = VARR_LENGTH(stack_slot, stack);

	VARR_TRUNC(stack_slot, stack, len - n);
	break;
    }
    case BIN(defined): {
	VALUE op;
	vindex_t res;
	enum ruby_vminsn_type insn = BIN(val_defined_p);

	slot = pop_stack_slot();
	if (slot.mode == VAL) {
	    op = slot.u.val;
	} else if (slot.mode == STR) {
	    op = slot.u.str;
	} else {
	    op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	    op = to_var(slot, op);
	    insn = BIN(defined_p);
	}
	res = new_top_stack_temp_var();
	APPEND6(insn, res, op, code[pos + 1], code[pos + 2], code[pos + 3]);
	break;
    }
    case BIN(checkmatch): {
	vindex_t op1, op2, res;
	
	slot = pop_stack_slot();
	op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op2 = to_var(slot, op2);
	slot = pop_stack_slot();
	op1 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op1 = to_var(slot, op1);
	res = new_top_stack_temp_var();
	APPEND5(BIN(check_match), res, op1, op2, code[pos + 1]);
	break;
    }
    case BIN(checkkeyword): {
	abort();
	/* ??? */
	break;
    }
    case BIN(defineclass): {
	vindex_t op1, op2, res;
	
	slot = pop_stack_slot();
	op2 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op2 = to_var(slot, op2);
	slot = pop_stack_slot();
	op1 = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	op1 = to_var(slot, op1);
	res = new_top_stack_temp_var();
	APPEND7(BIN(define_class), code[pos + 1], code[pos + 2], code[pos + 3],
		op1, op2, res);
	break;
    }
    case BIN(send):
	generate_call(iseq, &code[pos + 1], code[pos + 3]);
	break;
    case BIN(opt_str_freeze): {
	VALUE str = code[pos + 1];
	vindex_t res;
	
	res = new_top_stack_temp_var();
	APPEND3(BIN(str_freeze_call), res, str);
	break;
    }
    case BIN(opt_newarray_max):
    case BIN(opt_newarray_min): {
	rb_num_t num = code[pos + 1];
	vindex_t start;
	enum ruby_vminsn_type res_insn;

	put_args_on_stack(num);
	start = new_top_stack_temp_var();
	res_insn = (insn == BIN(opt_newarray_max)
		    ? BIN(new_array_max) : BIN(new_array_min));
	APPEND4(res_insn, start, start, num);
	break;
    }
    case BIN(opt_send_without_block):
	generate_call(iseq, &code[pos + 1], 0);
	break;
    case BIN(invokesuper): {
	CALL_INFO ci = (CALL_INFO) code[pos + 1];
	CALL_CACHE cc = (CALL_CACHE) code[pos + 2];
	VALUE block = code[pos + 3];
	VALUE recval;
	struct rb_call_data *cd;
	vindex_t args_num;
	int stack_block_p = ci->flag & VM_CALL_ARGS_BLOCKARG;

	args_num = ci->orig_argc + (stack_block_p ? 1: 0);
	put_args_on_stack(args_num);
	slot = pop_stack_slot();
	cd = get_cd(iseq, ci, cc, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	assert(slot.mode == VAL);
	recval = slot.u.val;
	APPEND5(BIN(call_super), cd, cd->call_start, block, recval);
	slot.mode = TEMP;
	slot.u.temp = cd->call_start;
	push_stack_slot(slot);
	break;
    }
    case BIN(invokeblock): {
	CALL_INFO ci = (CALL_INFO) code[pos + 1];
	struct rb_call_data *cd;
	vindex_t args_num;
    
	args_num = ci->orig_argc;
	put_args_on_stack(args_num);
	cd = get_cd(iseq, ci, NULL, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	APPEND3(BIN(call_block), cd, cd->call_start);
	new_top_stack_temp_var();
	break;
    }
    case BIN(leave): {
	generate_leave();
	break;
    }
    case BIN(throw): {
	rb_num_t throw_state = code[pos + 1];

	/* Insns after raise are unreachable so do not pop the
	   stack.  */
	slot = VARR_LAST(stack_slot, stack);
	if (slot.mode == VAL) {
	    APPEND3(BIN(raise_except_val), slot.u.val, throw_state);
	} else {
	    vindex_t op = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;

	    op = to_var(slot, op);
	    APPEND3(BIN(raise_except), op, throw_state);
	}
	break;
    }
    case BIN(jump): {
	size_t dest = code[pos + 1] + pos + stack_insn_len;
	
	if (code[dest] == BIN(leave)) {
	    generate_leave();
	} else if (code[dest] == BIN(trace)
		   && code[dest + insn_len(BIN(trace))] == BIN(leave)) {
	    generate_leave();
	} else {
	    put_top_on_stack();
	    APPEND2(BIN(goto), dest);
	    loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	    loc.offset = 1;
	    VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	}
	break;
    }
    case BIN(branchif):
    case BIN(branchunless):
    case BIN(branchnil): {
	vindex_t op;
	enum ruby_vminsn_type res_insn;

	res_insn = insn == BIN(branchif) ? BIN(bt) : insn == BIN(branchunless) ? BIN(bf) : BIN(bnil);
	slot = pop_stack_slot();
	op = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	put_top_on_stack();
	APPEND3(res_insn, code[pos + 1] + pos + stack_insn_len, op);
	loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	loc.offset = 2;
	VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	break;
    }
    case BIN(getinlinecache): {
	vindex_t res;
	
	put_top_on_stack();
	res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	APPEND4(BIN(get_inline_cache), code[pos + 1] + pos + stack_insn_len, res, code[pos + 2]);
	loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	loc.offset = 3;
	VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	slot.mode = TEMP;
	slot.u.temp = res;
	push_stack_slot(slot);
	break;
    }
    case BIN(setinlinecache): {
	vindex_t op;
	
	slot = pop_stack_slot();
	op = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	APPEND3(BIN(set_inline_cache), op, code[pos + 1]);
	slot.mode = TEMP;
	slot.u.temp = op;
	push_stack_slot(slot);
	break;
    }
    case BIN(once): {
	vindex_t res = new_top_stack_temp_var();
	
	APPEND4(BIN(run_once), res, code[pos + 1], code[pos + 2]);
	break;
    }
    case BIN(opt_case_dispatch): {
	CDHASH hash = code[pos + 1];
	vindex_t op;
	REL_PC incr = pos + stack_insn_len;
	struct hash_label_transform_arg arg;

	hash = rb_hash_dup(hash);
	rb_iseq_add_mark_object(iseq, hash);
	arg.hash = hash;
	arg.map_p = FALSE;
	arg.decr = -incr;
	change_hash_values(hash, &arg);
	slot = pop_stack_slot();
	op = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	APPEND4(BIN(case_dispatch), op, hash, code[pos + 2] + incr);
	loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	loc.offset = 1;
	VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	loc.next_insn_pc = VARR_LENGTH(VALUE, iseq_rtl);
	loc.offset = 0; /* it means the hash table */
	VARR_PUSH(branch_target_loc, branch_target_locs, loc);
	break;
    }
    case BIN(opt_plus):
	generate_bin_op(iseq, &code[pos + 1], BIN(plus));
	break;
    case BIN(opt_minus):
	generate_bin_op(iseq, &code[pos + 1], BIN(minus));
	break;
    case BIN(opt_mult):
	generate_bin_op(iseq, &code[pos + 1], BIN(mult));
	break;
    case BIN(opt_div):
	generate_bin_op(iseq, &code[pos + 1], BIN(div));
	break;
    case BIN(opt_mod):
	generate_bin_op(iseq, &code[pos + 1], BIN(mod));
	break;
    case BIN(opt_eq):
	stack_insn_len = generate_rel_op(iseq, code, pos, BIN(eq));
	break;
    case BIN(opt_neq):
	stack_insn_len = generate_rel_op(iseq, code, pos, BIN(ne));
	break;
    case BIN(opt_lt):
	stack_insn_len = generate_rel_op(iseq, code, pos, BIN(lt));
	break;
    case BIN(opt_le):
	stack_insn_len = generate_rel_op(iseq, code, pos, BIN(le));
	break;
    case BIN(opt_gt):
	stack_insn_len = generate_rel_op(iseq, code, pos, BIN(gt));
	break;
    case BIN(opt_ge):
	stack_insn_len = generate_rel_op(iseq, code, pos, BIN(ge));
	break;
    case BIN(opt_ltlt):
	generate_bin_op(iseq, &code[pos + 1], BIN(ltlt));
	break;
    case BIN(opt_aref):
	generate_bin_op(iseq, &code[pos + 1], BIN(ind));
	break;
    case BIN(opt_aset):
	generate_aset_op(iseq, &code[pos + 1]);
	break;
    case BIN(opt_aset_with): {
	stack_slot slot2;
	VALUE key = code[pos + 3];

	slot = pop_stack_slot();
	slot2.mode = STR;
	slot2.u.str = key;
	push_stack_slot(slot2);
	push_stack_slot(slot);
	generate_aset_op(iseq, &code[pos + 1]);
	break;
    }
    case BIN(opt_aref_with):
	slot.mode = STR;
	slot.u.str = code[pos + 3];
	push_stack_slot(slot);
	generate_bin_op(iseq, &code[pos + 1], BIN(ind));
	break;
    case BIN(opt_length):
	generate_unary_op(iseq, &code[pos + 1], BIN(length));
	break;
    case BIN(opt_size):
	generate_unary_op(iseq, &code[pos + 1], BIN(size));
	break;
    case BIN(opt_empty_p):
	generate_unary_op(iseq, &code[pos + 1], BIN(empty_p));
	break;
    case BIN(opt_succ):
	generate_unary_op(iseq, &code[pos + 1], BIN(succ));
	break;
    case BIN(opt_not):
	generate_unary_op(iseq, &code[pos + 1], BIN(not));
	break;
    case BIN(opt_regexpmatch1): {
	VALUE regexp = code[pos + 1];
	vindex_t res, op;
	
	slot = pop_stack_slot();
	op = to_var(slot, -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1);
	res = new_top_stack_temp_var();
	APPEND4(BIN(regexp_match1), res, regexp, op);
    	break;
    }
    case BIN(opt_regexpmatch2): {
	vindex_t op, op2, res;
	stack_slot slot2;
	CALL_INFO ci = (CALL_INFO) code[pos + 1];
	CALL_CACHE cc = (CALL_CACHE) code[pos + 2];
	struct rb_call_data *cd;

	slot2 = pop_stack_slot();
	slot = pop_stack_slot();
	res = -(vindex_t) VARR_LENGTH(stack_slot, stack) - 1;
	cd = get_cd(iseq, ci, cc, res);
	op = to_var(slot, res);
	op2 = to_var(slot2, res - 1);
	APPEND6(BIN(regexp_match2), BIN(cont_op2), cd, res, op, op2);
	slot.mode = TEMP;
	slot.u.temp = res;
	push_stack_slot(slot);
	break;
    }
    case BIN(opt_call_c_function): {
	vindex_t args_num = iseq->body->param.size + 1;

	put_on_stack(args_num);
	APPEND3(BIN(call_c_func), code[pos + 1], args_num);
	break;
    }
    case BIN(bitblt):
    case BIN(answer):
	assert(FALSE);
	break;
    case BIN(nop):
	if (VARR_ADDR(char, catch_bound_pos_p)[pos])
	    APPEND1(BIN(nop));
	break;
    case BIN(trace): {
	enum ruby_vminsn_type next_insn = code[pos + stack_insn_len];
	rb_num_t nf = code[pos + 1];
	
	APPEND2(BIN(trace), nf);
	break;
    }
    case BIN(getlocal_OP__WC__0):
	get_local(code[pos + 1], 0);
	break;
    case BIN(getlocal_OP__WC__1):
	get_local(code[pos + 1], 1);
	break;
    case BIN(setlocal_OP__WC__0):
	set_local(code[pos + 1], 0);
	break;
    case BIN(setlocal_OP__WC__1):
	set_local(code[pos + 1], 1);
	break;
    case BIN(putobject_OP_INT2FIX_O_0_C_):
	putobject(INT2FIX(0));
	break;
    case BIN(putobject_OP_INT2FIX_O_1_C_):
	putobject(INT2FIX(1));
	break;
    default:
	break;
    }
    pos += stack_insn_len;
    return pos;
}

/* Generate RTL insns from stack insns of ISEQ.  */
static void
translate(rb_iseq_t *iseq){
    const VALUE *code = iseq->body->iseq_encoded;
    size_t i, pos, size;
    
    VARR_TRUNC(branch_target_loc, branch_target_locs, 0);
    VARR_TRUNC(size_t, new_insn_offsets, 0);
    VARR_TRUNC(stack_slot, stack, 0);
    max_stack_depth = 0;
    VARR_TRUNC(size_t, loc_stack_count, 0);
    size = iseq->body->local_table_size + VM_ENV_DATA_SIZE;
    for (i = 0; i < size; i++) {
	VARR_PUSH(size_t, loc_stack_count, 0);
    }
    VARR_TRUNC(VALUE, iseq_rtl, 0);
    size = iseq->body->iseq_size;
    for (pos = 0; pos < size; pos++) {
	VARR_PUSH(size_t, new_insn_offsets, 0);
    }
    for (pos = 0; pos < size;)
	pos = translate_stack_insn(iseq, code, pos);
 }

/* Create a line table of the generated RTL of ISEQ.  Return FALSE if
   we failed to do this.  */
static int
create_rtl_line_table(rb_iseq_t *iseq) {
    size_t i, size;
    struct iseq_line_info_entry *table;
    
    iseq->body->rtl_line_info_size = size = iseq->body->line_info_size;
    iseq->body->rtl_line_info_table = table = ALLOC_N(struct iseq_line_info_entry, size);
    if (table == NULL)
	return FALSE;
    MEMMOVE(table, iseq->body->line_info_table, struct iseq_line_info_entry, size);
    for (i = 0; i < size; i++)
	table[i].position = VARR_ADDR(size_t, new_insn_offsets)[table[i].position];
    return TRUE;
}

/* Create a catch table of the generated RTL of ISEQ.  Return FALSE if
   we failed to do this.  */
static int
create_rtl_catch_table(rb_iseq_t *iseq) {
    size_t i, size;
    const struct iseq_catch_table *table;
    struct iseq_catch_table *rtl_table;
    const struct iseq_catch_table_entry *entries;
    struct iseq_catch_table_entry *rtl_entries;
    const size_t *addr;
    
    table = iseq->body->catch_table;
    if (table == NULL)
	return TRUE;
    size = table->size;
    iseq->body->rtl_catch_table = rtl_table = xmalloc(iseq_catch_table_bytes(size));
    if (rtl_table == NULL)
	return FALSE;
    entries = table->entries;
    rtl_table->size = size;
    rtl_entries = rtl_table->entries;
    addr = VARR_ADDR(size_t, new_insn_offsets);
    for (i = 0; i < size; i++) {
	rtl_entries[i] = entries[i];
	rtl_entries[i].start = addr[rtl_entries[i].start];
	rtl_entries[i].end = addr[rtl_entries[i].end];
	rtl_entries[i].cont = addr[rtl_entries[i].cont];
    }
    return TRUE;
}

/* Create call data for RTL part of ISEQ.  Return FALSE if we failed
   to do this.  */
static int
create_cd_data(rb_iseq_t *iseq) {
    struct rb_call_data *call_data_addr;

    call_data_addr
      = ((struct rb_call_data *)
	 ruby_xmalloc(sizeof(struct rb_call_data) * iseq->body->ci_size +
		      sizeof(struct rb_call_data_with_kwarg) * iseq->body->ci_kw_size));
    if (call_data_addr == NULL)
	return FALSE;
    iseq->body->cd_size = iseq->body->ci_size;
    iseq->body->cd_entries = call_data_addr;
    iseq->body->cd_kw_size = iseq->body->ci_kw_size;
    return TRUE;
}

/* Modify optional param code offsets for generated RTL of ISEQ.  */
static void
setup_opt_table(rb_iseq_t *iseq) {
    int i, opt_num = iseq->body->param.opt_num;
    VALUE *opt_table = iseq->body->param.opt_table;
    
    if (opt_num == 0)
	return;
    for (i = 0; i <= opt_num; i++)
	opt_table[i] = VARR_ADDR(size_t, new_insn_offsets)[opt_table[i]];
}

/* Entry function to generate RTL parts of ISEQ from stack insns.
   Return FALSE if we failed to generate RTL. */
int
rtl_gen(rb_iseq_t *iseq) {
    size_t i;
    branch_target_loc loc;
    REL_PC dest, new_dest;
    
    if (! create_cd_data(iseq))
	return FALSE;
    /* First pass on stack insns:  */
    calculate_stack_depth(iseq);
    /* Second pass on stack insns: */
    translate(iseq);
    iseq->body->rtl_size = VARR_LENGTH(VALUE, iseq_rtl);
    iseq->body->rtl_encoded = ALLOC_N(VALUE, iseq->body->rtl_size);
    iseq->body->temp_vars_num = max_stack_depth;
    if (iseq->body->rtl_encoded == NULL)
      return FALSE;
    MEMMOVE(iseq->body->rtl_encoded, VARR_ADDR(VALUE, iseq_rtl), VALUE, iseq->body->rtl_size);
    /* Change branch destinations:  */
    for (i = 0; i < VARR_LENGTH(branch_target_loc, branch_target_locs); i++) {
	loc = VARR_ADDR(branch_target_loc, branch_target_locs)[i];
	if (loc.offset == 0) {
	    CDHASH hash = iseq->body->rtl_encoded[loc.next_insn_pc - 2];
	    struct hash_label_transform_arg arg;

	    arg.hash = hash;
	    arg.map_p = TRUE;
	    arg.decr = loc.next_insn_pc;
	    RBASIC(hash)->flags &= ~(VALUE) RUBY_FL_FREEZE;
	    change_hash_values(hash, &arg);
	    RB_OBJ_FREEZE(hash);
	} else {
	    dest = iseq->body->rtl_encoded[loc.next_insn_pc - loc.offset];
	    new_dest = VARR_ADDR(size_t, new_insn_offsets)[dest];
	    iseq->body->rtl_encoded[loc.next_insn_pc - loc.offset] = new_dest - loc.next_insn_pc;
	}
    }
    setup_opt_table(iseq);
    if (! create_rtl_line_table(iseq))
	return FALSE;
    return create_rtl_catch_table(iseq);
}

/* Initiate stack insns -> RTL gnerator.  */
void
rtl_gen_init(void) {
    VARR_CREATE(branch_target_loc, branch_target_locs, 0);
    VARR_CREATE(size_t, new_insn_offsets, 0);
    VARR_CREATE(stack_slot, stack, 0);
    VARR_CREATE(size_t, loc_stack_count, 0);
    VARR_CREATE(VALUE, iseq_rtl, 0);
    VARR_CREATE(size_t, pos_stack_free, 0);
    VARR_CREATE(size_t, label_pos_stack, 0);
    VARR_CREATE(char, pos_label_type, 0);
    VARR_CREATE(char, pos_processed_p, 0);
    VARR_CREATE(char, catch_bound_pos_p, 0);
}

/* Finish stack insns -> RTL gnerator.  */
void
rtl_gen_finish(void) {
    VARR_DESTROY(branch_target_loc, branch_target_locs);
    VARR_DESTROY(size_t, new_insn_offsets);
    VARR_DESTROY(stack_slot, stack);
    VARR_DESTROY(size_t, loc_stack_count);
    VARR_DESTROY(VALUE, iseq_rtl);
    VARR_DESTROY(size_t, pos_stack_free);
    VARR_DESTROY(size_t, label_pos_stack);
    VARR_DESTROY(char, pos_label_type);
    VARR_DESTROY(char, pos_processed_p);
    VARR_DESTROY(char, catch_bound_pos_p);
}
