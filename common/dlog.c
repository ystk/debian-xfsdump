/*
 * Copyright (c) 2000-2001 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <xfs/xfs.h>
#include <xfs/jdm.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#include "types.h"
#include "mlog.h"
#include "dlog.h"
#include "getopt.h"

extern bool_t miniroot;
extern pid_t parentpid;

static int dlog_ttyfd = -1;
static bool_t dlog_allowed_flag = BOOL_FALSE;
static bool_t dlog_timeouts_flag = BOOL_FALSE;
static char *promptstr = " -> ";

static bool_t promptinput( char *buf,
			   size_t bufsz,
			   ix_t *exceptionixp,
			   time32_t timeout,
			   ix_t timeoutix,
			   ix_t sigintix,
			   ix_t sighupix,
			   ix_t sigquitix,
			   char *fmt, ... );
static void dlog_string_query_print( void *ctxp, char *fmt, ... );
static void sighandler( int );

bool_t
dlog_init( int argc, char *argv[ ] )
{
	intgen_t c;

	/* can only call once
	 */
	ASSERT( dlog_ttyfd == -1 );

	/* initially allow dialog, use stdin fd
	 */
	dlog_ttyfd = 0; /* stdin */
	dlog_allowed_flag = BOOL_TRUE;
	dlog_timeouts_flag = BOOL_TRUE;

	/* look for command line option claiming the operator knows
	 * what's up.
	 */
	optind = 1;
	opterr = 0;
	while ( ( c = getopt( argc, argv, GETOPT_CMDSTRING )) != EOF ) {
		switch ( c ) {
		case GETOPT_FORCE:
			dlog_ttyfd = -1;
			dlog_allowed_flag = BOOL_FALSE;
			break;
		case GETOPT_NOTIMEOUTS:
			dlog_timeouts_flag = BOOL_FALSE;
			break;
		}
	}
#ifdef RESTORE
	/* look to see if restore source coming in on
	 * stdin. If so , try to open /dev/tty for dialogs.
	 */
	if ( optind < argc && ! strcmp( argv[ optind ], "-" )) {
		dlog_ttyfd = open( "/dev/tty", O_RDWR );
		if ( dlog_ttyfd < 0 ) {
			perror("/dev/tty");
			dlog_ttyfd = -1;
			dlog_allowed_flag = BOOL_FALSE;
		}
	}
#endif /* RESTORE */

#ifdef CHKSTDIN
	/* if stdin is not a tty, don't allow dialogs
	 */
	if ( dlog_allowed_flag ) {
		struct stat statbuf;
		int rval;

		ASSERT( dlog_ttyfd >= 0 );
		rval = fstat( dlog_ttyfd, &statbuf );
		if ( rval ) {
			mlog( MLOG_VERBOSE | MLOG_WARNING,
			      _("could not fstat stdin (fd %d): %s (%d)\n"),
			      dlog_ttyfd,
			      strerror( errno ),
			      errno );
		} else {
			mlog( MLOG_DEBUG,
			      "stdin mode 0x%x\n",
			      statbuf.st_mode );
		}
	}
#endif /* CHKSTDIN */

	return BOOL_TRUE;
}

bool_t
dlog_allowed( void )
{
	return dlog_allowed_flag;
}

void
dlog_desist( void )
{
	dlog_allowed_flag = BOOL_FALSE;
	dlog_ttyfd = -1;
}

intgen_t
dlog_fd( void )
{
	return dlog_ttyfd;
}

void
dlog_begin( char *preamblestr[ ], size_t preamblecnt )
{
	size_t ix;

	mlog_lock( );
	for ( ix = 0 ; ix < preamblecnt ; ix++ ) {
		mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
		      preamblestr[ ix ] );
	}
}

void
dlog_end( char *postamblestr[ ], size_t postamblecnt )
{
	size_t ix;

	for ( ix = 0 ; ix < postamblecnt ; ix++ ) {
		mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
		      postamblestr[ ix ] );
	}
	mlog_unlock( );
}

