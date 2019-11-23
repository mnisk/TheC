#include "regalloc.h"

#include <assert.h>
#include <stdlib.h>  // malloc
#include <string.h>

#include "codegen.h"  // WORD_SIZE
#include "ir.h"
#include "util.h"
#include "var.h"

// Register allocator

RegAlloc *new_reg_alloc(void) {
  RegAlloc *ra = malloc(sizeof(*ra));
  ra->vregs = new_vector();
  ra->regno = 0;
  vec_clear(ra->vregs);
  return ra;
}

VReg *reg_alloc_spawn(RegAlloc *ra, const Type *type) {
  VReg *vreg = new_vreg(ra->regno++, type);
  vec_push(ra->vregs, vreg);
  return vreg;
}

// Rewrite `A = B op C` to `A = B; A = A op C`.
static void three_to_two(BB *bb) {
  Vector *irs = bb->irs;
  for (int i = 0; i < irs->len; ++i) {
    IR *ir = bb->irs->data[i];

    switch (ir->kind) {
    case IR_ADD:  // binops
    case IR_SUB:
    case IR_MUL:
    case IR_DIV:
    case IR_MOD:
    case IR_BITAND:
    case IR_BITOR:
    case IR_BITXOR:
    case IR_LSHIFT:
    case IR_RSHIFT:
    case IR_NEG:  // unary ops
    case IR_BITNOT:
      {
        IR *ir2 = malloc(sizeof(*ir));
        ir2->kind = IR_MOV;
        ir2->dst = ir->dst;
        ir2->opr1 = ir->opr1;
        ir2->opr2 = NULL;
        ir2->size = ir->size;
        vec_insert(irs, i, ir2);

        ir->opr1 = ir->dst;
        ++i;
      }
      break;

    default:
      break;
    }
  }
  bb->irs = irs;
}

typedef struct LiveInterval {
  int vreg;
  int rreg;
  int start;
  int end;
  bool spill;
} LiveInterval;

static int insert_active(LiveInterval **active, int active_count, LiveInterval *li) {
  int j;
  for (j = 0; j < active_count; ++j) {
    LiveInterval *p = active[j];
    if (li->end < p->end)
      break;
  }
  if (j < active_count)
    memmove(&active[j + 1], &active[j], sizeof(LiveInterval*) * (active_count - j));
  active[j] = li;
  return j;
}

static void remove_active(LiveInterval **active, int active_count, int start, int n) {
  if (n <= 0)
    return;
  int tail = active_count - (start + n);
  assert(tail >= 0);

  if (tail > 0)
    memmove(&active[start], &active[start + n], sizeof(LiveInterval*) * tail);
}

static int sort_live_interval(LiveInterval **pa, LiveInterval **pb) {
  LiveInterval *a = *pa, *b = *pb;
  int d = a->start - b->start;
  if (d == 0)
    d = b->end - a->start;
  return d;
}

static void split_at_interval(LiveInterval **active, int active_count, LiveInterval *li) {
  assert(active_count > 0);
  LiveInterval *spill = active[active_count - 1];
  if (spill->end > li->end) {
    li->rreg = spill->rreg;
    spill->rreg = SPILLED_REG_NO;
    spill->spill = true;
    insert_active(active, active_count - 1, li);
  } else {
    li->rreg = SPILLED_REG_NO;
    li->spill = true;
  }
}

static void expire_old_intervals(LiveInterval **active, int *pactive_count, short *pusing_bits, int start) {
  int active_count = *pactive_count;
  int j;
  short using_bits = *pusing_bits;
  for (j = 0; j < active_count; ++j) {
    LiveInterval *li = active[j];
    if (li->end > start)
      break;
    using_bits &= ~((short)1 << li->rreg);
  }
  remove_active(active, active_count, 0, j);
  *pactive_count = active_count - j;
  *pusing_bits = using_bits;
}

static LiveInterval **check_live_interval(BBContainer *bbcon, int vreg_count, LiveInterval **pintervals) {
  LiveInterval *intervals = malloc(sizeof(LiveInterval) * vreg_count);
  for (int i = 0; i < vreg_count; ++i) {
    LiveInterval *li = &intervals[i];
    li->vreg = i;
    li->rreg = -1;
    li->start = li->end = -1;
    li->spill = false;
  }

  int nip = 0;
  for (int i = 0; i < bbcon->bbs->len; ++i) {
    BB *bb = bbcon->bbs->data[i];
    for (int j = 0; j < bb->irs->len; ++j, ++nip) {
      IR *ir = bb->irs->data[j];
      VReg *regs[] = {ir->dst, ir->opr1, ir->opr2};
      for (int k = 0; k < 3; ++k) {
        VReg *reg = regs[k];
        if (reg == NULL)
          continue;
        LiveInterval *li = &intervals[reg->v];
        if (li->start < 0)
          li->start = nip;
        if (li->end < nip)
          li->end = nip;
      }
    }

    for (int j = 0; j < bb->out_regs->len; ++j) {
      VReg *reg = bb->out_regs->data[j];
      LiveInterval *li = &intervals[reg->v];
      if (li->start < 0)
        li->start = nip;
      if (li->end < nip)
        li->end = nip;
    }
  }

  // Sort by start, end
  LiveInterval **sorted_intervals = malloc(sizeof(LiveInterval*) * vreg_count);
  for (int i = 0; i < vreg_count; ++i)
    sorted_intervals[i] = &intervals[i];
  myqsort(sorted_intervals, vreg_count, sizeof(LiveInterval*), (int (*)(const void*, const void*))sort_live_interval);

  *pintervals = intervals;
  return sorted_intervals;
}

