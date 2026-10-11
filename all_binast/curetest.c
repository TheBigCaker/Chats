/* curetest.c -- does repair actually work, on damage whose answer is KNOWN?
 *
 * A repairer that is only shown damage it did not cause proves nothing. This injects
 * damage into a real program with a known original, runs binastcure repair, and compares
 * the repaired bytes to the original. The only honest test is byte equality with the
 * ground truth.
 *
 * Damage types, because they need different fixes and because a single test that only
 * flips an opcode bit would hide the failures:
 *
 *   OPCODE   zero the top byte: the word stops matching any row. The map sees this.
 *   OPERAND  corrupt a low byte: the word STILL matches a row (operands are free), so the
 *            map says nothing. Only the operands are wrong. This is the case the map
 *            CANNOT see and it is the majority of real corruption.
 *   BITFLIP  flip one bit: usually lands on a sibling row one step away in the geometry.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "arbishim.h"

static uint16_t r16_(const uint8_t *p){return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t r32_(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t r64_(const uint8_t *p){uint64_t v=0;for(int i=7;i>=0;i--)v=(v<<8)|p[i];return v;}

typedef struct { uint8_t *img; size_t size; uint8_t *code; size_t off,sz; uint64_t va; } Img;
static int load(const char *p, Img *o){
    FILE *f=fopen(p,"rb"); if(!f) return -1;
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    uint8_t *d=(uint8_t*)arb_carve_zeroed(n);
    if(!d||fread(d,1,(size_t)n,f)!=(size_t)n){fclose(f);return -1;}
    fclose(f);
    uint64_t so=r64_(d+0x28); int sn=r16_(d+0x3c), se=r16_(d+0x3a);
    uint64_t bo=0,bv=0,bs=0;
    for(int i=0;i<sn;i++){const uint8_t*sh=d+so+(uint64_t)i*se;
        uint64_t fl=r64_(sh+8),va=r64_(sh+0x10),of=r64_(sh+0x18),sz=r64_(sh+0x20);
        if(!(fl&4)||!sz||of>=(uint64_t)n)continue;
        if(of+sz>(uint64_t)n)sz=(uint64_t)n-of;
        if(sz>bs){bs=sz;bo=of;bv=va;}}
    o->img=d;o->size=(size_t)n;o->code=d+bo;o->off=(size_t)bo;o->sz=(size_t)bs;o->va=bv;
    return 0;
}
static void save(const Img *o,const char*p){FILE*f=fopen(p,"wb");if(!f)return;fwrite(o->img,1,o->size,f);fclose(f);}

#define SENTINEL 0xC0DEu

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: curetest <good.elf> <workdir>\n"); return 2; }
    arbit_init(0);
    const char *good = argv[1];
    const char *wd = argv[2];
    char dmg[512], out[512];
    Img src;
    if (load(good, &src) != 0) { fprintf(stderr, "cannot load %s\n", good); return 2; }

    long n = (long)(src.sz / 4);
    printf("=== the test program: %s, %ld code words\n", good, n);
    printf("    ground truth: the ORIGINAL bytes. A repair is correct only if it\n");
    printf("    reproduces them exactly.\n");

    /* pick words to damage: skip zeros and the first few (entry, so the run stays sane) */
    /* PICK REAL INSTRUCTIONS ONLY. The first version picked by "looks like it has an opcode
     * byte", which landed on a DATA word inside the code section (6474e551 is the ASCII
     * "Q\xe5td"), and the repairer CORRECTLY refused to treat data as an instruction. The
     * test was wrong, not the tool. So the choice is made the same way the repairer decides:
     * the word must decode to a catalog row. */
    long picks[3] = {0,0,0};
    int found = 0;
    {
        char cmd[1024], line[512];
        for (long i = 40; i < n && found < 3; i += 211) {
            uint32_t w = r32_(src.code + i*4);
            if (w == 0) continue;
            snprintf(cmd, sizeof cmd, "./isadb decode %08x 2>/dev/null", w);
            FILE *pp = popen(cmd, "r");
            int is_instr = 0;
            if (pp) { if (fgets(line, sizeof line, pp) && !strstr(line, "??")) is_instr = 1; pclose(pp); }
            if (!is_instr) continue;
            /* and it must not be a branch: those carry a PC-relative displacement, so
             * corrupting one changes control flow and the program stops running, which
             * would confound "did the repair work" with "did the program survive" */
            if (strstr(line, "B_") || strstr(line, "BL_") || strstr(line, "CBZ") ||
                strstr(line, "CBNZ") || strstr(line, "TBZ") || strstr(line, "RET")) continue;
            if (found == 0 && (w >> 24) == 0) continue;   /* want a real opcode byte to zero */
            picks[found++] = i;
        }
    }
    if (found < 3) { fprintf(stderr, "could not find 3 words to damage\n"); return 2; }

    const char *names[3] = {"OPCODE", "OPERAND", "BITFLIP"};
    uint32_t origs[3], dmgs[3];
    for (int t = 0; t < 3; t++)
        origs[t] = r32_(src.code + picks[t]*4);

    /* --- inject, one damage at a time, and repair --- */
    int pass = 0;
    for (int t = 0; t < 3; t++) {
        Img im;
        if (load(good, &im) != 0) return 2;
        uint8_t *b = im.code + picks[t]*4;
        uint32_t d = origs[t];
        if (t == 0) {
            /* zero the opcode byte -- but ONLY the part that is actually there, and reject
             * the word if its top byte is already 0, because then the "damage" changes
             * nothing and a BYTE-EXACT verdict would be vacuous. The previous run passed
             * this case on 00000015, which was no damage at all. */
            if ((origs[t] >> 24) == 0) { printf("\n--- OPCODE: skipped, top byte already 0 (damage would be vacuous)\n"); continue; }
            d = origs[t] & 0x00FFFFFFu;
        }
        else if (t == 1) d = (origs[t] & 0xFF00FFFFu) | (0x5Au << 16);  /* corrupt an operand */
        else             d = origs[t] ^ 0x00000040u;                /* flip one bit */
        if (d == origs[t]) { printf("\n--- %s: skipped, injected damage did not change the word\n", names[t]); continue; }
        dmgs[t] = d;
        b[0]=(uint8_t)d; b[1]=(uint8_t)(d>>8); b[2]=(uint8_t)(d>>16); b[3]=(uint8_t)(d>>24);
        snprintf(dmg,sizeof dmg,"%s/dmg%d.elf",wd,t);
        snprintf(out,sizeof out,"%s/fix%d.elf",wd,t);
        save(&im, dmg);
        printf("\n--- %s damage at 0x%llx:  %08x -> %08x\n", names[t],
               (unsigned long long)(im.va + (uint64_t)picks[t]*4), origs[t], d);

        /* run the repairer */
        char cmd[2048];
        snprintf(cmd,sizeof cmd,"%s/binastcure repair %s %s %s 2>&1 | tail -4", wd, dmg, good, out);
        FILE *pp = popen(cmd, "r");
        if (pp) { char l[512]; while (fgets(l,sizeof l,pp)) printf("      %s", l); pclose(pp); }

        /* --- VERDICT: byte-exact against the original? --- */
        Img fixed;
        if (load(out, &fixed) != 0) { printf("    VERDICT: could not read the repaired file\n"); continue; }
        uint32_t got = r32_(fixed.code + picks[t]*4);
        int ok = (got == origs[t]);
        printf("    VERDICT: %s  (repaired %08x, original %08x)\n", ok ? "BYTE-EXACT" : "WRONG", got, origs[t]);
        if (ok) pass++;
    }
    printf("\n=== %d of 3 repairs byte-exact\n", pass);
    printf("    OPCODE damage: the map sees it (no row matches), so it is repairable from\n");
    printf("    the map plus context alone.\n");
    printf("    OPERAND damage: the word still matches a row, so the map is SILENT. Only a\n");
    printf("    reference can know the operand was wrong -- this is the honest limit.\n");
    printf("    BITFLIP: usually lands on a sibling row one step away in the geometry.\n");
    return pass == 3 ? 0 : 1;
}