ix_t
dlog_multi_query( char *querystr[ ],
		  size_t querycnt,
		  char *choicestr[ ],
		  size_t choicecnt,
		  char *hilitestr,
		  size_t hiliteix,
		  char *defaultstr,
		  ix_t defaultix,
		  time32_t timeout,
		  ix_t timeoutix,
		  ix_t sigintix,
		  ix_t sighupix,
		  ix_t sigquitix )
{
	size_t ix;
	char buf[ 100 ];
	char *prepromptstr;

	/* sanity
	 */
	ASSERT( dlog_allowed_flag );
	ASSERT( choicecnt < 9 );

	/* display query description strings
	 */
	for ( ix = 0 ; ix < querycnt ; ix++ ) {
		mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
		      querystr[ ix ] );
	}

	/* display the choices: NOTE: display is 1-based, code intfs 0-based!
	 */
	for ( ix = 0 ; ix < choicecnt ; ix++ ) {
		mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
		      "%u: %s",
		      ix + 1,
		      choicestr[ ix ] );
		if ( ix == hiliteix ) {
			mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
			      "%s",
			      hilitestr ?  hilitestr : " *" );
		}
		if ( ix == defaultix ) {
			mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
			      "%s",
			      defaultstr ?  defaultstr : _(" (default)") );
		}
		if ( dlog_timeouts_flag
		     &&
		     timeoutix != IXMAX
		     &&
		     ix == timeoutix ) {
			mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
			      _(" (timeout in %u sec)"),
			      timeout );
		}
		mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
		      "\n" );
	}

	/* read the tty until we get a proper answer or are interrupted
	 */
	prepromptstr = "";
	for ( ; ; ) {
		ix_t exceptionix;
		bool_t ok;

		/* prompt and accept input
		 */
		ok = promptinput( buf,
				  sizeof( buf ),
				  &exceptionix,
				  timeout,
				  timeoutix,
				  sigintix,
				  sighupix,
				  sigquitix,
				  prepromptstr,
				  choicecnt );
		if ( ok ) {
			if ( ! strlen( buf )) {
				return defaultix;
			}
			if ( strlen( buf ) != 1 ) {
				prepromptstr = _(
				    "please enter a single "
				    "digit response (1 to %d)");
				continue;
			}
			if ( buf[ 0 ] < '1'
			     ||
			     buf[ 0 ] >= '1' + ( u_char_t )choicecnt ) {
				prepromptstr = _(
				      "please enter a single digit "
				      "between 1 and %d inclusive ");
				continue;
			}
			return ( size_t )( buf[ 0 ] - '1' );
		} else {
			return exceptionix;
		}
	}
	/* NOTREACHED */
}

void
dlog_multi_ack( char *ackstr[ ], size_t ackcnt )
{
	size_t ix;

	for ( ix = 0 ; ix < ackcnt ; ix++ ) {
		mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
		      ackstr[ ix ] );
	}
}

ix_t
dlog_string_query( dlog_ucbp_t ucb, /* user's print func */
		   void *uctxp,	  /* user's context for above */
		   char *bufp,	  /* typed string returned in */
		   size_t bufsz,	  /* buffer size */
		   time32_t timeout,  /* secs b4 giving up */
		   ix_t timeoutix,
		   ix_t sigintix,
		   ix_t sighupix,
		   ix_t sigquitix,
		   ix_t okix )
{
	ix_t exceptionix;
	bool_t ok;

	/* call the caller's callback with his context, print context, and
	 * print operator
	 */
	( * ucb )( uctxp, dlog_string_query_print, 0 );

	/* if called for, print the timeout and a newline.
	 * if not, print just a newline
	 */
	if ( dlog_timeouts_flag && timeoutix != IXMAX ) {
		mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
		      _(" (timeout in %u sec)\n"),
		      timeout );
	} else {
		mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
		      "\n" );
	}

	/* prompt and accept input
	 */
	ok = promptinput( bufp,
			  bufsz,
			  &exceptionix,
			  timeout,
			  timeoutix,
			  sigintix,
			  sighupix,
			  sigquitix,
			  "" );
	if ( ok ) {
		return okix;
	} else {
		return exceptionix;
	}
}

void
dlog_string_ack( char *ackstr[ ], size_t ackcnt )
{
	dlog_multi_ack( ackstr, ackcnt );
}

/* ok that this is a static, since used under mutual exclusion lock
 */
static int dlog_signo_received;

static void
sighandler( int signo )
{
	dlog_signo_received = signo;
}

/* ARGSUSED */
static void
dlog_string_query_print( void *ctxp, char *fmt, ... )
{
	va_list args;

	ASSERT( ! ctxp );

	va_start( args, fmt );
	mlog_va( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE, fmt, args );
	va_end( args );
}

static bool_t
promptinput( char *buf,
	     size_t bufsz,
	     ix_t *exceptionixp,
	     time32_t timeout,
	     ix_t timeoutix,
	     ix_t sigintix,
	     ix_t sighupix,
	     ix_t sigquitix,
	     char *fmt,
	     ... )
{
	va_list args;
	u_intgen_t alarm_save = 0;
	void (* sigalrm_save)(int) = NULL;
	void (* sigint_save)(int) = NULL;
	void (* sighup_save)(int) = NULL;
	void (* sigterm_save)(int) = NULL;
	void (* sigquit_save)(int) = NULL;
	intgen_t nread;
	pid_t pid = getpid( );

	/* display the pre-prompt
	 */
	va_start( args, fmt );
	mlog_va( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE, fmt, args );
	va_end( args );

	/* display standard prompt
	 */
#ifdef NOTYET
	if ( dlog_timeouts_flag && timeoutix != IXMAX ) {
		mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
		      _("(timeout in %d sec)"),
		      timeout );
	}
