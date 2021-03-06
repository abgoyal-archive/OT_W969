
#include "defs.h"

#include <fcntl.h>
#if HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#ifdef HAVE_LONG_LONG_OFF_T

#define sys_pread64	sys_pread
#define sys_pwrite64	sys_pwrite
#endif

int
sys_read(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
	} else {
		if (syserror(tcp))
			tprintf("%#lx", tcp->u_arg[1]);
		else
			printstr(tcp, tcp->u_arg[1], tcp->u_rval);
		tprintf(", %lu", tcp->u_arg[2]);
	}
	return 0;
}

int
sys_write(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
		printstr(tcp, tcp->u_arg[1], tcp->u_arg[2]);
		tprintf(", %lu", tcp->u_arg[2]);
	}
	return 0;
}

#if HAVE_SYS_UIO_H
void
tprint_iov(tcp, len, addr)
struct tcb * tcp;
unsigned long len;
unsigned long addr;
{
	struct iovec iov;
	unsigned long size, cur, end, abbrev_end;
	int failed = 0;

	if (!len) {
		tprintf("[]");
		return;
	}
	size = len * sizeof(iov);
	end = addr + size;
	if (!verbose(tcp) || size / sizeof(iov) != len || end < addr) {
		tprintf("%#lx", addr);
		return;
	}
	if (abbrev(tcp)) {
		abbrev_end = addr + max_strlen * sizeof(iov);
		if (abbrev_end < addr)
			abbrev_end = end;
	} else {
		abbrev_end = end;
	}
	tprintf("[");
	for (cur = addr; cur < end; cur += sizeof(iov)) {
		if (cur > addr)
			tprintf(", ");
		if (cur >= abbrev_end) {
			tprintf("...");
			break;
		}
		if (umoven(tcp, cur, sizeof iov, (char *) &iov) < 0) {
			tprintf("?");
			failed = 1;
			break;
		}
		tprintf("{");
		printstr(tcp, (long) iov.iov_base, iov.iov_len);
		tprintf(", %lu}", (unsigned long)iov.iov_len);
	}
	tprintf("]");
	if (failed)
		tprintf(" %#lx", addr);
}

int
sys_readv(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
	} else {
		if (syserror(tcp)) {
			tprintf("%#lx, %lu",
					tcp->u_arg[1], tcp->u_arg[2]);
			return 0;
		}
		tprint_iov(tcp, tcp->u_arg[2], tcp->u_arg[1]);
		tprintf(", %lu", tcp->u_arg[2]);
	}
	return 0;
}

int
sys_writev(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
		tprint_iov(tcp, tcp->u_arg[2], tcp->u_arg[1]);
		tprintf(", %lu", tcp->u_arg[2]);
	}
	return 0;
}
#endif

#if defined(SVR4)

int
sys_pread(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
	} else {
		if (syserror(tcp))
			tprintf("%#lx", tcp->u_arg[1]);
		else
			printstr(tcp, tcp->u_arg[1], tcp->u_rval);
#if UNIXWARE
		/* off_t is signed int */
		tprintf(", %lu, %ld", tcp->u_arg[2], tcp->u_arg[3]);
#else
		tprintf(", %lu, %llu", tcp->u_arg[2],
			LONG_LONG(tcp->u_arg[3], tcp->u_arg[4]));
#endif
	}
	return 0;
}

int
sys_pwrite(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
		printstr(tcp, tcp->u_arg[1], tcp->u_arg[2]);
#if UNIXWARE
		/* off_t is signed int */
		tprintf(", %lu, %ld", tcp->u_arg[2], tcp->u_arg[3]);
#else
		tprintf(", %lu, %llu", tcp->u_arg[2],
			LONG_LONG(tcp->u_arg[3], tcp->u_arg[4]));
#endif
	}
	return 0;
}
#endif /* SVR4 */

#ifdef FREEBSD
#include <sys/types.h>
#include <sys/socket.h>

int
sys_sendfile(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, %ld, %llu, %lu", tcp->u_arg[0], tcp->u_arg[1],
			LONG_LONG(tcp->u_arg[2], tcp->u_arg[3]),
			tcp->u_arg[4]);
	} else {
		off_t offset;

		if (!tcp->u_arg[5])
			tprintf(", NULL");
		else {
			struct sf_hdtr hdtr;

			if (umove(tcp, tcp->u_arg[5], &hdtr) < 0)
				tprintf(", %#lx", tcp->u_arg[5]);
			else {
				tprintf(", { ");
				tprint_iov(tcp, hdtr.hdr_cnt, hdtr.headers);
				tprintf(", %u, ", hdtr.hdr_cnt);
				tprint_iov(tcp, hdtr.trl_cnt, hdtr.trailers);
				tprintf(", %u }", hdtr.hdr_cnt);
			}
		}
		if (!tcp->u_arg[6])
			tprintf(", NULL");
		else if (umove(tcp, tcp->u_arg[6], &offset) < 0)
			tprintf(", %#lx", tcp->u_arg[6]);
		else
			tprintf(", [%llu]", offset);
		tprintf(", %lu", tcp->u_arg[7]);
	}
	return 0;
}
#endif /* FREEBSD */

