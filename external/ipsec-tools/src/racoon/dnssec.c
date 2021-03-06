/*	$NetBSD: dnssec.c,v 1.4 2006/09/09 16:22:09 manu Exp $	*/

/*	$KAME: dnssec.c,v 1.2 2001/08/05 18:46:07 itojun Exp $	*/


#include "config.h"

#include <sys/types.h>
#include <sys/param.h>
#include <stdlib.h>
#include <string.h>

#include "var.h"
#include "vmbuf.h"
#include "misc.h"
#include "plog.h"
#include "debug.h"

#include "isakmp_var.h"
#include "isakmp.h"
#include "ipsec_doi.h"
#include "oakley.h"
#include "netdb_dnssec.h"
#include "strnames.h"
#include "dnssec.h"
#include "gcmalloc.h"

extern int h_errno;

cert_t *
dnssec_getcert(id)
	vchar_t *id;
{
	cert_t *cert = NULL;
	struct certinfo *res = NULL;
	struct ipsecdoi_id_b *id_b;
	int type;
	char *name = NULL;
	int namelen;
	int error;

	id_b = (struct ipsecdoi_id_b *)id->v;

	namelen = id->l - sizeof(*id_b);
	name = racoon_malloc(namelen + 1);
	if (!name) {
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get buffer.\n");
		return NULL;
	}
	memcpy(name, id_b + 1, namelen);
	name[namelen] = '\0';

	switch (id_b->type) {
	case IPSECDOI_ID_FQDN:
		error = getcertsbyname(name, &res);
		if (error != 0) {
			plog(LLV_ERROR, LOCATION, NULL,
				"getcertsbyname(\"%s\") failed.\n", name);
			goto err;
		}
		break;
	case IPSECDOI_ID_IPV4_ADDR:
	case IPSECDOI_ID_IPV6_ADDR:
		/* XXX should be processed to query PTR ? */
	default:
		plog(LLV_ERROR, LOCATION, NULL,
			"inpropper ID type passed %s "
			"though getcert method is dnssec.\n",
			s_ipsecdoi_ident(id_b->type));
		goto err;
	}

	/* check response */
	if (res->ci_next != NULL) {
		plog(LLV_WARNING, LOCATION, NULL,
			"not supported multiple CERT RR.\n");
	}
	switch (res->ci_type) {
	case DNSSEC_TYPE_PKIX:
		/* XXX is it enough condition to set this type ? */
		type = ISAKMP_CERT_X509SIGN;
		break;
	default:
		plog(LLV_ERROR, LOCATION, NULL,
			"not supported CERT RR type %d.\n", res->ci_type);
		goto err;
	}

	/* create cert holder */
	cert = oakley_newcert();
	if (cert == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get cert buffer.\n");
		goto err;
	}
	cert->pl = vmalloc(res->ci_certlen + 1);
	if (cert->pl == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get cert buffer.\n");
		goto err;
	}
	memcpy(cert->pl->v + 1, res->ci_cert, res->ci_certlen);
	cert->pl->v[0] = type;
	cert->cert.v = cert->pl->v + 1;
	cert->cert.l = cert->pl->l - 1;

	plog(LLV_DEBUG, LOCATION, NULL, "created CERT payload:\n");
	plogdump(LLV_DEBUG, cert->pl->v, cert->pl->l);

end:
	if (res)
		freecertinfo(res);

	return cert;

err:
	if (name)
		racoon_free(name);
	if (cert) {
		oakley_delcert(cert);
		cert = NULL;
	}

	goto end;
}