static short linear_scan_register_allocation(LiveInterval **sorted_intervals, int vreg_count) {
  LiveInterval *active[REG_COUNT];
  int active_count = 0;
  short using_bits = 0;
  short used_bits = 0;

  for (int i = 0; i < vreg_count; ++i) {
    LiveInterval *li = sorted_intervals[i];
    if (li->spill)
      continue;
    expire_old_intervals(active, &active_count, &using_bits, li->start);
    if (active_count >= REG_COUNT) {
      split_at_interval(active, active_count, li);
    } else {
      int regno = -1;
      for (int j = 0; j < REG_COUNT; ++j) {
        if (!(using_bits & (1 << j))) {
          regno = j;
          break;
        }
      }
      assert(regno >= 0);
      li->rreg = regno;
      using_bits |= 1 << regno;

      insert_active(active, active_count, li);
      ++active_count;
    }
    used_bits |= using_bits;
  }
  return used_bits;
}

static int insert_load_store_spilled(BBContainer *bbcon, Vector *vregs) {
  int inserted = 0;
  for (int i = 0; i < bbcon->bbs->len; ++i) {
    BB *bb = bbcon->bbs->data[i];
    Vector *irs = bb->irs;
    for (int j = 0; j < irs->len; ++j) {
      IR *ir = irs->data[j];

      switch (ir->kind) {
      case IR_IMM:
      case IR_BOFS:
      case IR_IOFS:
      case IR_MOV:
      case IR_ADD:  // binops
      case IR_SUB:
      case IR_MUL:
      case IR_DIV:
      case IR_MOD:
      case IR_BITAND:
      case IR_BITOR:
      case IR_BITXOR:
      case IR_LSHIFT:
      case IR_RSHIFT:
      case IR_CMP:
      case IR_NEG:  // unary ops
      case IR_NOT:
      case IR_BITNOT:
      case IR_SET:
      case IR_TEST:
      case IR_PUSHARG:
      case IR_CALL:
      case IR_CAST:
      case IR_RESULT:
        if (ir->opr1 != NULL && ir->opr1->r == SPILLED_REG_NO) {
          vec_insert(irs, j,
                     new_ir_load_spilled(((VReg*)vregs->data[ir->opr1->v])->offset, ir->size));
          ++j;
          inserted |= 1;
        }

        if (ir->opr2 != NULL && ir->opr2->r == SPILLED_REG_NO) {
          vec_insert(irs, j,
                     new_ir_load_spilled(((VReg*)vregs->data[ir->opr2->v])->offset, ir->size));
          ++j;
          inserted |= 2;
        }

        if (ir->dst != NULL && ir->dst->r == SPILLED_REG_NO) {
          ++j;
          vec_insert(irs, j,
                     new_ir_store_spilled(((VReg*)vregs->data[ir->dst->v])->offset, ir->size));
          inserted |= 4;
        }
        break;

      case IR_LOAD:
      case IR_STORE:
      case IR_MEMCPY:
        if (ir->opr1 != NULL && ir->opr1->r == SPILLED_REG_NO) {
          vec_insert(irs, j,
                     new_ir_load_spilled(((VReg*)vregs->data[ir->opr1->v])->offset, WORD_SIZE));
          ++j;
          inserted |= 1;
        }

        if (ir->opr2 != NULL && ir->opr2->r == SPILLED_REG_NO) {
          vec_insert(irs, j,
                     new_ir_load_spilled(((VReg*)vregs->data[ir->opr2->v])->offset, WORD_SIZE));
          ++j;
          inserted |= 2;
        }

        if (ir->dst != NULL && ir->dst->r == SPILLED_REG_NO) {
          ++j;
          vec_insert(irs, j,
                     new_ir_store_spilled(((VReg*)vregs->data[ir->dst->v])->offset, ir->size));
          inserted |= 4;
        }
        break;

      default:
        break;
      }
    }
  }
  return inserted;
}