#ifdef LINUX

#if defined(SH)
#define PREAD_OFFSET_ARG 4
#else
#define PREAD_OFFSET_ARG 3
#endif

int
sys_pread(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
	} else {
		if (syserror(tcp))
			tprintf("%#lx", tcp->u_arg[1]);
		else
			printstr(tcp, tcp->u_arg[1], tcp->u_rval);
		ALIGN64 (tcp, PREAD_OFFSET_ARG); /* PowerPC alignment restriction */
		tprintf(", %lu, %llu", tcp->u_arg[2],
			*(unsigned long long *)&tcp->u_arg[PREAD_OFFSET_ARG]);
	}
	return 0;
}

int
sys_pwrite(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
		printstr(tcp, tcp->u_arg[1], tcp->u_arg[2]);
		ALIGN64 (tcp, PREAD_OFFSET_ARG); /* PowerPC alignment restriction */
		tprintf(", %lu, %llu", tcp->u_arg[2],
			*(unsigned long long *)&tcp->u_arg[PREAD_OFFSET_ARG]);
	}
	return 0;
}

int
sys_sendfile(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		off_t offset;

		tprintf("%ld, %ld, ", tcp->u_arg[0], tcp->u_arg[1]);
		if (!tcp->u_arg[2])
			tprintf("NULL");
		else if (umove(tcp, tcp->u_arg[2], &offset) < 0)
			tprintf("%#lx", tcp->u_arg[2]);
		else
			tprintf("[%lu]", offset);
		tprintf(", %lu", tcp->u_arg[3]);
	}
	return 0;
}

int
sys_sendfile64(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		loff_t offset;

		tprintf("%ld, %ld, ", tcp->u_arg[0], tcp->u_arg[1]);
		if (!tcp->u_arg[2])
			tprintf("NULL");
		else if (umove(tcp, tcp->u_arg[2], &offset) < 0)
			tprintf("%#lx", tcp->u_arg[2]);
		else
			tprintf("[%llu]", (unsigned long long int) offset);
		tprintf(", %lu", tcp->u_arg[3]);
	}
	return 0;
}

#endif /* LINUX */

#if _LFS64_LARGEFILE || HAVE_LONG_LONG_OFF_T
int
sys_pread64(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
	} else {
		ALIGN64 (tcp, 3);
		if (syserror(tcp))
			tprintf("%#lx", tcp->u_arg[1]);
		else
			printstr(tcp, tcp->u_arg[1], tcp->u_rval);
		tprintf(", %lu, %#llx", tcp->u_arg[2],
			LONG_LONG(tcp->u_arg[3], tcp->u_arg[4]));
	}
	return 0;
}

int
sys_pwrite64(tcp)
struct tcb *tcp;
{
	if (entering(tcp)) {
		ALIGN64 (tcp, 3);
		tprintf("%ld, ", tcp->u_arg[0]);
		printstr(tcp, tcp->u_arg[1], tcp->u_arg[2]);
		tprintf(", %lu, %#llx", tcp->u_arg[2],
			LONG_LONG(tcp->u_arg[3], tcp->u_arg[4]));
	}
	return 0;
}
#endif

int
sys_ioctl(tcp)
struct tcb *tcp;
{
	const struct ioctlent *iop;

	if (entering(tcp)) {
		tprintf("%ld, ", tcp->u_arg[0]);
		iop = ioctl_lookup(tcp->u_arg[1]);
		if (iop) {
			tprintf("%s", iop->symbol);
			while ((iop = ioctl_next_match(iop)))
				tprintf(" or %s", iop->symbol);
		} else
			tprintf("%#lx", tcp->u_arg[1]);
		ioctl_decode(tcp, tcp->u_arg[1], tcp->u_arg[2]);
	}
	else {
		int ret;
		if (!(ret = ioctl_decode(tcp, tcp->u_arg[1], tcp->u_arg[2])))
			tprintf(", %#lx", tcp->u_arg[2]);
		else
			return ret - 1;
	}
	return 0;
}
