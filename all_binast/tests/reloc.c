
#include <stdio.h>
static int fib(int n) {
    int a = 0, b = 1, t;
    for (int i = 0; i < n; i++) { t = a + b; a = b; b = t; }
    return a;
}
static int count_down(int n) {
    int acc = 0;
    while (n > 0) { acc += n; n--; }
    return acc;
}
int main(void) {
    printf("%d %d\n", fib(12), count_down(20));
    return 0;
}