static void analyze_reg_flow(BBContainer *bbcon) {
  // Enumerate in and defined regsiters for each BB.
  for (int i = 0; i < bbcon->bbs->len; ++i) {
    BB *bb = bbcon->bbs->data[i];
    Vector *in_regs = new_vector();
    Vector *assigned_regs = new_vector();
    Vector *irs = bb->irs;
    for (int j = 0; j < irs->len; ++j) {
      IR *ir = irs->data[j];
      VReg *regs[] = {ir->opr1, ir->opr2};
      for (int k = 0; k < 2; ++k) {
        VReg *reg = regs[k];
        if (reg == NULL)
          continue;
        if (!vec_contains(in_regs, reg) &&
            !vec_contains(assigned_regs, reg))
          vec_push(in_regs, reg);
      }
      if (ir->dst != NULL && !vec_contains(assigned_regs, ir->dst))
        vec_push(assigned_regs, ir->dst);
    }

    bb->in_regs = in_regs;
    bb->out_regs = new_vector();
    bb->assigned_regs = assigned_regs;
  }

  // Propagate in regs to previous BB.
  bool cont;
  do {
    cont = false;
    for (int i = 0; i < bbcon->bbs->len; ++i) {
      BB *bb = bbcon->bbs->data[i];
      Vector *irs = bb->irs;

      BB *next_bbs[2];
      next_bbs[0] = bb->next;
      next_bbs[1] = NULL;

      if (irs->len > 0) {
        IR *ir = irs->data[irs->len - 1];
        if (ir->kind == IR_JMP) {
          next_bbs[1] = ir->jmp.bb;
          if (ir->jmp.cond == COND_ANY)
            next_bbs[0] = NULL;
        }
      }
      for (int j = 0; j < 2; ++j) {
        BB *next = next_bbs[j];
        if (next == NULL)
          continue;
        Vector *in_regs = next->in_regs;
        for (int k = 0; k < in_regs->len; ++k) {
          VReg *reg = in_regs->data[k];
          if (!vec_contains(bb->out_regs, reg))
            vec_push(bb->out_regs, reg);
          if (vec_contains(bb->assigned_regs, reg) ||
              vec_contains(bb->in_regs, reg))
            continue;
          vec_push(bb->in_regs, reg);
          cont = true;
        }
      }
    }
  } while (cont);
}

size_t alloc_real_registers(Function *func) {
  BBContainer *bbcon = func->bbcon;
  for (int i = 0; i < bbcon->bbs->len; ++i) {
    BB *bb = bbcon->bbs->data[i];
    three_to_two(bb);
  }

  analyze_reg_flow(bbcon);

  int vreg_count = func->ra->regno;
  LiveInterval *intervals;
  LiveInterval **sorted_intervals = check_live_interval(bbcon, vreg_count, &intervals);

  for (int i = 0; i < vreg_count; ++i) {
    LiveInterval *li = &intervals[i];
    VReg *vreg = func->ra->vregs->data[i];
    if (vreg->r == SPILLED_REG_NO) {
      li->spill = true;
      li->rreg = vreg->r;
    }
  }

  if (func->params != NULL) {
    // Make function parameters all spilled.
    for (int i = 0; i < func->params->len; ++i) {
      VarInfo *varinfo = func->params->data[i];

      LiveInterval *li = &intervals[varinfo->reg->v];
      li->start = 0;
      li->spill = true;
      li->rreg = SPILLED_REG_NO;
    }
  }

  func->used_reg_bits = linear_scan_register_allocation(sorted_intervals, vreg_count);

  // Map vreg to rreg.
  for (int i = 0; i < vreg_count; ++i) {
    VReg *vreg = func->ra->vregs->data[i];
    vreg->r = intervals[vreg->v].rreg;
  }

  // Allocated spilled virtual registers onto stack.
  size_t frame_size = 0;
  for (int i = 0; i < vreg_count; ++i) {
    LiveInterval *li = sorted_intervals[i];
    if (!li->spill)
      continue;
    VReg *vreg = func->ra->vregs->data[li->vreg];
    if (vreg->offset != 0) {  // Variadic function parameter or stack parameter.
      if (-vreg->offset > (int)frame_size)
        frame_size = -vreg->offset;
      continue;
    }

    int size, align;
    const Type *type = vreg->type;
    assert(type != NULL);
    size = type_size(type);
    align = align_size(type);
    if (size < 1)
      size = 1;

    frame_size = ALIGN(frame_size + size, align);
    vreg->offset = -frame_size;
  }

  int inserted = insert_load_store_spilled(bbcon, func->ra->vregs);
  if (inserted != 0)
    func->used_reg_bits |= 1 << SPILLED_REG_NO;

  func->ra->sorted_intervals = sorted_intervals;

  return ALIGN(frame_size, 16);
}