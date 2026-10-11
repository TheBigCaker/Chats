/* cosmetic -- strip comments and reformat C source WITHOUT changing tokens.
 * Used to measure how much of a binary's bytes come from formatting/comment
 * churn rather than semantics.  Pure C11, no deps.
 *
 *   cosmetic <in.c> <out.c>
 */
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: cosmetic <in.c> <out.c>\n"); return 2; }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { fprintf(stderr, "cosmetic: cannot open %s\n", argv[1]); return 1; }
    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "cosmetic: cannot write %s\n", argv[2]); return 1; }

    int c, prev = 0, col = 0;
    /* string/char literal state, and preprocessor line state */
    int in_str = 0, in_chr = 0, in_pre = 0, esc = 0;
    while ((c = fgetc(in)) != EOF) {
        if (in_str || in_chr) {
            fputc(c, out); col++;
            if (esc) { esc = 0; continue; }
            if (c == '\\') { esc = 1; continue; }
            if (in_str && c == '"') in_str = 0;
            if (in_chr && c == '\'') in_chr = 0;
            prev = c;
            continue;
        }
        if (c == '\n') {
            in_pre = 0;
            if (prev != '\n') { fputc('\n', out); prev = '\n'; }
            col = 0;
            continue;
        }
        if (!in_pre && c == '"') { in_str = 1; fputc(c, out); prev = c; col++; continue; }
        if (!in_pre && c == '\'') { in_chr = 1; fputc(c, out); prev = c; col++; continue; }
        /* comment handling */
        if (c == '/') {
            int n = fgetc(in);
            if (n == '/') { while ((c = fgetc(in)) != EOF && c != '\n') {} if (c == '\n') { if (prev != '\n') { fputc('\n', out); prev = '\n'; } col = 0; } continue; }
            if (n == '*') {
                int a = 0;
                while ((c = fgetc(in)) != EOF) { if (a && c == '/') break; a = (c == '*'); }
                continue;   /* comment removed entirely */
            }
            fputc('/', out); prev = '/'; col++;
            if (n != EOF) { fputc(n, out); prev = n; col++; }
            ungetc(n, in); /* note: re-emitted above; keep stream consistent */
            { int back = fgetc(in); (void)back; }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {   /* collapse runs, never at line start */
            if (prev == '\n' || prev == 0) continue;
            fputc(' ', out); prev = ' '; col++;
            continue;
        }
        /* preprocessor directives must keep their leading '#' on a real column */
        if (col == 0 && c == '#') in_pre = 1;
        fputc(c, out); prev = c; col++;
    }
    fclose(in); fclose(out);
    return 0;
}
