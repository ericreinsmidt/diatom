/* Does Diatom allocate inside its frame loop?
 *
 * §11 is the project's stated thesis and its "no malloc in the frame loop" item
 * said **Unverified. Believed true.** This is what turns that into a number.
 *
 * Linked with `-Wl,--wrap=malloc` and friends, which rewrites those calls in
 * the objects being LINKED. A core arrives by `dlopen` and binds its own malloc
 * through libc, so it is invisible here - and that is exactly right, because
 * the claim under test is that *Diatom* does not allocate per frame, not that
 * no code anywhere does. Cores allocate; several of them must.
 *
 * The measurement is DIFFERENTIAL, not absolute. Run N frames and 2N frames:
 * start-up allocations are identical in both, so anything the frame loop does
 * shows up as the difference. That is what lets this exist entirely outside
 * Diatom - no counter to arm, no hook to call, nothing test-shaped in the
 * shipped binary. The same reason the crash fixture lives in stubcore rather
 * than behind a `--crash` flag.
 *
 * An instrument. See tools/README.md.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void  __real_free(void *);

static unsigned long n_malloc, n_calloc, n_realloc, n_free;
static unsigned long long bytes;

void *__wrap_malloc(size_t n)             { n_malloc++;  bytes += n;     return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b)   { n_calloc++;  bytes += a * b; return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n)   { n_realloc++; bytes += n;     return __real_realloc(p, n); }
void  __wrap_free(void *p)                { if (p) n_free++;             __real_free(p); }

/* stderr, and unbuffered: the harness kills long runs, and a block-buffered
 * report is a report that is not there when it matters. That cost an evening
 * once already. */
__attribute__((destructor))
static void report(void)
{
	fprintf(stderr,
	        "allocwatch: malloc=%lu calloc=%lu realloc=%lu free=%lu bytes=%llu total=%lu\n",
	        n_malloc, n_calloc, n_realloc, n_free, bytes,
	        n_malloc + n_calloc + n_realloc);
	fflush(stderr);
}
