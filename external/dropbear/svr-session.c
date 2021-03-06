
#include "includes.h"
#include "session.h"
#include "dbutil.h"
#include "packet.h"
#include "algo.h"
#include "buffer.h"
#include "dss.h"
#include "ssh.h"
#include "random.h"
#include "kex.h"
#include "channel.h"
#include "chansession.h"
#include "atomicio.h"
#include "tcpfwd.h"
#include "service.h"
#include "auth.h"
#include "runopts.h"

static void svr_remoteclosed();

struct serversession svr_ses; /* GLOBAL */

static const packettype svr_packettypes[] = {
	{SSH_MSG_CHANNEL_DATA, recv_msg_channel_data},
	{SSH_MSG_CHANNEL_WINDOW_ADJUST, recv_msg_channel_window_adjust},
	{SSH_MSG_USERAUTH_REQUEST, recv_msg_userauth_request}, /* server */
	{SSH_MSG_SERVICE_REQUEST, recv_msg_service_request}, /* server */
	{SSH_MSG_KEXINIT, recv_msg_kexinit},
	{SSH_MSG_KEXDH_INIT, recv_msg_kexdh_init}, /* server */
	{SSH_MSG_NEWKEYS, recv_msg_newkeys},
#ifdef ENABLE_SVR_REMOTETCPFWD
	{SSH_MSG_GLOBAL_REQUEST, recv_msg_global_request_remotetcp},
#endif
	{SSH_MSG_CHANNEL_REQUEST, recv_msg_channel_request},
	{SSH_MSG_CHANNEL_OPEN, recv_msg_channel_open},
	{SSH_MSG_CHANNEL_EOF, recv_msg_channel_eof},
	{SSH_MSG_CHANNEL_CLOSE, recv_msg_channel_close},
#ifdef USING_LISTENERS
	{SSH_MSG_CHANNEL_OPEN_CONFIRMATION, recv_msg_channel_open_confirmation},
	{SSH_MSG_CHANNEL_OPEN_FAILURE, recv_msg_channel_open_failure},
#endif
	{0, 0} /* End */
};

static const struct ChanType *svr_chantypes[] = {
	&svrchansess,
#ifdef ENABLE_SVR_LOCALTCPFWD
	&svr_chan_tcpdirect,
#endif
	NULL /* Null termination is mandatory. */
};

void svr_session(int sock, int childpipe, 
		char* remotehost, char *addrstring) {

	struct timeval timeout;

    reseedrandom();

	crypto_init();
	common_session_init(sock, remotehost);

	/* Initialise server specific parts of the session */
	svr_ses.childpipe = childpipe;
	svr_ses.addrstring = addrstring;
	svr_authinitialise();
	chaninitialise(svr_chantypes);
	svr_chansessinitialise();

	if (gettimeofday(&timeout, 0) < 0) {
		dropbear_exit("Error getting time");
	}

	ses.connecttimeout = timeout.tv_sec + AUTH_TIMEOUT;

	/* set up messages etc */
	ses.remoteclosed = svr_remoteclosed;

	/* packet handlers */
	ses.packettypes = svr_packettypes;
	ses.buf_match_algo = svr_buf_match_algo;

	ses.isserver = 1;

	/* We're ready to go now */
	sessinitdone = 1;

	/* exchange identification, version etc */
	session_identification();

	/* start off with key exchange */
	send_msg_kexinit();

	/* Run the main for loop. NULL is for the dispatcher - only the client
	 * code makes use of it */
	session_loop(NULL);

	/* Not reached */

}

/* failure exit - format must be <= 100 chars */
void svr_dropbear_exit(int exitcode, const char* format, va_list param) {

	char fmtbuf[300];

	if (!sessinitdone) {
		/* before session init */
		snprintf(fmtbuf, sizeof(fmtbuf), 
				"premature exit: %s", format);
	} else if (ses.authstate.authdone) {
		/* user has authenticated */
		snprintf(fmtbuf, sizeof(fmtbuf),
				"exit after auth (%s): %s", 
				ses.authstate.printableuser, format);
	} else if (ses.authstate.printableuser) {
		/* we have a potential user */
		snprintf(fmtbuf, sizeof(fmtbuf), 
				"exit before auth (user '%s', %d fails): %s",
				ses.authstate.printableuser, ses.authstate.failcount, format);
	} else {
		/* before userauth */
		snprintf(fmtbuf, sizeof(fmtbuf), 
				"exit before auth: %s", format);
	}

	_dropbear_log(LOG_INFO, fmtbuf, param);

	/* must be after we've done with username etc */
	common_session_cleanup();

	exit(exitcode);

}

/* priority is priority as with syslog() */
void svr_dropbear_log(int priority, const char* format, va_list param) {

	char printbuf[1024];
	char datestr[20];
	time_t timesec;
	int havetrace = 0;

	vsnprintf(printbuf, sizeof(printbuf), format, param);

#ifndef DISABLE_SYSLOG
	if (svr_opts.usingsyslog) {
		syslog(priority, "%s", printbuf);
	}
#endif

	/* if we are using DEBUG_TRACE, we want to print to stderr even if
	 * syslog is used, so it is included in error reports */
#ifdef DEBUG_TRACE
	havetrace = debug_trace;
#endif

	if (!svr_opts.usingsyslog || havetrace)
	{
		struct tm * local_tm = NULL;
		timesec = time(NULL);
		local_tm = localtime(&timesec);
		if (local_tm == NULL
			|| strftime(datestr, sizeof(datestr), "%b %d %H:%M:%S", 
						localtime(&timesec)) == 0)
		{
			/* upon failure, just print the epoch-seconds time. */
			snprintf(datestr, sizeof(datestr), "%d", timesec);
		}
		fprintf(stderr, "[%d] %s %s\n", getpid(), datestr, printbuf);
	}
}

/* called when the remote side closes the connection */
static void svr_remoteclosed() {

	close(ses.sock);
	ses.sock = -1;
	dropbear_close("Exited normally");

}

