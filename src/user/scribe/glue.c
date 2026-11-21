/* glue.c — Scribe's C-library pieces: the same heap, errno and stubs as Reel's
 * (src/user/reel/glue.c), renamed. The old first-fit draft is gone. */
#define reel_fatal     scribe_fatal
#define reel_heap_init scribe_heap_init
#define reel_heap_used scribe_heap_used
#define reel_heap_peak scribe_heap_peak
#include "../reel/glue.c"
