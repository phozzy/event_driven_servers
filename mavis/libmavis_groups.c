/*
 * libmavis_groups.c
 * (C)2011 by Marc Huber <Marc.Huber@web.de>
 * All rights reserved.
 *
 * $Id: libmavis_groups.c,v 1.19 2015/03/14 06:11:28 marc Exp $
 *
 */

#define __MAVIS_groups__
#include "misc/sysconf.h"
#include <stdio.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <grp.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include "misc/strops.h"
#include "misc/io.h"
#include "groups.h"
#include "misc/memops.h"
#include "debug.h"
#include "log.h"

#ifdef WITH_PCRE
# include <pcre.h>
#endif

#include <regex.h>

static const char rcsid[] __attribute__ ((used)) = "$Id: libmavis_groups.c,v 1.19 2015/03/14 06:11:28 marc Exp $";


struct regex_list;

#define MAVIS_CTX_PRIVATE	int resolve_gid; \
				int resolve_gids; \
				struct regex_list *group_regex; \
				struct regex_list *groups_regex; \
				struct gid_list *gid; \
				struct gid_list *gids;

#include "mavis.h"

struct regex_list {
    struct regex_list *next;
    int negate;
    enum token type;
    void *p;
};

struct gid_list {
    struct gid_list *next;
    int negate;
    gid_t gid_start;
    gid_t gid_end;
};

static void parse_filter_regex(struct sym *sym, struct regex_list **l)
{
    int negate = 0;

    if (sym->code == S_not) {
	negate = 1;
	sym_get(sym);
    }

    do {
	int errcode = 0;
	while (*l)
	    l = &(*l)->next;
	*l = Xcalloc(1, sizeof(struct regex_list));
	(*l)->negate = negate;
#ifdef WITH_PCRE
	if (sym->code == S_slash) {
	    int erroffset;
	    const char *errptr;
	    (*l)->type = S_slash;
	    (*l)->p = (void *)
		pcre_compile2(sym->buf, PCRE_MULTILINE | common_data.regex_pcre_flags, &errcode, &errptr, &erroffset, NULL);
	    if (!(*l)->p)
		parse_error(sym, "In PCRE expression /%s/ at offset %d: %s", sym->buf, erroffset, errptr);
	} else
#endif
	{
	    (*l)->type = S_regex;
	    (*l)->p = Xcalloc(1, sizeof(regex_t));
	    errcode = regcomp((regex_t *) (*l)->p, sym->buf, REG_EXTENDED | REG_NOSUB | REG_NEWLINE | common_data.regex_posix_flags);
	    if (errcode) {
		char e[160];
		regerror(errcode, (regex_t *) (*l)->p, e, sizeof(e));
		parse_error(sym, "In regular expression '%s': %s", sym->buf, e);
	    }
	}
	sym_get(sym);
    } while (parse_comma(sym));
}

static void parse_gid(struct sym *sym, struct gid_list **l)
{
    int negate = 0;
    if (sym->code == S_not) {
	negate = 1;
	sym_get(sym);
    }

    do {
	u_int gs, ge;
	while (*l)
	    l = &(*l)->next;
	*l = Xcalloc(1, sizeof(struct gid_list));
	(*l)->negate = negate;

	switch (sscanf(sym->buf, "%u-%u", &gs, &ge)) {
	case 1:
	    ge = gs;
	case 2:
	    break;
	default:
	    parse_error(sym, "Expected numeric GID or GID range, but got \"%s\"", sym->buf);
	}
	(*l)->gid_start = gs;
	(*l)->gid_end = ge;
	sym_get(sym);
    } while (parse_comma(sym));
}

