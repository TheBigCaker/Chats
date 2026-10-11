#include <stdio.h>

/* Sum of squares over [1, n], with a running accumulator.
   Written for humans: long descriptive names, comments, airy spacing. */
int sum_of_squares(int n) {
    int accumulator = 0;
    for (int index = 1; index <= n; index++) {
        accumulator += index * index;
    }
    return accumulator;
}

int main(void) {
    printf("%d\n", sum_of_squares(10));
    return 0;
}