#endif /* NOTYET */
	mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE, promptstr );

	/* set up signal handling
	 */
	dlog_signo_received = -1;
	if ( dlog_timeouts_flag && timeoutix != IXMAX ) {
		if ( pid == parentpid && ! miniroot ) {
			( void )sigrelse( SIGALRM );
		}
		sigalrm_save = sigset( SIGALRM, sighandler );
		alarm_save = alarm( ( u_intgen_t )timeout );
	}
	if ( sigintix != IXMAX ) {
		if ( pid == parentpid && ! miniroot ) {
			( void )sigrelse( SIGINT );
		}
		sigint_save = sigset( SIGINT, sighandler );
	}
	if ( sighupix != IXMAX ) {
		if ( pid == parentpid && ! miniroot ) {
			( void )sigrelse( SIGHUP );
		}
		sighup_save = sigset( SIGHUP, sighandler );
		if ( pid == parentpid && ! miniroot ) {
			( void )sigrelse( SIGTERM );
		}
		sigterm_save = sigset( SIGTERM, sighandler );
	}
	if ( sigquitix != IXMAX ) {
		if ( pid == parentpid && ! miniroot ) {
			( void )sigrelse( SIGQUIT );
		}
		sigquit_save = sigset( SIGQUIT, sighandler );
	}

	/* wait for input, timeout, or interrupt
	 */
	ASSERT( dlog_ttyfd >= 0 );
	nread = read( dlog_ttyfd, buf, bufsz - 1 );

	/* restore signal handling
	 */
	if ( sigquitix != IXMAX ) {
		( void )sigset( SIGQUIT, sigquit_save );
		if ( pid == parentpid && ! miniroot ) {
			( void )sighold( SIGQUIT );
		}
	}
	if ( sighupix != IXMAX ) {
		( void )sigset( SIGHUP, sighup_save );
		if ( pid == parentpid && ! miniroot ) {
			( void )sighold( SIGHUP );
		}
		( void )sigset( SIGTERM, sigterm_save );
		if ( pid == parentpid && ! miniroot ) {
			( void )sighold( SIGTERM );
		}
	}
	if ( sigintix != IXMAX ) {
		( void )sigset( SIGINT, sigint_save );
		if ( pid == parentpid && ! miniroot ) {
			( void )sighold( SIGINT );
		}
	}
	if ( dlog_timeouts_flag && timeoutix != IXMAX ) {
		( void )alarm( alarm_save );
		( void )sigset( SIGALRM, sigalrm_save );
		if ( pid == parentpid && ! miniroot ) {
			( void )sighold( SIGALRM );
		}
	}
	
	/* check for timeout or interrupt
	 */
	if ( nread < 0 ) {
		if ( dlog_signo_received == SIGALRM ) {
			mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
			      _("timeout\n") );
			*exceptionixp = timeoutix;
		} else if ( dlog_signo_received == SIGINT ) {
			mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
			      _("keyboard interrupt\n") );
			mlog_exit_hint(RV_KBD_INTR);
			*exceptionixp = sigintix;
		} else if ( dlog_signo_received == SIGHUP ) {
			mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
			      _("hangup\n") );
			*exceptionixp = sighupix;
		} else if ( dlog_signo_received == SIGTERM ) {
			mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
			      _("terminate\n") );
			*exceptionixp = sighupix;
		} else if ( dlog_signo_received == SIGQUIT ) {
			mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
			      _("keyboard quit\n") );
			mlog_exit_hint(RV_KBD_INTR);
			*exceptionixp = sigquitix;
		} else {
			mlog( MLOG_NORMAL | MLOG_NOLOCK | MLOG_BARE,
			      _("unknown signal during dialog: %d\n"),
			      dlog_signo_received );
			*exceptionixp = sigquitix;
		}
		return BOOL_FALSE;
	} else if ( nread == 0 ) {
		*exceptionixp = timeoutix;
		if ( bufsz > 0 ) {
			buf[ 0 ] = 0;
		}
		return BOOL_FALSE;
	} else {
		ASSERT( dlog_signo_received == -1 );
		ASSERT( ( size_t )nread < bufsz );
		ASSERT( buf[ nread - 1 ] == '\n' );
		buf[ nread - 1 ] = 0;
		*exceptionixp = 0;
		return BOOL_TRUE;
	}
}
