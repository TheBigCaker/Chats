/* relink.h -- the linker half: relocate code by rewriting PC-relative operands.
 *
 * An instruction like `b #off19` stores a distance, not a destination. Move the
 * instruction and the encoded distance is wrong. The fix is not to patch bytes but
 * to recover the absolute target, then re-encode the distance to that same target
 * from the new location:
 *
 *   decode -> human operands -> PC-relative operand becomes an absolute address
 *          -> move the block -> absolute address becomes a new PC-relative offset
 *          -> re-encode
 *
 * Ranges are enforced by the encoder, because isa_encode refuses an out-of-range
 * offset: a relocated branch that no longer fits is a hard error, never a silent
 * wrong jump.
 *
 * Fixed point: for a fixed-width ISA one pass is exact. For a variable-length ISA a
 * retarget can change an encoding's length, shifting every later pc, so the loop
 * repeats until nothing moves or the cap is hit.
 */
#ifndef RELINK_H
#define RELINK_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "isadb.h"

#define RELINK_MAXINSN 65536

typedef struct {
    int            off;
    int            len;
    const IsaForm *form;
    long           human[ISA_MAXV];
    int            prel_idx;
    long           prel_orig;   /* the offset as it was in the SOURCE block */
} RInsn;

/* RBlock is megabytes (RELINK_MAXINSN * sizeof(RInsn)). It must never be a local:
 * relink_verify holds two of them, which overflowed the stack and segfaulted. These
 * are file-scope because the tool is a single-threaded CLI. */
typedef struct {
    RInsn insn[RELINK_MAXINSN];
    int   n;
    int   block_len;
    int   n_prel;
} RBlock;

/* Which operand of this row is PC-relative? Branch rows carry a signed immediate
 * whose template names it an offset ("#simm26", "#simm19"). */
/* Which operand of this row names an address rather than a value? Two shapes:
 * a branch offset (#simm, relative to the instruction) and ADRP's page count
 * (relative to the page containing the instruction). Missing the second one leaves
 * every adrp pointing at its old page after a move -- the code loads its data from
 * the wrong address and faults, which is exactly what happened before this. */
static int relink_prel_operand(const IsaForm *f) {
    for (int v = f->nvars - 1; v >= 0; v--) {
        if (f->vkind[v] == K_PAGCNT) return v;
        if (f->vkind[v] != K_SIMM) continue;
        const char *p = f->operands;
        int k = 0;
        while (*p && k < v) { if (*p == ',') k++; p++; }
        if (!*p) continue;
        if (p[0] == '#' && (p[1] == 's' || p[1] == 'S' || p[1] == 'o' || p[1] == 'O'))
            return v;
    }
    return -1;
}

static int relink_scan(const IsaTable *t, const uint8_t *code, int len, uint64_t vaddr, RBlock *b) {
    memset(b, 0, sizeof *b);
    b->block_len = len;
    int off = 0;
    while (off < len) {
        const IsaForm *f = NULL;
        long human[ISA_MAXV] = {0, 0, 0, 0};
        int used = isa_decode(t, code + off, len - off, &f, human);
        if (!used) {
            fprintf(stderr, "relink: no catalog form at %#llx (offset %d, byte %02x)\n",
                    (unsigned long long)vaddr + (unsigned)off, off, code[off]);
            return -1;
        }
        if (b->n >= RELINK_MAXINSN) { fprintf(stderr, "relink: block too large\n"); return -1; }
        RInsn *r = &b->insn[b->n++];
        r->off = off;
        r->len = used;
        r->form = f;
        memcpy(r->human, human, sizeof human);
        r->prel_idx = relink_prel_operand(f);
        r->prel_orig = (r->prel_idx >= 0) ? human[r->prel_idx] : 0;
        if (r->prel_idx >= 0) b->n_prel++;
        off += used;
    }
    if (off != len) { fprintf(stderr, "relink: %d instructions cover %d of %d bytes\n", b->n, off, len); return -1; }
    return 0;
}

/* The target of a branch as it stands in the SOURCE block. Using the current human
 * value here instead is the bug this exists to prevent: after the first pass the
 * stored value is a destination-relative distance, so recomputing from the source pc
 * adds the shift a second time and produces a wrong target on every later pass.
 *
 * The base differs by kind: a branch is relative to the instruction address, ADRP is
 * relative to the page the instruction sits in. */
