// Mirage Wallpaper
// Created on 2026/10/29. Copyright © 2026 王孝慈. All rights reserved.

#include <windows.h>
#include <stdio.h>

int main(void) {
    LARGE_INTEGER frequency, start, finish;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&start)) return 1;
    volatile unsigned long long sum = 0;
    for (unsigned i = 0; i < 1000000; ++i) sum += i;
    if (!QueryPerformanceCounter(&finish) || sum != 499999500000ULL) return 2;
    printf("MIRAGE_FEX_X64_OK sum=%llu frequency=%lld elapsed=%lld\n",
           sum, frequency.QuadPart, finish.QuadPart - start.QuadPart);
    return 0;
}
