/*
 * main.c
 * (C)1999-2011 by Marc Huber <Marc.Huber@web.de>
 * All rights reserved.
 *
 * $Id: main.c,v 1.30 2015/03/14 06:11:31 marc Exp marc $
 *
 */

#define __MAIN__

#include "misc/sysconf.h"
#include "headers.h"
#include "misc/version.h"
#include "misc/sig_segv.h"
#include <sys/un.h>

static const char rcsid[] __attribute__ ((used)) = "$Id: main.c,v 1.30 2015/03/14 06:11:31 marc Exp marc $";

#ifdef WITH_SSL
#include "misc/ssl_init.h"
#endif				/* WITH_SSL */

static void periodics(struct context *ctx, int cur __attribute__ ((unused)))
{
    struct scm_data sd;
    DebugIn(DEBUG_PROC);

    io_sched_renew(ctx->io, ctx);
    process_signals();		/* process pending signals */

    sd.type = SCM_KEEPALIVE;
    if (!die_when_idle && common_data.scm_send_msg(0, &sd, -1))
	die_when_idle = -1;

    if (common_data.users_cur == 0 && die_when_idle) {
	Debug((DEBUG_PROC, "exiting -- process out of use\n"));
	logmsg("Terminating, no longer needed.");
	exit(EX_OK);
    }

    DebugOut(DEBUG_PROC);
}

int main(int argc, char **argv, char **envp)
{
    struct io_context *io;
    struct rlimit rlim;
    struct scm_data_max sd;

    scm_main(argc, argv, envp);

    if (!common_data.conffile) {
	common_data.conffile = argv[optind];
	common_data.id = argv[optind + 1];
    }
    cfg_read_config(common_data.conffile, parse_decls, common_data.id ? common_data.id : common_data.progname);

    if (common_data.parse_only)
	exit(EX_OK);

    umask(022);

    if (!con_arr) {
	logmsg("No remote services defined! Exiting.");
	exit(EX_USAGE);
    }

    logmsg("startup (version " VERSION ")");

    mavis_detach();

#ifdef WITH_SSL
    if (ssl_cert)
	ssl_ctx = ssl_init(ssl_cert, ssl_key, ssl_pass, NULL);
#endif				/* WITH_SSL */

    setup_sig_segv(common_data.coredumpdir, common_data.gcorepath, common_data.debug_cmd);

    if (common_data.singleprocess) {
	common_data.scm_accept = accepted_raw;
	io = common_data.io;
    } else {
	setproctitle_init(argv, envp);
	io = common_data.io = io_init();
	setup_signals();
	ctx_spawnd = new_context(io);
	ctx_spawnd->ifn = 0;
	io_register(io, 0, ctx_spawnd);
	io_set_cb_i(io, 0, (void *) accepted);
	io_set_cb_e(io, 0, (void *) cleanup_spawnd);
	io_set_cb_h(io, 0, (void *) cleanup_spawnd);
	io_set_i(io, 0);
    }

    if (!getrlimit(RLIMIT_NOFILE, &rlim)) {
	rlim.rlim_cur = rlim.rlim_max;
	setrlimit(RLIMIT_NOFILE, &rlim);
	getrlimit(RLIMIT_NOFILE, &rlim);
	nfds_max = (int) rlim.rlim_cur;
    }

    sd.type = SCM_MAX;
    sd.max = (nfds_max - 10) / 2;
    common_data.scm_send_msg(0, (struct scm_data *) &sd, -1);
    io_sched_add(io, new_context(io), (void *) periodics, 60, 0);

    set_proctitle(ACCEPT_YES);

    io_main(io);
}
