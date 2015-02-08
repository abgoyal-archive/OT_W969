

#include <linux/kernel.h>
#include <linux/console.h>

#include <asm/machdep.h>
#include <asm/prom.h>
#include <asm/udbg.h>

#include "beat.h"

#define	celleb_vtermno	0

static void udbg_putc_beat(char c)
{
	unsigned long rc;

	if (c == '\n')
		udbg_putc_beat('\r');

	rc = beat_put_term_char(celleb_vtermno, 1, (uint64_t)c << 56, 0);
}

/* Buffered chars getc */
static u64 inbuflen;
static u64 inbuf[2];	/* must be 2 u64s */

static int udbg_getc_poll_beat(void)
{
	/* The interface is tricky because it may return up to 16 chars.
	 * We save them statically for future calls to udbg_getc().
	 */
	char ch, *buf = (char *)inbuf;
	int i;
	long rc;
	if (inbuflen == 0) {
		/* get some more chars. */
		inbuflen = 0;
		rc = beat_get_term_char(celleb_vtermno, &inbuflen,
					inbuf+0, inbuf+1);
		if (rc != 0)
			inbuflen = 0;	/* otherwise inbuflen is garbage */
	}
	if (inbuflen <= 0 || inbuflen > 16) {
		/* Catch error case as well as other oddities (corruption) */
		inbuflen = 0;
		return -1;
	}
	ch = buf[0];
	for (i = 1; i < inbuflen; i++)	/* shuffle them down. */
		buf[i-1] = buf[i];
	inbuflen--;
	return ch;
}

static int udbg_getc_beat(void)
{
	int ch;
	for (;;) {
		ch = udbg_getc_poll_beat();
		if (ch == -1) {
			/* This shouldn't be needed...but... */
			volatile unsigned long delay;
			for (delay = 0; delay < 2000000; delay++)
				;
		} else {
			return ch;
		}
	}
}

void __init udbg_init_debug_beat(void)
{
	udbg_putc = udbg_putc_beat;
	udbg_getc = udbg_getc_beat;
	udbg_getc_poll = udbg_getc_poll_beat;
}