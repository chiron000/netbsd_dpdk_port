/*	$NetBSD: npf_ext_normalise.c,v 1.1.4.3 2013/02/08 19:18:11 riz Exp $	*/

/*-
 * Copyright (c) 2009-2012 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <special_includes/sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: npf_ext_normalise.c,v 1.1.4.3 2013/02/08 19:18:11 riz Exp $");

#include <special_includes/sys/types.h>
#include <special_includes/sys/module.h>
#include <special_includes/sys/kmem.h>

#include <net/if.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/in_var.h>

#include "npf.h"
#include "npf_impl.h"

/*
 * NPF extension module definition and the identifier.
 */
NPF_EXT_MODULE(npf_ext_normalise, "");

#define	NPFEXT_NORMALISE_VER	1

static void *		npf_ext_normalise_id;

/*
 * Normalisation parameters.
 */
typedef struct {
	u_int		n_minttl;
	u_int		n_maxmss;
	bool		n_random_id;
	bool		n_no_df;
} npf_normalise_t;

/*
 * npf_normalise_ctor: a constructor for the normalisation rule procedure
 * with the given parameters.
 */
static int
npf_normalise_ctor(npf_rproc_t *rp, prop_dictionary_t params)
{
	npf_normalise_t *np;

	/* Create a structure for normalisation parameters. */
	np = kmem_zalloc(sizeof(npf_normalise_t), KM_SLEEP);

	/* IP ID randomisation and IP_DF flag cleansing. */
	prop_dictionary_get_bool(params, "random-id", &np->n_random_id);
	prop_dictionary_get_bool(params, "no-df", &np->n_no_df);

	/* Minimum IP TTL and maximum TCP MSS. */
	prop_dictionary_get_uint32(params, "min-ttl", &np->n_minttl);
	prop_dictionary_get_uint32(params, "max-mss", &np->n_maxmss);

	/* Assign the parameters for this rule procedure. */
	npf_rproc_assign(rp, np);
	return 0;
}

/*
 * npf_normalise_dtor: a destructor for a normalisation rule procedure.
 */
static void
npf_normalise_dtor(npf_rproc_t *rp, void *params)
{
	/* Free our meta-data, associated with the procedure. */
	kmem_free(params, sizeof(npf_normalise_t));
}

/*
 * npf_normalise_ip4: routine to normalise IPv4 header (randomise ID,
 * clear "don't fragment" and/or enforce minimum TTL).
 */
static inline void
npf_normalise_ip4(npf_cache_t *npc, npf_normalise_t *np)
{
	struct ip *ip = npc->npc_ip.v4;
	uint16_t cksum = ip->ip_sum;
	uint16_t ip_off = ip->ip_off;
	uint8_t ttl = ip->ip_ttl;
	u_int minttl = np->n_minttl;

	KASSERT(np->n_random_id || np->n_no_df || minttl);

	/* Randomise IPv4 ID. */
	if (np->n_random_id) {
		uint16_t oid = ip->ip_id, nid;

		nid = htons(ip_randomid(ip_ids, 0));
		cksum = npf_fixup16_cksum(cksum, oid, nid);
		ip->ip_id = nid;
	}

	/* IP_DF flag cleansing. */
	if (np->n_no_df && (ip_off & htons(IP_DF)) != 0) {
		uint16_t nip_off = ip_off & ~htons(IP_DF);

		cksum = npf_fixup16_cksum(cksum, ip_off, nip_off);
		ip->ip_off = nip_off;
	}

	/* Enforce minimum TTL. */
	if (minttl && ttl < minttl) {
		cksum = npf_fixup16_cksum(cksum, ttl, minttl);
		ip->ip_ttl = minttl;
	}

	/* Update IPv4 checksum. */
	ip->ip_sum = cksum;
}

/*
 * npf_normalise: the main routine to normalise IPv4 and/or TCP headers.
 */
static void
npf_normalise(npf_cache_t *npc, nbuf_t *nbuf, void *params, int *decision)
{
	npf_normalise_t *np = params;
	struct tcphdr *th = npc->npc_l4.tcp;
	uint16_t cksum, mss, maxmss = np->n_maxmss;
	int wscale;

	/* Skip, if already blocking. */
	if (*decision == NPF_DECISION_BLOCK) {
		return;
	}

	/* Normalise IPv4.  Nothing to do for IPv6. */
	if (npf_iscached(npc, NPC_IP4) && (np->n_random_id || np->n_minttl)) {
		npf_normalise_ip4(npc, np);
	}

	/*
	 * TCP Maximum Segment Size (MSS) "clamping".  Only if SYN packet.
	 * Fetch MSS and check whether rewrite to lower is needed.
	 */
	if (maxmss == 0 || !npf_iscached(npc, NPC_TCP) ||
	    (th->th_flags & TH_SYN) == 0) {
		/* Not required; done. */
		return;
	}
	mss = 0;
	if (!npf_fetch_tcpopts(npc, nbuf, &mss, &wscale)) {
		return;
	}
	if (ntohs(mss) <= maxmss) {
		/* Nothing else to do. */
		return;
	}
	maxmss = htons(maxmss);

	/* Store new MSS, calculate TCP checksum and update it. */
	if (npf_fetch_tcpopts(npc, nbuf, &maxmss, &wscale)) {
		cksum = npf_fixup16_cksum(th->th_sum, mss, maxmss);
		th->th_sum = cksum;
	}
}

static int
npf_ext_normalise_modcmd(modcmd_t cmd, void *arg)
{
	static const npf_ext_ops_t npf_normalise_ops = {
		.version	= NPFEXT_NORMALISE_VER,
		.ctx		= NULL,
		.ctor		= npf_normalise_ctor,
		.dtor		= npf_normalise_dtor,
		.proc		= npf_normalise
	};

	switch (cmd) {
	case MODULE_CMD_INIT:
		/*
		 * Initialise normalisation module.  Register the "normalise"
		 * extension and its calls.
		 */
		npf_ext_normalise_id =
		    npf_ext_register("normalise", &npf_normalise_ops);
		return npf_ext_normalise_id ? 0 : EEXIST;

	case MODULE_CMD_FINI:
		/* Unregister the normalisation rule procedure. */
		return npf_ext_unregister(npf_ext_normalise_id);

	case MODULE_CMD_AUTOUNLOAD:
		return npf_autounload_p() ? 0 : EBUSY;

	default:
		return ENOTTY;
	}
	return 0;
}
