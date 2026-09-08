/*
 * A tiny cache-sensitive benchmark: N x N matrix multiply.
 * Small enough to simulate in seconds, big enough that the L2 size matters.
 *
 * Build (static, so gem5 SE mode can run it):
 *   gcc -O2 -static -o mm mm.c
 */
#include <stdio.h>

#define N 128

static double a[N][N], b[N][N], c[N][N];

int main(void) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            a[i][j] = i + j;
            b[i][j] = i - j;
        }

    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            double sum = 0.0;
            for (int k = 0; k < N; k++)
                sum += a[i][k] * b[k][j];
            c[i][j] = sum;
        }

    printf("c[0][0] = %.1f, c[N-1][N-1] = %.1f\n", c[0][0], c[N-1][N-1]);
    return 0;
}