static uint64_t relink_target_orig(const RInsn *r, uint64_t pc) {
    if (r->form->vkind[r->prel_idx] == K_PAGCNT)
        return (pc & ~(uint64_t)0xfff) + (uint64_t)r->prel_orig * 4096u;
    return pc + (uint64_t)r->prel_orig;
}

/* The value to store so that `pc` reaches `target`, for this operand's kind. */
static long relink_offset_for(const RInsn *r, uint64_t pc, uint64_t target) {
    if (r->form->vkind[r->prel_idx] == K_PAGCNT)
        return (long)((int64_t)target - (int64_t)(pc & ~(uint64_t)0xfff)) / 4096;
    return (long)((int64_t)target - (int64_t)pc);
}

/* Retarget the block from src_va to dst_va. Targets inside the block travel with
 * it; everything else keeps its absolute address. Non-zero on failure. */
__attribute__((unused)) static int relink_move(const IsaTable *t, const uint8_t *code, int len,
                       uint64_t src_va, uint64_t dst_va, uint8_t *out, int *out_len) {
    static RBlock b;
    if (relink_scan(t, code, len, src_va, &b) != 0) return -1;
    memcpy(out, code, (size_t)len);

    for (int pass = 0; pass < 4; pass++) {
        int moved = 0;
        for (int i = 0; i < b.n; i++) {
            RInsn *r = &b.insn[i];
            if (r->prel_idx < 0) continue;
            uint64_t new_pc = dst_va + (unsigned)r->off;
            uint64_t target = relink_target_orig(r, src_va + (unsigned)r->off);
            if (target >= src_va && target < src_va + (uint64_t)len)
                target = dst_va + (target - src_va);
            long new_off = relink_offset_for(r, new_pc, target);
            if (new_off == r->human[r->prel_idx]) continue;

            long human2[ISA_MAXV];
            memcpy(human2, r->human, sizeof human2);
            human2[r->prel_idx] = new_off;
            uint8_t enc[ISA_MAXBYTES]; char err[200];
            if (isa_encode(r->form, human2, enc, err, sizeof err) != 0) {
                fprintf(stderr, "relink: %s at %#llx cannot reach %#llx after the move: %s\n",
                        r->form->name, (unsigned long long)new_pc, (unsigned long long)target, err);
                return -1;
            }
            memcpy(out + r->off, enc, (size_t)r->len);
            r->human[r->prel_idx] = new_off;
            moved++;
        }
        if (!moved) break;
    }
    *out_len = len;
    return 0;
}

/* The invariant that matters, checked by scanning both versions independently:
 * after the move every PC-relative operand must still name the same destination,
 * with in-block targets having travelled along with the block. */
__attribute__((unused)) static int relink_verify(const IsaTable *t, const uint8_t *before, int len,
                         uint64_t src_va, const uint8_t *after, uint64_t dst_va) {
    static RBlock a, b;
    if (relink_scan(t, before, len, src_va, &a) != 0) return -1;
    if (relink_scan(t, after, len, dst_va, &b) != 0) return -1;
    if (a.n != b.n) {
        fprintf(stderr, "relink verify: instruction count changed %d -> %d\n", a.n, b.n);
        return -1;
    }
    int checked = 0, bad = 0;
    for (int i = 0; i < a.n; i++) {
        const RInsn *ra = &a.insn[i], *rb = &b.insn[i];
        if (ra->off != rb->off) { printf("relink verify: offset drift at insn %d\n", i); return -1; }
        if (ra->prel_idx < 0) continue;
        uint64_t ta = relink_target_orig(ra, src_va + (unsigned)ra->off);
        uint64_t tb = relink_target_orig(rb, dst_va + (unsigned)rb->off);
        uint64_t expect = (ta >= src_va && ta < src_va + (uint64_t)len) ? dst_va + (ta - src_va) : ta;
        if (expect != tb) {
            printf("relink verify: %s at offset %d pointed at %#llx, now %#llx (want %#llx)\n",
                   ra->form->name, ra->off, (unsigned long long)ta,
                   (unsigned long long)tb, (unsigned long long)expect);
            bad++;
        }
        checked++;
    }
    if (bad) return -1;
    printf("relink verify: %d instruction(s), %d PC-relative operand(s) hold their target\n",
           a.n, checked);
    return 0;
}

#endif /* RELINK_H */
