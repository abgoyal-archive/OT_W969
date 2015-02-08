/*	$NetBSD: localconf.c,v 1.4 2006/09/09 16:22:09 manu Exp $	*/

/*	$KAME: localconf.c,v 1.33 2001/08/09 07:32:19 sakane Exp $	*/


#include "config.h"

#include <sys/types.h>
#include <sys/param.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <err.h>

#include "var.h"
#include "misc.h"
#include "vmbuf.h"
#include "plog.h"
#include "debug.h"

#include "localconf.h"
#include "algorithm.h"
#include "admin.h"
#include "privsep.h"
#include "isakmp_var.h"
#include "isakmp.h"
#include "ipsec_doi.h"
#include "grabmyaddr.h"
#include "vendorid.h"
#include "str2val.h"
#include "safefile.h"
#include "admin.h"
#include "gcmalloc.h"

struct localconf *lcconf;

static void setdefault __P((void));

void
initlcconf()
{
	lcconf = racoon_calloc(1, sizeof(*lcconf));
	if (lcconf == NULL)
		errx(1, "failed to allocate local conf.");

	setdefault();

	lcconf->racoon_conf = LC_DEFAULT_CF;
}

void
flushlcconf()
{
	int i;

	setdefault();
	clear_myaddr(&lcconf->myaddrs);
	for (i = 0; i < LC_PATHTYPE_MAX; i++) {
		if (lcconf->pathinfo[i]) {
			racoon_free(lcconf->pathinfo[i]);
			lcconf->pathinfo[i] = NULL;
		}
	}
	for (i = 0; i < LC_IDENTTYPE_MAX; i++) {
		if (lcconf->ident[i])
			vfree(lcconf->ident[i]);
		lcconf->ident[i] = NULL;
	}
}

static void
setdefault()
{
	lcconf->uid = 0;
	lcconf->gid = 0;
	lcconf->chroot = NULL;
	lcconf->autograbaddr = 1;
	lcconf->port_isakmp = PORT_ISAKMP;
	lcconf->port_isakmp_natt = PORT_ISAKMP_NATT;
	lcconf->default_af = AF_INET;
	lcconf->pad_random = LC_DEFAULT_PAD_RANDOM;
	lcconf->pad_randomlen = LC_DEFAULT_PAD_RANDOMLEN;
	lcconf->pad_maxsize = LC_DEFAULT_PAD_MAXSIZE;
	lcconf->pad_strict = LC_DEFAULT_PAD_STRICT;
	lcconf->pad_excltail = LC_DEFAULT_PAD_EXCLTAIL;
	lcconf->retry_counter = LC_DEFAULT_RETRY_COUNTER;
	lcconf->retry_interval = LC_DEFAULT_RETRY_INTERVAL;
	lcconf->count_persend = LC_DEFAULT_COUNT_PERSEND;
	lcconf->secret_size = LC_DEFAULT_SECRETSIZE;
	lcconf->retry_checkph1 = LC_DEFAULT_RETRY_CHECKPH1;
	lcconf->wait_ph2complete = LC_DEFAULT_WAIT_PH2COMPLETE;
	lcconf->strict_address = FALSE;
	lcconf->complex_bundle = TRUE; /*XXX FALSE;*/
	lcconf->gss_id_enc = LC_GSSENC_UTF16LE; /* Windows compatibility */
	lcconf->natt_ka_interval = LC_DEFAULT_NATT_KA_INTERVAL;
}

vchar_t *
getpskbyname(id0)
	vchar_t *id0;
{
	char *id;
	vchar_t *key = NULL;

	id = racoon_calloc(1, 1 + id0->l - sizeof(struct ipsecdoi_id_b));
	if (id == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get psk buffer.\n");
		goto end;
	}
	memcpy(id, id0->v + sizeof(struct ipsecdoi_id_b),
		id0->l - sizeof(struct ipsecdoi_id_b));
	id[id0->l - sizeof(struct ipsecdoi_id_b)] = '\0';

	key = privsep_getpsk(id, id0->l - sizeof(struct ipsecdoi_id_b));

end:
	if (id)
		racoon_free(id);
	return key;
}

vchar_t *
getpskbyaddr(remote)
	struct sockaddr *remote;
{
	vchar_t *key = NULL;
	char addr[NI_MAXHOST], port[NI_MAXSERV];

	GETNAMEINFO(remote, addr, port);

