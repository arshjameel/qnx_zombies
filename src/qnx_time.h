#ifndef QNX_TIME_H
#define QNX_TIME_H

/*
 * qnx_time.h -- portable monotonic time and millisecond sleep
 *
 * QNX:   ClockCycles() + SYSPAGE for time, usleep() for sleep
 * Linux: clock_gettime(CLOCK_MONOTONIC),   nanosleep() for sleep
 *
 * Avoids CLOCK_MONOTONIC on QNX entirely -- its visibility varies
 * across SDP versions and conflicts with __EXT_* guards.
 * ClockCycles() and SYSPAGE need no feature-test macros.
 */

#if defined(__QNXNTO__)

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

#else  /* Linux */

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

#endif /* __QNXNTO__ */

#endif /* QNX_TIME_H */
