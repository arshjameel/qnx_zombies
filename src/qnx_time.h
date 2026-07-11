#ifndef QNX_TIME_H
#define QNX_TIME_H

#if defined(__QNXNTO__) // QNX

#include <sys/neutrino.h>   /* ClockCycles() */
#include <sys/syspage.h>    /* SYSPAGE_ENTRY  */
#include <unistd.h>         /* usleep()       */
#include <stdint.h>

static double portable_time(void) __attribute__((unused));
static double portable_time(void)
{
    uint64_t cycles = ClockCycles();
    uint64_t cps    = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    return (double)cycles / (double)cps;
}

static void portable_sleep_ms(int ms) __attribute__((unused));
static void portable_sleep_ms(int ms)
{
    usleep((unsigned int)ms * 1000u);
}

#else  // linux

#include <time.h>

static double portable_time(void) __attribute__((unused));
static double portable_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void portable_sleep_ms(int ms) __attribute__((unused));
static void portable_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

#endif

#endif
