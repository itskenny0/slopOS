/* primes.c -- a deliberately rude program.
 *
 * It never sleeps, never yields and never asks the kernel for anything for
 * seconds at a time. If the shell still answers while this is running in
 * the background, the scheduler is genuinely taking the CPU away from it
 * rather than politely waiting to be handed it.
 *
 *   start primes &     run it behind the shell
 *   ps                 watch its CPU time climb
 *   fg 2               bring it forward, output and all
 *   ctrl-c             stop it
 */
#include "picoapp.h"

static int is_prime(unsigned n)
{
    if (n < 2) return 0;
    if (n % 2 == 0) return n == 2;
    for (unsigned d = 3; d * d <= n; d += 2)
        if (n % d == 0) return 0;
    return 1;
}

int app_main(int argc, char **argv)
{
    unsigned limit = argc > 1 ? (unsigned)api_atoi(argv[1]) : 0;   /* 0 = forever */
    unsigned found = 0, n = 1;
    unsigned t0 = api->ms(), last = t0;

    api->printf("  hunting primes%s. ctrl-c stops me.\n",
                limit ? "" : " until you stop me");

    for (;;) {
        n++;
        if (!is_prime(n)) continue;
        found++;
        if ((found & 255) == 0 && api->ms() - last >= 500) {
            last = api->ms();
            api->printf("  %u primes, highest %u, %u ms\n",
                        found, n, last - t0);
        }
        if (limit && found >= limit) break;
    }

    api->printf("  done: %u primes below %u in %u ms\n",
                found, n, api->ms() - t0);
    return 0;
}