	key = privsep_getpsk(addr, strlen(addr));

	return key;
}

vchar_t *
getpsk(str, len)
	const char *str;
	const int len;
{
	FILE *fp;
	char buf[1024];	/* XXX how is variable length ? */
	vchar_t *key = NULL;
	char *p, *q;
	size_t keylen;
	char *k = NULL;

	if (safefile(lcconf->pathinfo[LC_PATHTYPE_PSK], 1) == 0)
		fp = fopen(lcconf->pathinfo[LC_PATHTYPE_PSK], "r");
	else
		fp = NULL;
	if (fp == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to open pre_share_key file %s\n",
			lcconf->pathinfo[LC_PATHTYPE_PSK]);
		return NULL;
	}

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		/* comment line */
		if (buf[0] == '#')
			continue;

		/* search the end of 1st string. */
		for (p = buf; *p != '\0' && !isspace((int)*p); p++)
			;
		if (*p == '\0')
			continue;	/* no 2nd parameter */
		*p = '\0';
		/* search the 1st of 2nd string. */
		while (isspace((int)*++p))
			;
		if (*p == '\0')
			continue;	/* no 2nd parameter */
		p--;
		if (strncmp(buf, str, len) == 0 && buf[len] == '\0') {
			p++;
			keylen = 0;
			for (q = p; *q != '\0' && *q != '\n'; q++)
				keylen++;
			*q = '\0';

			/* fix key if hex string */
			if (strncmp(p, "0x", 2) == 0) {
				k = str2val(p + 2, 16, &keylen);
				if (k == NULL) {
					plog(LLV_ERROR, LOCATION, NULL,
						"failed to get psk buffer.\n");
					goto end;
				}
				p = k;
			}

			key = vmalloc(keylen);
			if (key == NULL) {
				plog(LLV_ERROR, LOCATION, NULL,
					"failed to allocate key buffer.\n");
				goto end;
			}
			memcpy(key->v, p, key->l);
			if (k)
				racoon_free(k);
			goto end;
		}
	}

end:
	fclose(fp);
	return key;
}

void
getpathname(path, len, type, name)
	char *path;
	int len, type;
	const char *name;
{
	snprintf(path, len, "%s%s%s", 
		name[0] == '/' ? "" : lcconf->pathinfo[type],
		name[0] == '/' ? "" : "/",
		name);

	plog(LLV_DEBUG, LOCATION, NULL, "filename: %s\n", path);
}

#if 0 /* DELETEIT */
static int lc_doi2idtype[] = {
	-1,
	-1,
	LC_IDENTTYPE_FQDN,
	LC_IDENTTYPE_USERFQDN,
	-1,
	-1,
	-1,
	-1,
	-1,
	LC_IDENTTYPE_CERTNAME,
	-1,
	LC_IDENTTYPE_KEYID,
};

int
doi2idtype(idtype)
	int idtype;
{
	if (ARRAYLEN(lc_doi2idtype) > idtype)
		return lc_doi2idtype[idtype];
	return -1;
}
#endif

static int lc_sittype2doi[] = {
	IPSECDOI_SIT_IDENTITY_ONLY,
	IPSECDOI_SIT_SECRECY,
	IPSECDOI_SIT_INTEGRITY,
};

int
sittype2doi(sittype)
	int sittype;
{
	if (ARRAYLEN(lc_sittype2doi) > sittype)
		return lc_sittype2doi[sittype];
	return -1;
}

static int lc_doitype2doi[] = {
	IPSEC_DOI,
};

int
doitype2doi(doitype)
	int doitype;
{
	if (ARRAYLEN(lc_doitype2doi) > doitype)
		return lc_doitype2doi[doitype];
	return -1;
}



static void
saverestore_params(f)
	int f;
{
	static u_int16_t s_port_isakmp;
#ifdef ENABLE_ADMINPORT
	static u_int16_t s_port_admin;
#endif

	/* 0: save, 1: restore */
	if (f) {
		lcconf->port_isakmp = s_port_isakmp;
#ifdef ENABLE_ADMINPORT
		lcconf->port_admin = s_port_admin;
#endif
	} else {
		s_port_isakmp = lcconf->port_isakmp;
#ifdef ENABLE_ADMINPORT
		s_port_admin = lcconf->port_admin;
#endif
	}
}

void
restore_params()
{
	saverestore_params(1);
}

void
save_params()
{
	saverestore_params(0);
}