#define HAVE_mavis_parse_in
static int mavis_parse_in(mavis_ctx * mcx, struct sym *sym)
{
    while (1) {
	switch (sym->code) {
	case S_script:
	    mavis_script_parse(mcx, sym);
	    continue;
	case S_resolve:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_gid:
		// resolve gid = (yes|no)
		sym_get(sym);
		parse(sym, S_equal);
		mcx->resolve_gid = parse_bool(sym);
		continue;
	    case S_gids:
		// resolve gids = (yes|no)
		sym_get(sym);
		parse(sym, S_equal);
		mcx->resolve_gids = parse_bool(sym);
		continue;
	    default:
		parse_error_expect(sym, S_gid, S_gids, S_unknown);
	    }
	    continue;
	case S_group:
	    sym_get(sym);
	    parse(sym, S_filter);
	    sym->flag_parse_pcre = 1;
	    parse(sym, S_equal);
	    // group filter = [not] regex ...
	    parse_filter_regex(sym, &mcx->group_regex);
	    sym->flag_parse_pcre = 0;
	    continue;
	case S_groups:
	    sym_get(sym);
	    parse(sym, S_filter);
	    sym->flag_parse_pcre = 1;
	    parse(sym, S_equal);
	    // groups filter = [not] regex ...
	    parse_filter_regex(sym, &mcx->groups_regex);
	    sym->flag_parse_pcre = 0;
	    continue;
	case S_gid:
	    sym_get(sym);
	    parse(sym, S_filter);
	    parse(sym, S_equal);
	    // gid filter = [not] <gid>[-<gid>][,<gid>[-<gid>]]+
	    parse_gid(sym, &mcx->gid);
	    continue;
	case S_gids:
	    sym_get(sym);
	    parse(sym, S_filter);
	    parse(sym, S_equal);
	    // gids filter = [not] <gid>[-<gid>][,<gid>[-<gid>]]+
	    parse_gid(sym, &mcx->gids);
	    continue;
	case S_eof:
	case S_closebra:
	    return MAVIS_CONF_OK;
	default:
	    parse_error_expect(sym, S_resolve, S_script, S_group, S_group, S_gid, S_gids, S_unknown);
	}
    }
}

static int good_gid(struct gid_list *l, u_long gid)
{
    if (!l)
	return -1;
    while (l) {
	int match = l->gid_start <= (gid_t) gid && l->gid_end >= (gid_t) gid;
	if (l->negate)
	    match = !match;
	if (match)
	    return match;
	l = l->next;
    }
    return 0;
}

static int rxmatch(void *v, char *s, enum token token)
{
    switch (token) {
#ifdef WITH_PCRE
    case S_slash:
	return -1 < pcre_exec((pcre *) v, NULL, s, strlen(s), 0, 0, NULL, 0);
#endif
    default:
	return !regexec((regex_t *) v, s, 0, NULL, 0);
    }
}

static int good_name(struct regex_list *l, char *s)
{
    if (!l)
	return -1;
    while (l) {
	int match = rxmatch(l->p, s, l->type);

	if (l->negate)
	    match = !match;
	if (match)
	    return match;
	l = l->next;
    }
    return 0;
}

#define HAVE_mavis_recv_out
static int mavis_recv_out(mavis_ctx * mcx, av_ctx ** ac)
{
    if (mcx->resolve_gid) {
	char *s = av_get(*ac, AV_A_GID);

	if (s) {
	    u_long u = strtoul(s, NULL, 10);
	    if (u || errno != EINVAL) {
		struct group *g;
		if (!good_gid(mcx->gid, u)) {
		    av_unset(*ac, AV_A_GID);
		} else {
		    g = getgrgid((gid_t) u);
		    if (g && good_name(mcx->group_regex, g->gr_name))
			av_set(*ac, AV_A_GID, g->gr_name);
		    else
			av_unset(*ac, AV_A_GID);
		}
	    } else
		av_unset(*ac, AV_A_GID);
	}
    }

    if (mcx->resolve_gids) {
	char *s = av_get(*ac, AV_A_GIDS);
	if (s) {
	    char b[8192];
	    char *p = b;
	    *p = 0;

	    while (*s)
		if (isdigit((int) *s)) {
		    u_long u = strtoul(s, &s, 10);
		    if (u || errno != EINVAL) {
			struct group *g;
			if (!good_gid(mcx->gids, u))
			    continue;
			g = getgrgid((gid_t) u);
			if (g) {
			    ssize_t l;
			    if (!good_name(mcx->groups_regex, g->gr_name))
				continue;
			    l = strlen(g->gr_name);
			    if (b + sizeof(b) - p - 2 > l) {
				if (b[0])
				    *p++ = ',';
				strcpy(p, g->gr_name);
				p += l;
			    }
			}
		    }
		} else
		    s++;

	    if (b[0])
		av_set(*ac, AV_A_GIDS, b);
	    else
		av_unset(*ac, AV_A_GIDS);
	}
    }

    return MAVIS_FINAL;
}

static void drop_gr(struct regex_list *r)
{
    while (r) {
	struct regex_list *l = r->next;
#ifdef WITH_PCRE
	if (r->type == S_slash)
	    pcre_free(r->p);
	else
#endif
	    regfree(r->p);

	free(r);
	r = l;
    }
}

static void drop_gl(struct gid_list *r)
{
    while (r) {
	struct gid_list *l = r->next;
	free(r);
	r = l;
    }
}

#define HAVE_mavis_drop_in
static void mavis_drop_in(mavis_ctx * mcx)
{
    drop_gr(mcx->group_regex);
    drop_gr(mcx->groups_regex);
    drop_gl(mcx->gid);
    drop_gl(mcx->gids);
}

#define MAVIS_name "groups"
#include "mavis_glue.c"
