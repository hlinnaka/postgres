/* src/port/getopt.c */

/*
 * Copyright (c) 1987, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 */

#include "c.h"

#include "pg_getopt.h"

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)getopt.c	8.3 (Berkeley) 4/27/95";
#endif							/* LIBC_SCCS and not lint */

#define BADCH	(int)'?'
#define BADARG	(int)':'
#define EMSG	""

/*
 * getopt
 *	Parse argc/argv argument vector.
 *
 * XXX This implementation does not use optreset.  Instead, we guarantee that
 * it can be restarted on a new argv array after a previous call returned -1,
 * if the caller resets optind to 1 before the first call of the new series.
 * (Internally, this means we must be sure to reset "place" to EMSG before
 * returning -1.)
 */
void
pg_getopt_start(pg_getopt_ctx *ctx, int nargc, char *const *nargv, const char *ostr)
{
	ctx->nargc = nargc;
	ctx->nargv = nargv;
	ctx->ostr = ostr;

	ctx->place = EMSG;	/* option letter processing */
}

int
pg_getopt_next(pg_getopt_ctx *ctx)
{
	char	   *oli;			/* option letter list index */
	
	if (!*ctx->place)
	{							/* update scanning pointer */
		if (ctx->optind >= ctx->nargc || *(ctx->place = ctx->nargv[ctx->optind]) != '-')
		{
			ctx->place = EMSG;
			return -1;
		}
		if (ctx->place[1] && *++ctx->place == '-' && ctx->place[1] == '\0')
		{						/* found "--" */
			++ctx->optind;
			ctx->place = EMSG;
			return -1;
		}
	}							/* option letter okay? */
	if ((ctx->optopt = (int) *ctx->place++) == (int) ':' ||
		!(oli = strchr(ctx->ostr, ctx->optopt)))
	{
		/*
		 * if the user didn't specify '-' as an option, assume it means -1.
		 */
		if (ctx->optopt == (int) '-')
		{
			ctx->place = EMSG;
			return -1;
		}
		if (!*ctx->place)
			++ctx->optind;
		if (ctx->opterr && *ctx->ostr != ':')
			(void) fprintf(stderr,
						   "illegal option -- %c\n", ctx->optopt);
		return BADCH;
	}
	if (*++oli != ':')
	{							/* don't need argument */
		ctx->optarg = NULL;
		if (!*ctx->place)
			++ctx->optind;
	}
	else
	{							/* need an argument */
		if (*ctx->place)				/* no white space */
			ctx->optarg = ctx->place;
		else if (ctx->nargc <= ++ctx->optind)
		{						/* no arg */
			ctx->place = EMSG;
			if (*ctx->ostr == ':')
				return BADARG;
			if (ctx->opterr)
				(void) fprintf(stderr,
							   "option requires an argument -- %c\n",
							   ctx->optopt);
			return BADCH;
		}
		else
			/* white space */
			ctx->optarg = ctx->nargv[ctx->optind];
		ctx->place = EMSG;
		++ctx->optind;
	}
	return ctx->optopt;				/* dump back option letter */
}
