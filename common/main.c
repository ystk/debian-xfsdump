/*
 * Copyright (c) 2000-2002 Silicon Graphics, Inc.
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
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <getopt.h>
#include <stdint.h>
#include <sched.h>

#include "exit.h"
#include "types.h"
#include "stream.h"
#include "cldmgr.h"
#include "util.h"
#include "getopt.h"
#include "mlog.h"
#include "qlock.h"
#include "lock.h"
#include "dlog.h"
#include "global.h"
#include "drive.h"
#include "media.h"
#include "content.h"
#include "inventory.h"

#ifdef DUMP
/* main.c - main for dump
 */
#endif /* DUMP */
#ifdef RESTORE
/* main.c - main for restore
 */
#endif /* RESTORE */


/* structure definitions used locally ****************************************/

#ifdef RESTORE
#define VMSZ_PER	4	/* proportion of available vm to use in tree */
#endif /* RESTORE */
#define DLOG_TIMEOUT	60	/* time out operator dialog */
#define STOP_TIMEOUT	600	/* seconds after stop req. before abort */
#define ABORT_TIMEOUT	10	/* seconds after abort req. before abort */
#define MINSTACKSZ	0x02000000
#define MAXSTACKSZ	0x08000000


/* declarations of externally defined global symbols *************************/

extern void rmt_turnonmsgs(int);

/* forward declarations of locally defined global functions ******************/

void usage( void );
bool_t preemptchk( int );


/* forward declarations of locally defined static functions ******************/

static bool_t loadoptfile( int *argcp, char ***argvp );
static char * stripquotes( char *p );
static void shiftleftby1( char *p, char *endp );
static bool_t in_miniroot_heuristic( void );
#ifdef HIDDEN
static void mrh_sighandler( int );
#endif
static void sighandler( int );
static int childmain( void * );
static bool_t sigint_dialog( void );
static char *sigintstr( void );
#ifdef DUMP
static bool_t set_rlimits( void );
#endif /* DUMP */
#ifdef RESTORE
static bool_t set_rlimits( size64_t * );
#endif /* RESTORE */
static char *exit_codestring( intgen_t code );
static char *sig_numstring( intgen_t num );
static char *strpbrkquotes( char *p, const char *sep );


/* definition of locally defined global variables ****************************/

intgen_t version = 3;
intgen_t subversion = 0;
char *progname = 0;			/* used in all error output */
char *homedir = 0;			/* directory invoked from */
#ifdef HIDDEN
bool_t miniroot = BOOL_FALSE;
#else
bool_t miniroot = BOOL_TRUE;
#endif /* HIDDEN */
bool_t pipeline = BOOL_FALSE;
bool_t stdoutpiped = BOOL_FALSE;
pid_t parentpid;
char *sistr;
size_t pgsz;
size_t pgmask;


/* definition of locally defined static variables *****************************/

static rlim64_t minstacksz;
static rlim64_t maxstacksz;
#ifdef RESTORE
static size64_t vmsz;
#endif /* RESTORE */
static time32_t stop_deadline;
static bool_t stop_in_progress;
static bool_t sighup_received;
static bool_t sigterm_received;
static bool_t sigpipe_received;
static bool_t sigquit_received;
static bool_t sigint_received;
static size_t prbcld_cnt;
static pid_t prbcld_pid;
static intgen_t prbcld_xc;
static intgen_t prbcld_signo;
/* REFERENCED */
static intgen_t sigstray_received;
static bool_t progrpt_enabledpr;
static time32_t progrpt_interval;
static time32_t progrpt_deadline;


/* definition of locally defined global functions ****************************/


int
main( int argc, char *argv[] )
{
	int c;
#ifdef DUMP
	uid_t euid;
#endif /* DUMP */
	ix_t stix; /* stream index */
	bool_t infoonly;
#ifdef DUMP
	global_hdr_t *gwhdrtemplatep;
#endif /* DUMP */
	bool_t init_error;
	bool_t coredump_requested = BOOL_FALSE;
	intgen_t exitcode;
	rlim64_t tmpstacksz;
	bool_t ok;
	/* REFERENCED */
	int rval;

	/* sanity checks
	 */
	ASSERT( sizeof( char_t ) == 1 );
	ASSERT( sizeof( u_char_t ) == 1 );
	ASSERT( sizeof( int32_t ) == 4 );
	ASSERT( sizeof( u_int32_t ) == 4 );
	ASSERT( sizeof( size32_t ) == 4 );
	ASSERT( sizeof( int64_t ) == 8 );
	ASSERT( sizeof( u_int64_t ) == 8 );
	ASSERT( sizeof( size64_t ) == 8 );

	/* record the command name used to invoke
	 */
	progname = argv[ 0 ];

	/* setup I18N support */
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	/* bootstrap message logging (stage 0)
	*/
	mlog_init0();

	/* Get the parent's pid. will be used in signal handling
	 * to differentiate parent from children.
	 */
	parentpid = getpid( );
	rval = atexit(mlog_exit_flush);
	assert(rval == 0);

	/* pre-scan the command line for the option file option.
	 * if found, create a new argv.
	 */
	ok = loadoptfile( &argc, &argv );
	if ( ! ok ) {
		return mlog_exit(EXIT_ERROR, RV_OPT);
	}
	
	/* initialize message logging (stage 1)
	 */
	ok = mlog_init1( argc, argv );
	if ( ! ok ) {
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}
	/* scan the command line for the miniroot, info, progress
	 * report options, and stacksz.
	 */
	minstacksz = MINSTACKSZ;
	maxstacksz = MAXSTACKSZ;
#ifdef HIDDEN
	miniroot = BOOL_FALSE;
#else
	miniroot = BOOL_TRUE;
#endif /* HIDDEN */
	infoonly = BOOL_FALSE;
	progrpt_enabledpr = BOOL_FALSE;
	optind = 1;
	opterr = 0;
	while ( ( c = getopt( argc, argv, GETOPT_CMDSTRING )) != EOF ) {
		switch ( c ) {
                case GETOPT_MINSTACKSZ:
			if ( ! optarg || optarg[ 0 ] == '-' ) {
				mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_NOLOCK,
				      _("-%c argument missing\n"),
				      c );
				usage( );
				return mlog_exit(EXIT_ERROR, RV_OPT);
			}
			errno = 0;
			tmpstacksz = strtoull( optarg, 0, 0 );
			if ( tmpstacksz == UINT64_MAX
			     ||
			     errno == ERANGE ) {
				mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_NOLOCK,
				      _("-%c argument (%s) invalid\n"),
				      c,
				      optarg );
				usage( );
				return mlog_exit(EXIT_ERROR, RV_OPT);
			}
			minstacksz = tmpstacksz;
			break;
                case GETOPT_MAXSTACKSZ:
			if ( ! optarg || optarg[ 0 ] == '-' ) {
				mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_NOLOCK,
				      _("-%c argument missing\n"),
				      c );
				usage( );
				return mlog_exit(EXIT_ERROR, RV_OPT);
			}
			errno = 0;
			tmpstacksz = strtoull( optarg, 0, 0 );
			if ( tmpstacksz == UINT64_MAX
			     ||
			     errno == ERANGE ) {
				mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_NOLOCK,
				      _("-%c argument (%s) invalid\n"),
				      c,
				      optarg );
				usage( );
				return mlog_exit(EXIT_ERROR, RV_OPT);
			}
			maxstacksz = tmpstacksz;
			break;
                case GETOPT_MINIROOT:
                        miniroot = BOOL_TRUE;
                        break;
		case GETOPT_HELP:
			infoonly = BOOL_TRUE;
			mlog_exit_hint(RV_USAGE);
			break;
		case GETOPT_PROGRESS:
			if ( ! optarg || optarg[ 0 ] == '-' ) {
				mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_NOLOCK,
				      _("-%c argument missing\n"),
				      c );
				usage( );
				return mlog_exit(EXIT_ERROR, RV_OPT);
			}
			progrpt_interval = ( time32_t )atoi( optarg );
			if ( progrpt_interval > 0 ) {
				progrpt_enabledpr = BOOL_TRUE;
			} else {
				progrpt_enabledpr = BOOL_FALSE;
			}
			break;
		}
	}

	/* sanity check resultant stack size limits
	 */
	if ( minstacksz > maxstacksz ) {
		mlog( MLOG_NORMAL
		      |
		      MLOG_ERROR
		      |
		      MLOG_NOLOCK
		      |
		      MLOG_PROC,
		      _("specified minimum stack size is larger than maximum: "
		      "min is 0x%llx,  max is 0x%llx\n"),
		      minstacksz,
		      maxstacksz );
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	if ( argc == 1 ) {
		infoonly = BOOL_TRUE;
	}

	/* set a progress report deadline to allow preemptchk() to
	 * report
	 */
	if ( progrpt_enabledpr ) {
		progrpt_deadline = time( 0 ) + progrpt_interval;
	}

	/* intitialize the stream manager
	 */
	stream_init( );

#ifdef DUMP
	/* set the memory limits to their appropriate values.
	 */
	ok = set_rlimits( );
#endif /* DUMP */
#ifdef RESTORE
	/* set the memory limits to their appropriate values. this is necessary
	 * to accomodate the tree abstraction and some recursive functions.
	 * also determines maximum vm, which will be budgeted among the
	 * various abstractions.
	 */
	ok = set_rlimits( &vmsz );
#endif /* RESTORE */
	if ( ! ok ) {
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	/* perform an experiment to determine if we are in the miniroot.
	 * various features will be disallowed if in miniroot.
	 */
	if ( ! miniroot && in_miniroot_heuristic( )) {
		miniroot = BOOL_TRUE;
	}

	/* initialize the spinlock allocator
	 */
	ok = qlock_init( miniroot );
	if ( ! ok ) {
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	/* initialize message logging (stage 2) - allocate the message lock
	 */
	ok = mlog_init2( );
	if ( ! ok ) {
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	/* initialize the critical region lock
	 */
	lock_init( );
	rmt_turnonmsgs(1); /* turn on WARNING msgs for librmt */

	mlog( MLOG_NITTY + 1, "INTGENMAX == %ld (0x%lx)\n", INTGENMAX, INTGENMAX );
	mlog( MLOG_NITTY + 1, "UINTGENMAX == %lu (0x%lx)\n", UINTGENMAX, UINTGENMAX );
	mlog( MLOG_NITTY + 1, "OFF64MAX == %lld (0x%llx)\n", OFF64MAX, OFF64MAX );
	mlog( MLOG_NITTY + 1, "OFFMAX == %ld (0x%lx)\n", OFFMAX, OFFMAX );
	mlog( MLOG_NITTY + 1, "SIZEMAX == %lu (0x%lx)\n", SIZEMAX, SIZEMAX );
	mlog( MLOG_NITTY + 1, "INOMAX == %lu (0x%lx)\n", INOMAX, INOMAX );
	mlog( MLOG_NITTY + 1, "TIMEMAX == %ld (0x%lx)\n", TIMEMAX, TIMEMAX );
	mlog( MLOG_NITTY + 1, "SIZE64MAX == %llu (0x%llx)\n", SIZE64MAX, SIZE64MAX );
	mlog( MLOG_NITTY + 1, "INO64MAX == %llu (0x%llx)\n", INO64MAX, INO64MAX );
	mlog( MLOG_NITTY + 1, "UINT64MAX == %llu (0x%llx)\n", UINT64MAX, UINT64MAX );
	mlog( MLOG_NITTY + 1, "INT64MAX == %lld (0x%llx)\n", INT64MAX, INT64MAX );
	mlog( MLOG_NITTY + 1, "UINT32MAX == %u (0x%x)\n", UINT32MAX, UINT32MAX );
	mlog( MLOG_NITTY + 1, "INT32MAX == %d (0x%x)\n", INT32MAX, INT32MAX );
	mlog( MLOG_NITTY + 1, "INT16MAX == %d (0x%x)\n", INT16MAX, INT16MAX );
	mlog( MLOG_NITTY + 1, "UINT16MAX == %u (0x%x)\n", UINT16MAX, UINT16MAX );

	/* ask the system for the true vm page size, which must be used
	 * in all mmap calls
	 */
	pgsz = ( size_t )getpagesize( );
	mlog( MLOG_DEBUG | MLOG_PROC,
	      "getpagesize( ) returns %u\n",
	      pgsz );
	ASSERT( ( intgen_t )pgsz > 0 );
	pgmask = pgsz - 1;

	/* report parent pid
         */
	mlog( MLOG_DEBUG | MLOG_PROC,
	      "parent pid is %d\n",
	      parentpid );

	/* get the current working directory: this is where we will dump
	 * core, if necessary. some tmp files may be placed here as well.
	 */
	homedir = getcwd( 0, MAXPATHLEN );
	if ( ! homedir ) {
		mlog( MLOG_NORMAL | MLOG_ERROR,
		      _("unable to determine current directory: %s\n"),
		      strerror( errno ));
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	/* sanity check the inventory database directory, setup global paths
	 */
	ok = inv_setup_base( );
	if ( ! ok ) {
		mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_NOLOCK,
		      _("both /var/lib/xfsdump and /var/xfsdump exist - fatal\n"));
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	/* if just looking for info, oblige
	 */
	if ( infoonly ) {
		mlog( MLOG_NORMAL,
		      _("version %s (dump format %d.%d)\n"),
		      VERSION,
		      version,
		      subversion );
		usage( );
		return mlog_exit(EXIT_NORMAL, RV_OK); /* normal termination */
	}

	/* if an inventory display is requested, do it and exit
	 */
	if ( ! inv_DEBUG_print( argc, argv )) {
		return mlog_exit(EXIT_NORMAL, RV_OK); /* normal termination */
	}

#ifdef DUMP
	/* insist that the effective user id is root.
	 * this must appear after inv_DEBUG_print(),
	 * so it may be done without root privilege.
	 */
	euid = geteuid( );
	mlog( MLOG_DEBUG | MLOG_PROC,
	      "effective user id is %d\n",
	      euid );
	if ( euid != 0 ) {
		mlog( MLOG_NORMAL,
		      _("effective user ID must be root\n") );
		return mlog_exit(EXIT_ERROR, RV_PERM);
	}
#endif /* DUMP */

	/* initialize operator dialog capability
	 */
	ok = dlog_init( argc, argv );
	if ( ! ok ) {
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	/* initialize the child process manager
	 */
	ok = cldmgr_init( );
	if ( ! ok ) {
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	/* select and instantiate a drive manager for each stream. this
	 * is the first pass at initialization, so don't do anything
	 * terribly time-consuming here. A second initialization pass
	 * will be done shortly.
	 */
	ok = drive_init1( argc, argv, miniroot );
	if ( ! ok ) {
		cldmgr_killall( );
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	/* check the drives to see if we're in a pipeline.
	 * if not, check stdout anyway, in case someone is trying to pipe
	 * the log messages into more, tee, ...
	 */
	if ( drivepp[ 0 ]->d_isunnamedpipepr ) {
		mlog( MLOG_DEBUG | MLOG_NOTE,
		      "pipeline detected\n" );
		pipeline = BOOL_TRUE;
	} else {
		struct stat64 statbuf;
		if ( fstat64( 1, &statbuf ) == 0
		     &&
		     ( statbuf.st_mode & S_IFMT ) == S_IFIFO ) {
			stdoutpiped = BOOL_TRUE;
		}
	}

	/* announce version and instructions
	 */
	sistr = sigintstr( );
	mlog( MLOG_VERBOSE,
	      _("version %s (dump format %d.%d)"),
	      VERSION,
	      version,
	      subversion );
	if ( miniroot ) {
		mlog( MLOG_VERBOSE | MLOG_BARE, _(
		      " - "
		      "Running single-threaded\n") );
	} else if ( ! pipeline && ! stdoutpiped && sistr && dlog_allowed( )) {
		mlog( MLOG_VERBOSE | MLOG_BARE, _(
		      " - "
		      "type %s for status and control\n"),
		      sistr );
	} else {
		mlog( MLOG_VERBOSE | MLOG_BARE,
		      "\n" );
	}

#ifdef DUMP
	/* build a global write header template
	 */
	gwhdrtemplatep = global_hdr_alloc( argc, argv );
	if ( ! gwhdrtemplatep ) {
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}
#endif /* DUMP */

	/* tell mlog how many streams there are. the format of log messages
	 * depends on whether there are one or many.
	 */
	mlog_tell_streamcnt( drivecnt );

	/* initialize the state of signal processing. if miniroot or
	 * pipeline, just want to exit when a signal is received. otherwise,
	 * hold signals so they don't interfere with sys calls; they will
	 * be released at pre-emption points and upon pausing in the main
	 * loop.
	 */
	if ( ! miniroot && ! pipeline ) {
		stop_in_progress = BOOL_FALSE;
		coredump_requested = BOOL_FALSE;
		sighup_received = BOOL_FALSE;
		sigterm_received = BOOL_FALSE;
		sigint_received = BOOL_FALSE;
		sigpipe_received = BOOL_FALSE;
		sigquit_received = BOOL_FALSE;
		sigstray_received = BOOL_FALSE;
		prbcld_cnt = 0;
		sigset( SIGINT, sighandler );
		sighold( SIGINT );
		sigset( SIGHUP, sighandler );
		sighold( SIGHUP );
		sigset( SIGTERM, sighandler );
		sighold( SIGTERM );
		sigset( SIGPIPE, sighandler );
		sighold( SIGPIPE );
		sigset( SIGQUIT, sighandler );
		sighold( SIGQUIT );
		alarm( 0 );
		sigset( SIGALRM, sighandler );
		sighold( SIGALRM );
		sigset( SIGCLD, sighandler );
		sighold( SIGCLD );
	}

	/* do content initialization.
	 */
#ifdef DUMP
	ok = content_init( argc, argv, gwhdrtemplatep );
#endif /* DUMP */
#ifdef RESTORE
	ok = content_init( argc, argv, vmsz / VMSZ_PER );
#endif /* RESTORE */
	if ( ! ok ) {
		cldmgr_killall( );
		return mlog_exit(EXIT_ERROR, RV_INIT);
	}

	/* if miniroot or a pipeline, go single-threaded
	 * with just one stream.
	 */
	if ( miniroot || pipeline ) {
		intgen_t exitcode;

		sigset( SIGINT, sighandler );
		sigset( SIGHUP, sighandler );
		sigset( SIGTERM, sighandler );
		sigset( SIGPIPE, sighandler );

		ok = drive_init2( argc,
				  argv,
#ifdef DUMP
				  gwhdrtemplatep );
#endif /* DUMP */
#ifdef RESTORE
				  ( global_hdr_t * )0 );
#endif /* RESTORE */
		if ( ! ok ) {
			return mlog_exit(EXIT_ERROR, RV_INIT);
		}
		ok = drive_init3( );
		if ( ! ok ) {
			return mlog_exit(EXIT_ERROR, RV_INIT);
		}
#ifdef DUMP
		exitcode = content_stream_dump( 0 );
#endif /* DUMP */
#ifdef RESTORE
		exitcode = content_stream_restore( 0 );
#endif /* RESTORE */
		if ( exitcode != EXIT_NORMAL ) {
			( void )content_complete( );
						/* for cleanup side-effect */
			return mlog_exit(exitcode, RV_UNKNOWN);
		} else if ( content_complete( )) {
			return mlog_exit(EXIT_NORMAL, RV_OK);
		} else {
			return mlog_exit(EXIT_INTERRUPT, RV_UNKNOWN);
		}
	}

	/* used to skip to end if errors occur during any
	 * stage of initialization.
	 */
	init_error = BOOL_FALSE;

	/* now do the second and third passes of drive initialization.
	 * allocate per-stream write and read headers. if a drive
	 * manager uses a slave process, it should be created now,
	 * using cldmgr_create( ). each drive manager may use the slave to
	 * asynchronously read the media file header, typically a very
	 * time-consuming chore. drive_init3 will synchronize with each slave.
	 */
	if ( ! init_error ) {
		ok = drive_init2( argc,
				  argv,
#ifdef DUMP
				  gwhdrtemplatep );
#endif /* DUMP */
#ifdef RESTORE
				  ( global_hdr_t * )0 );
#endif /* RESTORE */
		if ( ! ok ) {
			init_error = BOOL_TRUE;
		}
	}
	if ( ! init_error ) {
		ok = drive_init3( );
		if ( ! ok ) {
			init_error = BOOL_TRUE;
		}
	}

	/* create a child thread for each stream. drivecnt global from
	 * drive.h, initialized by drive_init[12]
	 */
	if ( ! init_error ) {
		for ( stix = 0 ; stix < drivecnt ; stix++ ) {
			ok = cldmgr_create( childmain,
					    CLONE_VM,
					    stix,
					    "child",
					    ( void * )stix );
			if ( ! ok ) {
				init_error = BOOL_TRUE;
			}
		}
	}

	/* loop here, waiting for children to die, processing operator
	 * signals.
	 */
	if ( progrpt_enabledpr ) {
		( void )alarm( ( u_intgen_t )progrpt_interval );
	}
	for ( ; ; ) {
		time32_t now;
		bool_t stop_requested = BOOL_FALSE;
		intgen_t stop_timeout = -1;

		/* if there was an initialization error,
		 * immediately stop all children.
		 */
		if ( init_error ) {
			stop_timeout = STOP_TIMEOUT;
			stop_requested = BOOL_TRUE;
		}

		/* if one or more children died abnormally, request a
		 * stop. furthermore, note that core should be dumped if
		 * the child explicitly exited with EXIT_FAULT.
		 */
		if ( prbcld_cnt ) {
			if ( prbcld_xc == EXIT_FAULT || prbcld_signo != 0 ) {
				coredump_requested = BOOL_TRUE;
				stop_timeout = ABORT_TIMEOUT;
			} else {
				stop_timeout = STOP_TIMEOUT;
			}
			stop_requested = BOOL_TRUE;
			if ( prbcld_xc != EXIT_NORMAL ) {
				mlog( MLOG_DEBUG | MLOG_PROC,
				      "child (pid %d) requested stop: "
				      "exit code %d (%s)\n",
				      prbcld_pid,
				      prbcld_xc,
				      exit_codestring( prbcld_xc ));
			} else if ( prbcld_signo ) {
				ASSERT( prbcld_signo );
				mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_PROC,
				      _("child (pid %d) faulted: "
				      "signal number %d (%s)\n"),
				      prbcld_pid,
				      prbcld_signo,
				      sig_numstring( prbcld_signo ));
			}
			prbcld_cnt = 0;
		}
			
		/* all children died normally. break out.
		 */
		if ( cldmgr_remainingcnt( ) == 0 ) {
			mlog( MLOG_DEBUG,
			      "all children have exited\n" );
			break;
		}

		/* get the current time
		 */
		now = time( 0 );

		/* check for stop timeout. request a core dump and bail
		 */
		if ( stop_in_progress && now >= stop_deadline ) {
			mlog( MLOG_NORMAL | MLOG_ERROR,
			      _("session interrupt timeout\n") );
			coredump_requested = BOOL_TRUE;
			break;
		}

		/* operator sent SIGINT. if dialog allowed, enter dialog.
		 * otherwise treat as a hangup and request a stop.
		 */
		if ( sigint_received ) {
			mlog( MLOG_DEBUG | MLOG_PROC,
			      "SIGINT received\n" );
			if ( stop_in_progress ) {
				if ( dlog_allowed( )) {
					( void )sigint_dialog( );
				}
				/*
				mlog( MLOG_NORMAL,
				      _("session interrupt in progress: "
				      "please wait\n") );
				 */
			} else {
				if ( dlog_allowed( )) {
					stop_requested = sigint_dialog( );
				} else {
					stop_requested = BOOL_TRUE;
				}
				stop_timeout = STOP_TIMEOUT;
			}
				
			/* important that this appear after dialog.
			 * allows dialog to be terminated with SIGINT,
			 * without infinite loop.
			 */
			sigint_received = BOOL_FALSE;
		}

		/* refresh the current time in case in dialog for a while
		 */
		now = time( 0 );

		/* request a stop on hangup
		 */
		if ( sighup_received ) {
			mlog( MLOG_DEBUG | MLOG_PROC,
			      "SIGHUP received\n" );
			stop_requested = BOOL_TRUE;
			stop_timeout = STOP_TIMEOUT;
			sighup_received = BOOL_FALSE;
		}

		/* request a stop on termination request
		 */
		if ( sigterm_received ) {
			mlog( MLOG_DEBUG | MLOG_PROC,
			      "SIGTERM received\n" );
			stop_requested = BOOL_TRUE;
			stop_timeout = STOP_TIMEOUT;
			sigterm_received = BOOL_FALSE;
		}

		/* request a stop on loss of write pipe
		 */
		if ( sigpipe_received ) {
			mlog( MLOG_DEBUG | MLOG_PROC,
			      "SIGPIPE received\n" );
			stop_requested = BOOL_TRUE;
			stop_timeout = STOP_TIMEOUT;
			sigpipe_received = BOOL_FALSE;
		}
		
		/* operator send SIGQUIT. treat like an interrupt,
		 * but force a core dump
		 */
		if ( sigquit_received ) {
			mlog( MLOG_NORMAL | MLOG_PROC,
			      "SIGQUIT received\n" );
			if ( stop_in_progress ) {
				mlog( MLOG_NORMAL,
				      _("session interrupt in progress: "
				      "please wait\n") );
				stop_deadline = now;
			} else {
				stop_requested = BOOL_TRUE;
				stop_timeout = ABORT_TIMEOUT;
				sigquit_received = BOOL_FALSE;
				coredump_requested = BOOL_TRUE;
			}
		}
		
		/* see if need to initiate a stop
		 */
		if ( stop_requested && ! stop_in_progress ) {
			mlog( MLOG_NORMAL,
			      _("initiating session interrupt (timeout in %d sec)\n"),
			      stop_timeout);
			mlog_exit_hint(RV_INTR);
			stop_in_progress = BOOL_TRUE;
			cldmgr_stop( );
			ASSERT( stop_timeout >= 0 );
			stop_deadline = now + ( time32_t )stop_timeout;
		}
		
		/* set alarm if needed (note time stands still during dialog)
		 */
		if ( stop_in_progress ) {
			intgen_t timeout = ( intgen_t )( stop_deadline - now );
			if ( timeout < 0 ) {
				timeout = 0;
			}
			mlog( MLOG_DEBUG | MLOG_PROC,
			      "setting alarm for %d second%s\n",
			      timeout,
			      timeout == 1 ? "" : "s" );
			( void )alarm( ( u_intgen_t )timeout );
			if ( timeout == 0 ) {
				continue;
			}
		}

		if ( progrpt_enabledpr && ! stop_in_progress ) {
			bool_t need_progrptpr = BOOL_FALSE;
			while ( now >= progrpt_deadline ) {
				need_progrptpr = BOOL_TRUE;
				progrpt_deadline += progrpt_interval;
			}
			if ( need_progrptpr ) {
				size_t statlinecnt;
				char **statline;
				ix_t i;
				statlinecnt = content_statline( &statline );
				for ( i = 0 ; i < statlinecnt ; i++ ) {
					mlog( MLOG_NORMAL,
					      statline[ i ] );
				}
			}
			( void )alarm( ( u_intgen_t )( progrpt_deadline
						       -
						       now ));
		}

		/* sleep until next signal
		 */
		sigrelse( SIGINT );
		sigrelse( SIGHUP );
		sigrelse( SIGTERM );
		sigrelse( SIGPIPE );
		sigrelse( SIGQUIT );
		sigrelse( SIGALRM );
		( void )sigpause( SIGCLD );
		sighold( SIGCLD );
		sighold( SIGALRM );
		sighold( SIGQUIT );
		sighold( SIGPIPE );
		sighold( SIGTERM );
		sighold( SIGHUP );
		sighold( SIGINT );
		( void )alarm( 0 );
	}

	/* check if core dump requested
	 */
	if ( coredump_requested ) {
		mlog( MLOG_DEBUG | MLOG_PROC,
		      "killing all remaining children\n" );
		cldmgr_killall( );
		sleep( 1 );
		mlog( MLOG_DEBUG | MLOG_PROC,
		      "parent sending SIGQUIT to self (pid %d)\n",
		      parentpid );
		sigrelse( SIGQUIT );
		sigset( SIGQUIT, SIG_DFL );
		kill( parentpid, SIGQUIT );
		for ( ; ; ) {
			sleep( 1 );
		}
	}

	/* determine if dump or restore was interrupted
	 * or an initialization error occurred.
	 */
	if ( init_error ) {
		( void )content_complete( );
		exitcode = EXIT_ERROR;
	} else {
		if ( content_complete( ) ) {
			if (prbcld_xc != EXIT_NORMAL)
				exitcode = EXIT_ERROR;
			else
				exitcode = EXIT_NORMAL;
		} else {
			exitcode = EXIT_INTERRUPT;
			if ( mlog_get_hint() == RV_NONE )
				mlog_exit_hint(RV_INCOMPLETE);
		}
	}
	return mlog_exit(exitcode, RV_UNKNOWN);
}

#define ULO( f, o )	fprintf( stderr,		\
				 "%*s[ -%c %s ]\n",	\
				 ps,			\
				 ns,			\
				 o,			\
				 f ),			\
			ps = pfxsz

#define ULN( f )	fprintf( stderr,		\
				 "%*s[ %s ]\n",		\
				 ps,			\
				 ns,			\
				 f ),			\
			ps = pfxsz

void
usage( void )
{
	int pfxsz;
	int ps = 0;
	char *ns = "";

	pfxsz = fprintf(stderr, _("%s: usage: %s "),
				progname, basename(progname));

#ifdef DUMP
	ULO(_("(dump DMF dualstate files as offline)"),	GETOPT_DUMPASOFFLINE );
	ULO(_("<blocksize>"),				GETOPT_BLOCKSIZE );
	ULO(_("<media change alert program> "),		GETOPT_ALERTPROG );
	ULO(_("<dump media file size> "),		GETOPT_FILESZ );
	ULO(_("(allow files to be excluded)"),		GETOPT_EXCLUDEFILES );
	ULO(_("<destination> ..."),			GETOPT_DUMPDEST );
	ULO(_("(help)"),				GETOPT_HELP );
	ULO(_("<level>"),				GETOPT_LEVEL );
	ULO(_("(force usage of minimal rmt)"),		GETOPT_MINRMT );
	ULO(_("(overwrite tape)"),			GETOPT_OVERWRITE );
	ULO(_("<seconds between progress reports>"),	GETOPT_PROGRESS );
	ULO(_("<use QIC tape settings>"),		GETOPT_QIC );
	ULO(_("<subtree> ..."),				GETOPT_SUBTREE );
	ULO(_("<file> (use file mtime for dump time"),	GETOPT_DUMPTIME );
	ULO(_("<verbosity {silent, verbose, trace}>"),	GETOPT_VERBOSITY );
	ULO(_("<maximum file size>"),			GETOPT_MAXDUMPFILESIZE );
	ULO(_("(don't dump extended file attributes)"),	GETOPT_NOEXTATTR );
#ifdef BASED
	ULO(_("<base dump session id>"),		GETOPT_BASED );
#endif /* BASED */
#ifdef REVEAL
	ULO(_("(generate tape record checksums)"),	GETOPT_RECCHKSUM );
#endif /* REVEAL */
	ULO(_("(pre-erase media)"),			GETOPT_ERASE );
	ULO(_("(don't prompt)"),			GETOPT_FORCE );
#ifdef REVEAL
	ULO(_("<minimum thread stack size>"),		GETOPT_MINSTACKSZ );
	ULO(_("<maximum thread stack size>"),		GETOPT_MAXSTACKSZ );
#endif /* REVEAL */
	ULO(_("(display dump inventory)"),		GETOPT_INVPRINT );
	ULO(_("(inhibit inventory update)"),		GETOPT_NOINVUPDATE );
	ULO(_("<session label>"),			GETOPT_DUMPLABEL );
	ULO(_("<media label> ..."),			GETOPT_MEDIALABEL );
#ifdef REVEAL
	ULO(_("(timestamp messages)"),			GETOPT_TIMESTAMP );
#endif /* REVEAL */
	ULO(_("<options file>"),			GETOPT_OPTFILE );
#ifdef REVEAL
	ULO(_("(pin down I/O buffers)"),		GETOPT_RINGPIN );
#endif /* REVEAL */
	ULO(_("(resume)"),				GETOPT_RESUME );
	ULO(_("(don't timeout dialogs)"),		GETOPT_NOTIMEOUTS );
#ifdef REVEAL
	ULO(_("(unload media when change needed)"),	GETOPT_UNLOAD );
	ULO(_("(show subsystem in messages)"),		GETOPT_SHOWLOGSS );
	ULO(_("(show verbosity in messages)"),		GETOPT_SHOWLOGLEVEL );
#endif /* REVEAL */
	ULO(_("<I/O buffer ring length>"),		GETOPT_RINGLEN );
#ifdef REVEAL
	ULO(_("(miniroot restrictions)"),		GETOPT_MINIROOT );
#endif /* REVEAL */
	ULN(_("- (stdout)") );
	ULN(_("<source (mntpnt|device)>") );
#endif /* DUMP */
#ifdef RESTORE
	ULO(_("<alt. workspace dir> ..."),		GETOPT_WORKSPACE );
	ULO(_("<blocksize>"),				GETOPT_BLOCKSIZE );
	ULO(_("<media change alert program> "),		GETOPT_ALERTPROG );
	ULO(_("(don't overwrite existing files)"),	GETOPT_EXISTING );
	ULO(_("<source> ..."),				GETOPT_DUMPDEST );
	ULO(_("(help)"),				GETOPT_HELP );
	ULO(_("(interactive)"),				GETOPT_INTERACTIVE );
	ULO(_("(force usage of minimal rmt)"),		GETOPT_MINRMT );
	ULO(_("<file> (restore only if newer than)"),	GETOPT_NEWER );
	ULO(_("(restore owner/group even if not root)"),GETOPT_OWNER );
	ULO(_("<seconds between progress reports>"),	GETOPT_PROGRESS );
	ULO(_("<use QIC tape settings>"),		GETOPT_QIC );
	ULO(_("(cumulative restore)"),			GETOPT_CUMULATIVE );
	ULO(_("<subtree> ..."),				GETOPT_SUBTREE );
	ULO(_("(contents only)"),			GETOPT_TOC );
	ULO(_("<verbosity {silent, verbose, trace}>"),	GETOPT_VERBOSITY );
	ULO(_("(use small tree window)"),		GETOPT_SMALLWINDOW );
	ULO(_("(don't restore extended file attributes)"),GETOPT_NOEXTATTR );
	ULO(_("(restore root dir owner/permissions)"),	GETOPT_ROOTPERM );
	ULO(_("(restore DMAPI event settings)"),	GETOPT_SETDM );
#ifdef REVEAL
	ULO(_("(check tape record checksums)"),		GETOPT_RECCHKSUM );
#endif /* REVEAL */
	ULO(_("(don't overwrite if changed)"),		GETOPT_CHANGED );
	ULO(_("(don't prompt)"),			GETOPT_FORCE );
	ULO(_("(display dump inventory)"),		GETOPT_INVPRINT );
	ULO(_("(inhibit inventory update)"),		GETOPT_NOINVUPDATE );
	ULO(_("<session label>"),			GETOPT_DUMPLABEL );
#ifdef REVEAL
	ULO(_("(timestamp messages)"),			GETOPT_TIMESTAMP );
#endif /* REVEAL */
	ULO(_("<options file>"),			GETOPT_OPTFILE );
#ifdef REVEAL
	ULO(_("(pin down I/O buffers)"),		GETOPT_RINGPIN );
#endif /* REVEAL */
#ifdef SESSCPLT
	ULO(_("(force interrupted session completion)"),GETOPT_SESSCPLT );
#endif /* SESSCPLT */
	ULO(_("(resume)"),				GETOPT_RESUME );
	ULO(_("<session id>"),				GETOPT_SESSIONID );
	ULO(_("(don't timeout dialogs)"),		GETOPT_NOTIMEOUTS );
#ifdef REVEAL
	ULO(_("(unload media when change needed)"),	GETOPT_UNLOAD );
	ULO(_("(show subsystem in messages)"),		GETOPT_SHOWLOGSS );
	ULO(_("(show verbosity in messages)"),		GETOPT_SHOWLOGLEVEL );
#endif /* REVEAL */
	ULO(_("<excluded subtree> ..."),		GETOPT_NOSUBTREE );
	ULO(_("<I/O buffer ring length>"),		GETOPT_RINGLEN );
#ifdef REVEAL
	ULO(_("(miniroot restrictions)"),		GETOPT_MINIROOT );
#endif /* REVEAL */
	ULN(_("- (stdin)") );
	ULN(_("<destination>") );
#endif /* RESTORE */

	/* anywhere usage is called we will exit shortly after...
	 * catch all of those cases below
	 */

	(void) mlog_exit(EXIT_ERROR, RV_OPT);
}

/* returns TRUE if preemption
 */
bool_t
preemptchk( int flg )
{
	bool_t preempt_requested;

	/* see if a progress report needed
	 */
	if ( progrpt_enabledpr ) {
		time32_t now = time( 0 );
		bool_t need_progrptpr = BOOL_FALSE;
		while ( now >= progrpt_deadline ) {
			need_progrptpr = BOOL_TRUE;
			progrpt_deadline += progrpt_interval;
		}
		if ( need_progrptpr ) {
			size_t statlinecnt;
			char **statline;
			ix_t i;
			statlinecnt = content_statline( &statline );
			for ( i = 0 ; i < statlinecnt ; i++ ) {
				mlog( MLOG_NORMAL,
				      statline[ i ] );
			}
		}
	}

	/* Progress report only */
	if (flg == PREEMPT_PROGRESSONLY) {
		return BOOL_FALSE;
	}

	/* signals not caught in these cases
	 */
	if ( miniroot || pipeline ) {
		return BOOL_FALSE;
	}

	/* release signals momentarily to let any pending ones
	 * invoke signal handler and set flags
	 */
	sigrelse( SIGINT );
	sigrelse( SIGHUP );
	sigrelse( SIGTERM );
	sigrelse( SIGPIPE );
	sigrelse( SIGQUIT );

	sighold( SIGQUIT );
	sighold( SIGPIPE );
	sighold( SIGTERM );
	sighold( SIGHUP );
	sighold( SIGINT );

	preempt_requested = BOOL_FALSE;

	if ( sigint_received ) {
		mlog( MLOG_DEBUG | MLOG_PROC,
		      "SIGINT received (preempt)\n" );
		if ( dlog_allowed( )) {
			preempt_requested = sigint_dialog( );
		} else {
			preempt_requested = BOOL_TRUE;
		}
		/* important that this appear after dialog.
		 * allows dialog to be terminated with SIGINT,
		 * without infinite loop.
		 */
		sigint_received = BOOL_FALSE;
	}

	if ( sighup_received ) {
		mlog( MLOG_DEBUG | MLOG_PROC,
		      "SIGHUP received (prempt)\n" );
		preempt_requested = BOOL_TRUE;
		sighup_received = BOOL_FALSE;
	}

	if ( sigterm_received ) {
		mlog( MLOG_DEBUG | MLOG_PROC,
		      "SIGTERM received (prempt)\n" );
		preempt_requested = BOOL_TRUE;
		sigterm_received = BOOL_FALSE;
	}

	if ( sigpipe_received ) {
		mlog( MLOG_DEBUG | MLOG_PROC,
		      "SIGPIPE received\n" );
		preempt_requested = BOOL_TRUE;
		sigpipe_received = BOOL_FALSE;
	}
	
	if ( sigquit_received ) {
		mlog( MLOG_DEBUG | MLOG_PROC,
		      "SIGQUIT received (preempt)\n" );
		preempt_requested = BOOL_TRUE;
		sigquit_received = BOOL_FALSE;
	}

	return preempt_requested;
}

/* definition of locally defined static functions ****************************/

static bool_t
loadoptfile( intgen_t *argcp, char ***argvp )
{
	char *optfilename;
	ix_t optfileix = 0;
	intgen_t fd;
	size_t sz;
	intgen_t i;
	struct stat64 stat;
	char *argbuf;
	char *p;
	size_t tokencnt;
	intgen_t nread;
	const char *sep = " \t\n\r";
	char **newargv;
	intgen_t c;
	intgen_t rval;

	/* see if option specified
	 */
	optind = 1;
	opterr = 0;
	optfilename =  0;
	while ( ( c = getopt( *argcp, *argvp, GETOPT_CMDSTRING )) != EOF ) {
		switch ( c ) {
		case GETOPT_OPTFILE:
			if ( ! optarg || optarg[ 0 ] == '-' ) {
				mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_NOLOCK,
				      _("-%c argument missing\n"),
				      c );
				usage( );
				return BOOL_FALSE;
			}
			if ( optfilename ) {
				mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_NOLOCK,
				      _("-%c allowed only once\n"),
				      c );
				usage( );
				return BOOL_FALSE;
			}
			optfilename = optarg;
			ASSERT( optind > 2 );
			optfileix = ( ix_t )optind - 2;
			break;
		}
	}
	if ( ! optfilename )  {
		return BOOL_TRUE;
	}

	/* attempt to open the option  file
	 */
	errno = 0;
	fd = open( optfilename, O_RDONLY );
	if ( fd  < 0 ) {
		mlog( MLOG_ERROR | MLOG_NOLOCK,
		      _("cannot open option file %s: %s (%d)\n"),
		      optfilename,
		      strerror( errno ),
		      errno );
		return BOOL_FALSE;
	}

	/* get file status
	 */
	rval = fstat64( fd, &stat );
	if ( rval ) {
		mlog( MLOG_ERROR | MLOG_NOLOCK,
		      _("cannot stat option file %s: %s (%d)\n"),
		      optfilename,
		      strerror( errno ),
		      errno );
		close( fd );
		return BOOL_FALSE;
	}

	/* ensure the file is ordinary
	 */
	if ( ( stat.st_mode & S_IFMT ) != S_IFREG ) {
		mlog( MLOG_ERROR | MLOG_NOLOCK,
		      _("given option file %s is not ordinary file\n"),
		      optfilename );
		close( fd );
		return BOOL_FALSE;
	}

	/* calculate the space required for the cmd line options.
	 * skip the GETOPT_OPTFILE option which put us here!
	 */
	sz = 0;
	for ( i =  0 ; i < *argcp ; i++ ) {
		if ( i == ( intgen_t )optfileix ) {
			i++; /* to skip option argument */
			continue;
		}
		sz += strlen( ( * argvp )[ i ] ) + 1;
	}

	/* add in the size of the option file (plus one byte in case
	 * option file ends without newline, and one NULL for safety)
	 */
	sz += ( size_t )stat.st_size + 2;

	/* allocate an argument buffer
	 */
	argbuf = ( char * )malloc( sz );
	ASSERT( argbuf );

	/* copy arg0 (the executable's name ) in first
	 */
	p = argbuf;
	i = 0;
	sprintf( p, "%s ", ( * argvp )[ i ] );
	p += strlen( ( * argvp )[ i ] ) + 1;
	i++;

	/* copy the options file into the buffer after the given args
	 */
	nread = read( fd, ( void * )p, ( size_t )stat.st_size );
	if ( nread < 0 ) {
		mlog( MLOG_ERROR | MLOG_NOLOCK,
		      _("read of option file %s failed: %s (%d)\n"),
		      optfilename,
		      strerror( errno ),
		      errno );
		close( fd );
		return BOOL_FALSE;
	}
	ASSERT( ( off64_t )nread == stat.st_size );
	p += ( size_t )stat.st_size;
	*p++ = ' ';

	/* copy the remaining command line args into the buffer
	 */
	for ( ; i < *argcp ; i++ ) {
		if ( i == ( intgen_t )optfileix ) {
			i++; /* to skip option argument */
			continue;
		}
		sprintf( p, "%s ", ( * argvp )[ i ] );
		p += strlen( ( * argvp )[ i ] ) + 1;
	}

	/* null-terminate the entire buffer
	 */
	*p++ = 0;
	ASSERT( ( size_t )( p - argbuf ) <= sz );

	/* change newlines and carriage returns into spaces
	 */
	for ( p = argbuf ; *p ; p++ ) {
		if ( strchr( "\n\r", ( intgen_t )( *p ))) {
			*p = ' ';
		}
	}

	/* count the tokens in the buffer
	 */
	tokencnt = 0;
	p = argbuf;
	for ( ; ; ) {
		/* start at the first non-separator character
		 */
		while ( *p && strchr( sep, ( intgen_t )( *p ))) {
			p++;
		}

		/* done when NULL encountered
		 */
		if ( ! *p ) {
			break;
		}

		/* we have a token
		 */
		tokencnt++;

		/* find the end of the first token
		 */
		p = strpbrkquotes( p, sep );

		/* if no more separators, all tokens seen
		 */
		if ( ! p ) {
			break;
		}
	}

	/* if no arguments, can return now
	 */
	if ( ! tokencnt ) {
		close( fd );
		return BOOL_TRUE;
	}

	/* allocate a new argv array to hold the tokens
	 */
	newargv = ( char ** )calloc( tokencnt, sizeof( char * ));
	ASSERT( newargv );

	/* null-terminate tokens and place in new argv, after
	 * extracting quotes and escapes
	 */
	p = argbuf;
	for ( i = 0 ; ; i++ ) {
		char *endp = 0;

		/* start at the first non-separator character
		 */
		while ( *p && strchr( sep, ( intgen_t )*p )) {
			p++;
		}

		/* done when NULL encountered
		 */
		if ( ! *p ) {
			break;
		}

		/* better not disagree with counting scan!
		 */
		ASSERT( i < ( intgen_t )tokencnt );

		/* find the end of the first token
		 */
		endp = strpbrkquotes( p, sep );

		/* null-terminate if needed
		 */
		if ( endp ) {
			*endp = 0;
		}

		/* strip quotes and escapes
		 */
		p = stripquotes( p );

		/* stick result in new argv array
		 */
		newargv[ i ] = p;

		/* if no more separators, all tokens seen
		 */
		if ( ! endp ) {
			break;
		}

		p = endp + 1;
	}

	/* return new argc anr argv
	 */
	close( fd );
	*argcp = ( intgen_t )tokencnt;
	*argvp = newargv;
	return BOOL_TRUE;
}

#ifdef HIDDEN
static pid_t mrh_cid;
#endif

static bool_t
in_miniroot_heuristic( void )
{
	return BOOL_TRUE;

#ifdef HIDDEN
	SIG_PF prev_handler_hup;
	SIG_PF prev_handler_term;
	SIG_PF prev_handler_int;
	SIG_PF prev_handler_quit;
	SIG_PF prev_handler_cld;
	bool_t in_miniroot;

	/* attempt to call sproc.
	 */
	prev_handler_hup = sigset( SIGHUP, SIG_IGN );
	prev_handler_term = sigset( SIGTERM, SIG_IGN );
	prev_handler_int = sigset( SIGINT, SIG_IGN );
	prev_handler_quit = sigset( SIGQUIT, SIG_IGN );
	prev_handler_cld = sigset( SIGCLD, mrh_sighandler );
	( void )sighold( SIGCLD );
	mrh_cid = ( pid_t )sproc( ( void ( * )( void * ))exit, PR_SALL, 0 );
	if ( mrh_cid < 0 ) {
		in_miniroot = BOOL_TRUE;
	} else {
		while ( mrh_cid >= 0 ) {
			( void )sigpause( SIGCLD );
		}
		in_miniroot = BOOL_FALSE;
	}
	( void )sigset( SIGHUP, prev_handler_hup );
	( void )sigset( SIGTERM, prev_handler_term );
	( void )sigset( SIGINT, prev_handler_int );
	( void )sigset( SIGQUIT, prev_handler_quit );
	( void )sigset( SIGCLD, prev_handler_cld );

	return in_miniroot;
#endif /* HIDDEN */
}

#ifdef HIDDEN
static void
mrh_sighandler( int signo )
{
	if ( signo == SIGCLD ) {
		pid_t cid;
		intgen_t stat;

		cid = wait( &stat );
		if ( cid == mrh_cid ) {
			mrh_cid = -1;
		}
	}
}
#endif

/* parent and children share this handler. 
 */
static void
sighandler( int signo )
{
	/* get the pid and stream index
	 */
	pid_t pid = getpid( );
	intgen_t stix = stream_getix( pid );

	/* if in miniroot, don't do anything risky. just quit.
	 */
	if ( miniroot || pipeline ) {
		intgen_t rval;

		mlog( MLOG_TRACE | MLOG_NOTE | MLOG_NOLOCK | MLOG_PROC,
		      _("received signal %d (%s): cleanup and exit\n"),
		      signo,
		      sig_numstring( signo ));

		if ( content_complete( )) {
			rval = EXIT_NORMAL;
		} else {
			rval = EXIT_INTERRUPT;
		}
		mlog_exit(rval, RV_NONE);
		exit( rval );
	}

	/* if death of a child of a child, bury the child and return.
	 * probably rmt.
	 */
	if ( pid != parentpid && signo == SIGCLD ) {
		intgen_t stat;
		( void )wait( &stat );
		( void )sigset( signo, sighandler );
		return;
	}

	/* if neither parent nor managed child nor slave, exit
	 */
	if ( pid != parentpid && stix == -1 ) {
		exit( 0 );
	}

	/* parent signal handling
	 */
	if ( pid == parentpid ) {
		pid_t cid;
		intgen_t stat;
		switch ( signo ) {
		case SIGCLD:
			/* bury the child and notify the child manager
			 * abstraction of its death, and record death stats
			 */
			cid = wait( &stat );
			stix = stream_getix( cid );
			cldmgr_died( cid );
			if ( WIFSIGNALED( stat ) || WEXITSTATUS( stat ) > 0 ) {
				if ( prbcld_cnt == 0 ) {
					if ( WIFSIGNALED( stat )) {
						prbcld_pid = cid;
						prbcld_xc = 0;
						prbcld_signo = WTERMSIG( stat );
					} else if ( WEXITSTATUS( stat ) > 0 ) {
						prbcld_pid = cid;
						prbcld_xc = WEXITSTATUS( stat );
						prbcld_signo = 0;
					}
				}
				prbcld_cnt++;
			}
			( void )sigset( signo, sighandler );
			return;
		case SIGHUP:
			/* immediately disable further dialogs
			 */
			dlog_desist( );
			sighup_received = BOOL_TRUE;
			return;
		case SIGTERM:
			/* immediately disable further dialogs
			 */
			dlog_desist( );
			sigterm_received = BOOL_TRUE;
			return;
		case SIGINT:
			sigint_received = BOOL_TRUE;
			return;
		case SIGQUIT:
			/* immediately disable further dialogs
			 */
			dlog_desist( );
			sigquit_received = BOOL_TRUE;
			return;
		case SIGPIPE:
			/* immediately disable further dialogs,
			 * and ignore subsequent signals
			 */
			dlog_desist( );
			sigpipe_received = BOOL_TRUE;
			( void )sigset( signo, SIG_IGN );
			return;
		case SIGALRM:
			return;
		default:
			sigstray_received = signo;
			return;
		}
	}

	/* managed child handling
	 */
	if ( stream_getix( pid ) != -1 ) {
		switch ( signo ) {
		case SIGHUP:
			/* can get SIGHUP during dialog: just dismiss
			 */
			return;
		case SIGTERM:
			/* can get SIGTERM during dialog: just dismiss
			 */
			return;
		case SIGINT:
			/* can get SIGINT during dialog: just dismiss
			 */
			return;
		case SIGQUIT:
			/* can get SIGQUIT during dialog: just dismiss
			 */
			return;
		case SIGPIPE:
			/* forward write pipe failures to parent,
			 * and ignore subsequent failures
			 */
			dlog_desist( );
			kill( parentpid, SIGPIPE );
			( void )sigset( signo, SIG_IGN );
			return;
		case SIGALRM:
			/* accept and do nothing about alarm signals
			 */
			return;
		default:
			/* should not be any other captured signals:
			 * request a core dump
			 */
			mlog_exit( EXIT_FAULT, RV_NONE );
			exit( EXIT_FAULT );
			return;
		}
	}

	/* if some other child, just exit
	 */
	exit( 0 );
}

static int
childmain( void *arg1 )
{
	ix_t stix;
	intgen_t exitcode;
	drive_t *drivep;

	/* ignore signals
	 */
	sigset( SIGHUP, SIG_IGN );
	sigset( SIGTERM, SIG_IGN );
	sigset( SIGINT, SIG_IGN );
	sigset( SIGQUIT, SIG_IGN );
	sigset( SIGPIPE, SIG_IGN );
	sigset( SIGALRM, SIG_IGN );
	sigset( SIGCLD, SIG_IGN );

	/* Determine which stream I am.
	 */
	stix = ( ix_t )arg1;

	/* tell the content manager to begin.
	 */
#ifdef DUMP
	exitcode = content_stream_dump( stix );
#endif /* DUMP */
#ifdef RESTORE
	exitcode = content_stream_restore( stix );
#endif /* RESTORE */

	/* let the drive manager shut down its slave thread
	 */
	drivep = drivepp[ stix ];
	( * drivep->d_opsp->do_quit )( drivep );

	exit( exitcode );
}


/* ARGSUSED */
static void
prompt_prog_cb( void *uctxp, dlog_pcbp_t pcb, void *pctxp )
{
	/* query: ask for a dump label
	 */
	( * pcb )( pctxp,
		   progrpt_enabledpr
		   ?
		   _("please enter seconds between progress reports, "
		   "or 0 to disable")
		   :
		   _("please enter seconds between progress reports") );
}

/* SIGINTR dialog
 *
 * side affect is to change verbosity level.
 * return code of BOOL_TRUE indicates a stop was requested.
 */
#define PREAMBLEMAX	( 7 + 2 * STREAM_SIMMAX )
#define QUERYMAX	3
#define CHOICEMAX	9
#define ACKMAX		7
#define POSTAMBLEMAX	3

static bool_t
sigint_dialog( void )
{
	fold_t fold;
	char **statline;
	ix_t i;
	size_t statlinecnt;
	char *preamblestr[ PREAMBLEMAX ];
	size_t preamblecnt;
	char *querystr[ QUERYMAX ];
	size_t querycnt;
	char *choicestr[ CHOICEMAX ];
	size_t choicecnt;
	char *ackstr[ ACKMAX ];
	size_t ackcnt;
	char *postamblestr[ POSTAMBLEMAX ];
	size_t postamblecnt;
	size_t interruptix;
	size_t verbosityix;
	size_t metricsix;
	size_t controlix;
	size_t ioix;
	size_t mediachangeix;
#ifdef RESTORE
	size_t piix;
	size_t roix;
#endif /* RESTORE */
	size_t progix;
	size_t mllevix;
	size_t mlssix;
	size_t mltsix;
	size_t continueix;
	size_t allix;
	size_t nochangeix;
	size_t responseix;
	intgen_t ssselected = 0;
	bool_t stop_requested = BOOL_FALSE;

	/* preamble: the content status line, indicate if interrupt happening
	 */
	fold_init( fold, _("status and control dialog"), '=' );
	statlinecnt = content_statline( &statline );
	preamblecnt = 0;
	preamblestr[ preamblecnt++ ] = "\n";
	preamblestr[ preamblecnt++ ] = fold;
	preamblestr[ preamblecnt++ ] = "\n";
	preamblestr[ preamblecnt++ ] = "\n";
	for ( i = 0 ; i < statlinecnt ; i++ ) {
		preamblestr[ preamblecnt++ ] = statline[ i ];
	}
	if ( stop_in_progress ) {
		preamblestr[ preamblecnt++ ] =
			_("\nsession interrupt in progress\n");
	}
	preamblestr[ preamblecnt++ ] = "\n";
	ASSERT( preamblecnt <= PREAMBLEMAX );
	dlog_begin( preamblestr, preamblecnt );

	/* top-level query: a function of session interrupt status
	 */
	querycnt = 0;
	querystr[ querycnt++ ] = _("please select one of "
				 "the following operations\n");
	ASSERT( querycnt <= QUERYMAX );
	choicecnt = 0;
	if ( ! stop_in_progress ) {
		interruptix = choicecnt;
		choicestr[ choicecnt++ ] = _("interrupt this session");
	} else {
		interruptix = SIZEMAX; /* never happen */
	}

	verbosityix = choicecnt;
	choicestr[ choicecnt++ ] = _("change verbosity");
	metricsix = choicecnt;
	choicestr[ choicecnt++ ] = _("display metrics");
	if ( content_media_change_needed ) {
		mediachangeix = choicecnt;
		choicestr[ choicecnt++ ] = _("confirm media change");
	} else {
		mediachangeix = SIZEMAX; /* never happen */
	}
	controlix = choicecnt;
	choicestr[ choicecnt++ ] = _("other controls");
	continueix = choicecnt;
	choicestr[ choicecnt++ ] = _("continue");
	ASSERT( choicecnt <= CHOICEMAX );

	responseix = dlog_multi_query( querystr,
				       querycnt,
				       choicestr,
				       choicecnt,
				       0,		/* hilitestr */
				       IXMAX,		/* hiliteix */
				       0,		/* defaultstr */
				       continueix,	/* defaultix */
				       DLOG_TIMEOUT,	/* timeout */
				       continueix,	/* timeout ix */
				       continueix,	/* sigint ix */
				       continueix,	/* sighup ix */
				       continueix );	/* sigquit ix */
	if ( responseix == interruptix ) {
		ackcnt = 0;
		ackstr[ ackcnt++ ] = "\n";
		dlog_multi_ack( ackstr,
				ackcnt );
		querycnt = 0;
		querystr[ querycnt++ ] = _("please confirm\n");
		ASSERT( querycnt <= QUERYMAX );
		choicecnt = 0;
		interruptix = choicecnt;
		choicestr[ choicecnt++ ] = _("interrupt this session");
		nochangeix = choicecnt;
		choicestr[ choicecnt++ ] = _("continue");
		ASSERT( choicecnt <= CHOICEMAX );
		responseix = dlog_multi_query( querystr,
					       querycnt,
					       choicestr,
					       choicecnt,
					       0,	/* hilitestr */
					       IXMAX,	/* hiliteix */
					       0,       /* defaultstr */
					       nochangeix, /* defaultix */
					       DLOG_TIMEOUT,/* timeout */
					       nochangeix, /* timeout ix */
					       nochangeix, /* sigint ix */
					       nochangeix, /* sighup ix */
					       nochangeix);/* sigquit ix */
		ackcnt = 0;
		if ( responseix == nochangeix ) {
			ackstr[ ackcnt++ ] = _("continuing\n");
		} else {
			ackstr[ ackcnt++ ] = _("interrupt request accepted\n");
			stop_requested = BOOL_TRUE;
		}
		dlog_multi_ack( ackstr,
				ackcnt );
	} else if ( responseix == verbosityix ) {
		ackcnt = 0;
		ackstr[ ackcnt++ ] = "\n";
		dlog_multi_ack( ackstr,
				ackcnt );
		querycnt = 0;
		querystr[ querycnt++ ] = _("please select one of "
					 "the following subsystems\n");
		ASSERT( querycnt <= QUERYMAX );
		choicecnt = 0;
		/* number of lines must match number of subsystems
		 */
		for ( choicecnt = 0 ; choicecnt < MLOG_SS_CNT ; choicecnt++ ) {
			choicestr[ choicecnt ] = mlog_ss_names[ choicecnt ];
		}
		allix = choicecnt;
		choicestr[ choicecnt++ ] = _("all of the above");
		nochangeix = choicecnt;
		choicestr[ choicecnt++ ] = _("no change");
		ASSERT( choicecnt <= CHOICEMAX );
		responseix = dlog_multi_query( querystr,
					       querycnt,
					       choicestr,
					       choicecnt,
					       0,	/* hilitestr */
					       IXMAX,	/* hiliteix */
					       0,       /* defaultstr */
					       allix,   /* defaultix */
					       DLOG_TIMEOUT,/* timeout */
					       nochangeix, /* timeout ix */
					       nochangeix, /* sigint ix */
					       nochangeix, /* sighup ix */
					       nochangeix);/* sigquit ix */
		ackcnt = 0;
		if ( responseix == nochangeix ) {
			ackstr[ ackcnt++ ] = _("no change\n");
		} else if ( responseix == allix ) {
			ssselected = -1;
			ackstr[ ackcnt++ ] = _("all subsystems selected\n\n");
		} else {
			ssselected = ( intgen_t )responseix;
			ackstr[ ackcnt++ ] = "\n";
		}
		dlog_multi_ack( ackstr,
				ackcnt );
		if ( responseix != nochangeix ) {
			querycnt = 0;
			querystr[ querycnt++ ] = ("please select one of the "
						  "following verbosity levels\n");
			ASSERT( querycnt <= QUERYMAX );
			choicecnt = 0;
			choicestr[ choicecnt++ ] = _("silent");
			choicestr[ choicecnt++ ] = _("verbose");
			choicestr[ choicecnt++ ] = _("trace");
			choicestr[ choicecnt++ ] = _("debug");
			choicestr[ choicecnt++ ] = _("nitty");
			choicestr[ choicecnt++ ] = _("nitty + 1");
			nochangeix = choicecnt;
			choicestr[ choicecnt++ ] = _("no change");
			ASSERT( choicecnt <= CHOICEMAX );
			responseix = dlog_multi_query( querystr,
						       querycnt,
						       choicestr,
						       choicecnt,
						       ssselected == -1
						       ?
						       0
						       :
						    _(" (current)"),/* hilitestr */
						       ssselected == -1
						       ?
						       IXMAX
						       :
			     ( ix_t )mlog_level_ss[ ssselected ], /* hiliteix */
						       0,       /* defaultstr */
						      nochangeix,/* defaultix */
						    DLOG_TIMEOUT,/* timeout */
						    nochangeix, /* timeout ix */
						     nochangeix, /* sigint ix */
						     nochangeix, /* sighup ix */
						    nochangeix);/* sigquit ix */
			ackcnt = 0;
			if ( responseix == nochangeix
			     ||
			     ( ssselected >= 0
			       &&
			       responseix
			       ==
			       ( ix_t )mlog_level_ss[ ssselected ] )) {
				ackstr[ ackcnt++ ] = _("no change\n");
			} else {
				if ( ssselected < 0 ) {
					ix_t ssix;
					ASSERT( ssselected == -1 );
					for ( ssix = 0
					      ;
					      ssix < MLOG_SS_CNT
					      ;
					      ssix++ ) {
						mlog_level_ss[ ssix ] =
							( intgen_t )responseix;
					}
				} else {
					mlog_level_ss[ ssselected ] =
							( intgen_t )responseix;
				}
				ackstr[ ackcnt++ ] = _("level changed\n");
			}
			dlog_multi_ack( ackstr,
					ackcnt );
		}
	} else if ( responseix == metricsix ) {
		ackcnt = 0;
		ackstr[ ackcnt++ ] = "\n";
		dlog_multi_ack( ackstr,
				ackcnt );
		querycnt = 0;
		querystr[ querycnt++ ] = _("please select one of "
					  "the following metrics\n");
		ASSERT( querycnt <= QUERYMAX );
		choicecnt = 0;
		ioix = choicecnt;
		choicestr[ choicecnt++ ] = _("I/O");
#ifdef RESTORE
		piix = choicecnt;
		choicestr[ choicecnt++ ] = _("media inventory status");
		roix = choicecnt;
		choicestr[ choicecnt++ ] = _("needed media objects");
#endif /* RESTORE */
		nochangeix = choicecnt;
		choicestr[ choicecnt++ ] = _("continue");
		ASSERT( choicecnt <= CHOICEMAX );
		responseix = dlog_multi_query( querystr,
					       querycnt,
					       choicestr,
					       choicecnt,
					       0,           /* hilitestr */
					       IXMAX,       /* hiliteix */
					       0,           /* defaultstr */
					       nochangeix,  /* defaultix */
					       DLOG_TIMEOUT,
					       nochangeix, /* timeout ix */
					       nochangeix, /* sigint ix */
					       nochangeix, /* sighup ix */
					       nochangeix);/* sigquit ix */
		if ( responseix != nochangeix ) {
			ackcnt = 0;
			ackstr[ ackcnt++ ] = "\n";
			dlog_multi_ack( ackstr,
					ackcnt );
		}
		if ( responseix == ioix ) {
			drive_display_metrics( );
#ifdef RESTORE
		} else if ( responseix == piix ) {
			content_showinv( );
		} else if ( responseix == roix ) {
			content_showremainingobjects( );
#endif /* RESTORE */
		}

		if ( responseix != nochangeix ) {
			querycnt = 0;
			querystr[ querycnt++ ] = "\n";
			ASSERT( querycnt <= QUERYMAX );
			choicecnt = 0;
			nochangeix = choicecnt;
			choicestr[ choicecnt++ ] = _("continue");
			ASSERT( choicecnt <= CHOICEMAX );
			responseix = dlog_multi_query( querystr,
						       querycnt,
						       choicestr,
						       choicecnt,
						       0,        /* hilitestr */
						       IXMAX,    /* hiliteix */
						       0,       /* defaultstr */
						       nochangeix,/* defaultix*/
						       DLOG_TIMEOUT,
						       nochangeix,/*timeout ix*/
						       nochangeix,/* sigint ix*/
						       nochangeix,/* sighup ix*/
						       nochangeix);/*sigquitix*/
		}
		ackcnt = 0;
		ackstr[ ackcnt++ ] = _("continuing\n");
		dlog_multi_ack( ackstr,
				ackcnt );
	} else if ( responseix == mediachangeix ) {
		ackcnt = 0;
		dlog_multi_ack( ackstr,
				ackcnt );
		ackcnt = 0;
		ackstr[ ackcnt++ ] = content_mediachange_query( );
		dlog_multi_ack( ackstr,
				ackcnt );
	} else if ( responseix == controlix ) {
		ackcnt = 0;
		ackstr[ ackcnt++ ] = "\n";
		dlog_multi_ack( ackstr,
				ackcnt );
		querycnt = 0;
		querystr[ querycnt++ ] = _("please select one of "
					   "the following controls\n");
		ASSERT( querycnt <= QUERYMAX );
		choicecnt = 0;
		progix = choicecnt;
		if ( progrpt_enabledpr ) {
		    choicestr[ choicecnt++ ] = _("change interval of "
					         "or disable progress reports");
		} else {
		    choicestr[ choicecnt++ ] = _("enable progress reports");
		}
		mllevix = choicecnt;
		if ( mlog_showlevel ) {
			choicestr[ choicecnt++ ] = _("hide log message levels");
		} else {
			choicestr[ choicecnt++ ] = _("show log message levels");
		}
		mlssix = choicecnt;
		if ( mlog_showss ) {
			choicestr[ choicecnt++ ] = _("hide log message subsystems");
		} else {
			choicestr[ choicecnt++ ] = _("show log message subsystems");
		}
		mltsix = choicecnt;
		if ( mlog_timestamp ) {
			choicestr[ choicecnt++ ] = _("hide log message timestamps");
		} else {
			choicestr[ choicecnt++ ] = _("show log message timestamps");
		}
		nochangeix = choicecnt;
		choicestr[ choicecnt++ ] = _("continue");
		ASSERT( choicecnt <= CHOICEMAX );
		responseix = dlog_multi_query( querystr,
					       querycnt,
					       choicestr,
					       choicecnt,
					       0,           /* hilitestr */
					       IXMAX,       /* hiliteix */
					       0,           /* defaultstr */
					       nochangeix,  /* defaultix */
					       DLOG_TIMEOUT,
					       nochangeix, /* timeout ix */
					       nochangeix, /* sigint ix */
					       nochangeix, /* sighup ix */
					       nochangeix);/* sigquit ix */
		ackcnt = 0;
		if ( responseix == progix ) {
			char buf[ 10 ];
			const size_t ncix = 1;
			const size_t okix = 2;

			ackstr[ ackcnt++ ] = "\n";
			dlog_multi_ack( ackstr,
					ackcnt );
			ackcnt = 0;
			responseix = dlog_string_query( prompt_prog_cb,
							0,
							buf,
							sizeof( buf ),
							DLOG_TIMEOUT,
							ncix,/* timeout ix */
							ncix, /* sigint ix */
							ncix,  /* sighup ix */
							ncix,  /* sigquit ix */
							okix );
			if ( responseix == okix ) {
				intgen_t newinterval;
				newinterval = atoi( buf );
				if ( ! strlen( buf )) {
					ackstr[ ackcnt++ ] = _("no change\n");
				} else if ( newinterval > 0 ) {
					time32_t newdeadline;
					char intervalbuf[ 64 ];
					newdeadline = time( 0 ) + ( time32_t )newinterval;
					if ( progrpt_enabledpr ) {
						if ( ( time32_t )newinterval == progrpt_interval ) {
							ackstr[ ackcnt++ ] = _("no change\n");
						} else {
							ackstr[ ackcnt++ ] = _("changing progress report interval to ");
							sprintf( intervalbuf,
								 _("%d seconds\n"),
								 newinterval );
							ASSERT( strlen( intervalbuf )
								<
								sizeof( intervalbuf ));
							ackstr[ ackcnt++ ] = intervalbuf;
							if ( progrpt_deadline > newdeadline ) {
								progrpt_deadline = newdeadline;
							}
						}
					} else {
						ackstr[ ackcnt++ ] = _("enabling progress reports at ");
						sprintf( intervalbuf,
							 _("%d second intervals\n"),
							 newinterval );
						ASSERT( strlen( intervalbuf )
							<
							sizeof( intervalbuf ));
						ackstr[ ackcnt++ ] = intervalbuf;
						progrpt_enabledpr = BOOL_TRUE;
						progrpt_deadline = newdeadline;
					}
					progrpt_interval = ( time32_t )newinterval;
				} else {
					if ( progrpt_enabledpr ) {
						ackstr[ ackcnt++ ] = _("disabling progress reports\n");
					} else {
						ackstr[ ackcnt++ ] = _("no change\n");
					}
					progrpt_enabledpr = BOOL_FALSE;
				}
			} else {
				ackstr[ ackcnt++ ] = _("no change\n");
			}
		} else if ( responseix == mllevix ) {
			mlog_showlevel = ! mlog_showlevel;
			if ( mlog_showlevel ) {
				ackstr[ ackcnt++ ] = _("showing log message levels\n");
			} else {
				ackstr[ ackcnt++ ] = _("hiding log message levels\n");
			}
		} else if ( responseix == mlssix ) {
			mlog_showss = ! mlog_showss;
			if ( mlog_showss ) {
				ackstr[ ackcnt++ ] = _("showing log message subsystems\n");
			} else {
				ackstr[ ackcnt++ ] = _("hiding log message subsystems\n");
			}
		} else if ( responseix == mltsix ) {
			mlog_timestamp = ! mlog_timestamp;
			if ( mlog_timestamp ) {
				ackstr[ ackcnt++ ] = _("showing log message timestamps\n");
			} else {
				ackstr[ ackcnt++ ] = _("hiding log message timestamps\n");
			}
		}
		dlog_multi_ack( ackstr,
				ackcnt );
	} else {
		ackcnt = 0;
		ackstr[ ackcnt++ ] = _("continuing\n");
		dlog_multi_ack( ackstr,
				ackcnt );
	}

	fold_init( fold, _("end dialog"), '-' );
	postamblecnt = 0;
	postamblestr[ postamblecnt++ ] = "\n";
	postamblestr[ postamblecnt++ ] = fold;
	postamblestr[ postamblecnt++ ] = "\n\n";
	ASSERT( postamblecnt <= POSTAMBLEMAX );
	dlog_end( postamblestr,
		  postamblecnt );

	return stop_requested;
}

static char *
sigintstr( void )
{
	intgen_t ttyfd;
	static char buf[ 20 ];
	struct termios termios;
	cc_t intchr;
	intgen_t rval;

	ttyfd = dlog_fd( );
	if ( ttyfd == -1 ) {
		return 0;
	}

	rval = tcgetattr( ttyfd, &termios );
	if ( rval ) {
		mlog( MLOG_NITTY | MLOG_PROC,
		      "could not get controlling terminal information: %s\n",
		      strerror( errno ));
		return 0;
	}
	
	intchr = termios.c_cc[ VINTR ];
	mlog( MLOG_NITTY | MLOG_PROC,
	      "tty fd: %d; terminal interrupt character: %c (0%o)\n",
	      ttyfd,
	      intchr,
	      intchr );

	if ( intchr < ' ' ) {
		sprintf( buf, "^%c", intchr + '@' );
	} else if ( intchr == 0177 ) {
		sprintf( buf, "DEL" );
	} else {
		sprintf( buf, "%c", intchr );
	}
	ASSERT( strlen( buf ) < sizeof( buf ));

	return buf;
}

#ifdef DUMP
static bool_t
set_rlimits( void )
#endif /* DUMP */
#ifdef RESTORE
static bool_t
set_rlimits( size64_t *vmszp )
#endif /* RESTORE */
{
	struct rlimit64 rlimit64;
#ifdef RESTORE
	size64_t vmsz;
#endif /* RESTORE */
	/* REFERENCED */
	intgen_t rval;

	ASSERT( minstacksz <= maxstacksz );

	rval = getrlimit64( RLIMIT_AS, &rlimit64 );

	ASSERT( ! rval );
	mlog( MLOG_NITTY | MLOG_NOLOCK | MLOG_PROC,
	      "RLIMIT_AS org cur 0x%llx max 0x%llx\n",
	      rlimit64.rlim_cur,
	      rlimit64.rlim_max );
#ifdef RESTORE
	if (rlimit64.rlim_cur != RLIM64_INFINITY) {
		rlimit64.rlim_cur = rlimit64.rlim_max;
		( void )setrlimit64( RLIMIT_AS, &rlimit64 );
		rval = getrlimit64( RLIMIT_AS, &rlimit64 );
		ASSERT( ! rval );
		mlog( MLOG_NITTY | MLOG_NOLOCK | MLOG_PROC,
			"RLIMIT_VMEM now cur 0x%llx max 0x%llx\n",
			rlimit64.rlim_cur,
			rlimit64.rlim_max );
	}

	vmsz = ( size64_t )rlimit64.rlim_cur;
#endif /* RESTORE */
	
	ASSERT( minstacksz <= maxstacksz );
	rval = getrlimit64( RLIMIT_STACK, &rlimit64 );
	ASSERT( ! rval );
	mlog( MLOG_NITTY | MLOG_NOLOCK | MLOG_PROC,
	      "RLIMIT_STACK org cur 0x%llx max 0x%llx\n",
	      rlimit64.rlim_cur,
	      rlimit64.rlim_max );
	if ( rlimit64.rlim_cur < minstacksz ) {
		if ( rlimit64.rlim_max < minstacksz ) {
			mlog( MLOG_DEBUG
			      |
			      MLOG_NOLOCK
			      |
			      MLOG_PROC,
			      "raising stack size hard limit "
			      "from 0x%llx to 0x%llx\n",
			      rlimit64.rlim_max,
			      minstacksz );
			rlimit64.rlim_cur = minstacksz;
			rlimit64.rlim_max = minstacksz;
			( void )setrlimit64( RLIMIT_STACK, &rlimit64 );
			rval = getrlimit64( RLIMIT_STACK, &rlimit64 );
			ASSERT( ! rval );
			if ( rlimit64.rlim_cur < minstacksz ) {
				mlog( MLOG_NORMAL
				      |
				      MLOG_WARNING
				      |
				      MLOG_NOLOCK
				      |
				      MLOG_PROC,
				      _("unable to raise stack size hard limit "
				      "from 0x%llx to 0x%llx\n"),
				      rlimit64.rlim_max,
				      minstacksz );
			}
		} else {
			mlog( MLOG_DEBUG
			      |
			      MLOG_NOLOCK
			      |
			      MLOG_PROC,
			      "raising stack size soft limit "
			      "from 0x%llx to 0x%llx\n",
			      rlimit64.rlim_cur,
			      minstacksz );
			rlimit64.rlim_cur = minstacksz;
			( void )setrlimit64( RLIMIT_STACK, &rlimit64 );
			rval = getrlimit64( RLIMIT_STACK, &rlimit64 );
			ASSERT( ! rval );
			if ( rlimit64.rlim_cur < minstacksz ) {
				mlog( MLOG_NORMAL
				      |
				      MLOG_WARNING
				      |
				      MLOG_NOLOCK
				      |
				      MLOG_PROC,
				      _("unable to raise stack size soft limit "
				      "from 0x%llx to 0x%llx\n"),
				      rlimit64.rlim_cur,
				      minstacksz );
			}
		}
	} else if ( rlimit64.rlim_cur > maxstacksz ) {
		mlog( MLOG_DEBUG
		      |
		      MLOG_NOLOCK
		      |
		      MLOG_PROC,
		      "lowering stack size soft limit "
		      "from 0x%llx to 0x%llx\n",
		      rlimit64.rlim_cur,
		      maxstacksz );
		rlimit64.rlim_cur = maxstacksz;
		( void )setrlimit64( RLIMIT_STACK, &rlimit64 );
		rval = getrlimit64( RLIMIT_STACK, &rlimit64 );
		ASSERT( ! rval );
		if ( rlimit64.rlim_cur > maxstacksz ) {
			mlog( MLOG_NORMAL
			      |
			      MLOG_WARNING
			      |
			      MLOG_NOLOCK
			      |
			      MLOG_PROC,
			      _("unable to lower stack size soft limit "
			      "from 0x%llx to 0x%llx\n"),
			      rlimit64.rlim_cur,
			      maxstacksz );
		}
	}
	mlog( MLOG_NITTY | MLOG_NOLOCK | MLOG_PROC,
	      "RLIMIT_STACK new cur 0x%llx max 0x%llx\n",
	      rlimit64.rlim_cur,
	      rlimit64.rlim_max );

	rval = getrlimit64( RLIMIT_DATA, &rlimit64 );
	ASSERT( ! rval );
	mlog( MLOG_NITTY | MLOG_NOLOCK | MLOG_PROC,
	      "RLIMIT_DATA org cur 0x%llx max 0x%llx\n",
	      rlimit64.rlim_cur,
	      rlimit64.rlim_max );
	
	rval = getrlimit64( RLIMIT_FSIZE, &rlimit64 );
	ASSERT( ! rval );
	mlog( MLOG_NITTY | MLOG_NOLOCK | MLOG_PROC,
	      "RLIMIT_FSIZE org cur 0x%llx max 0x%llx\n",
	      rlimit64.rlim_cur,
	      rlimit64.rlim_max );
	rlimit64.rlim_cur = rlimit64.rlim_max;
	( void )setrlimit64( RLIMIT_FSIZE, &rlimit64 );
	rlimit64.rlim_cur = RLIM64_INFINITY;
	( void )setrlimit64( RLIMIT_FSIZE, &rlimit64 );
	rval = getrlimit64( RLIMIT_FSIZE, &rlimit64 );
	ASSERT( ! rval );
	mlog( MLOG_NITTY | MLOG_NOLOCK | MLOG_PROC,
	      "RLIMIT_FSIZE now cur 0x%llx max 0x%llx\n",
	      rlimit64.rlim_cur,
	      rlimit64.rlim_max );
	
	rval = getrlimit64( RLIMIT_CPU, &rlimit64 );
	ASSERT( ! rval );
	mlog( MLOG_NITTY | MLOG_NOLOCK | MLOG_PROC,
	      "RLIMIT_CPU cur 0x%llx max 0x%llx\n",
	      rlimit64.rlim_cur,
	      rlimit64.rlim_max );
	rlimit64.rlim_cur = rlimit64.rlim_max;
	( void )setrlimit64( RLIMIT_CPU, &rlimit64 );
	rval = getrlimit64( RLIMIT_CPU, &rlimit64 );
	ASSERT( ! rval );
	mlog( MLOG_NITTY | MLOG_NOLOCK | MLOG_PROC,
	      "RLIMIT_CPU now cur 0x%llx max 0x%llx\n",
	      rlimit64.rlim_cur,
	      rlimit64.rlim_max );

#ifdef RESTORE
	*vmszp = vmsz;
#endif /* RESTORE */
	return BOOL_TRUE;
}

struct exit_printmap {
	intgen_t code;
	char *string;
};

typedef struct exit_printmap exit_printmap_t;

static exit_printmap_t exit_printmap[ ] = {
	{EXIT_NORMAL,	"EXIT_NORMAL"},
	{EXIT_ERROR,	"EXIT_ERROR"},
	{EXIT_INTERRUPT,"EXIT_INTERRUPT"},
	{EXIT_FAULT,	"EXIT_FAULT"}
};

static char *
exit_codestring( intgen_t code )
{
	exit_printmap_t *p = exit_printmap;
	exit_printmap_t *endp = exit_printmap
				+
				( sizeof( exit_printmap )
				  /
				  sizeof( exit_printmap[ 0 ] ));
	for ( ; p < endp ; p++ ) {
		if ( p->code == code ) {
			return p->string;
		}
	}

	return "???";
}

struct sig_printmap {
	intgen_t num;
	char *string;
};

typedef struct sig_printmap sig_printmap_t;

static sig_printmap_t sig_printmap[ ] = {
	{SIGHUP,	"SIGHUP"},
	{SIGINT,	"SIGINT"},
	{SIGQUIT,	"SIGQUIT"},
	{SIGILL,	"SIGILL"},
	{SIGABRT,	"SIGABRT"},
	{SIGFPE,	"SIGFPE"},
	{SIGBUS,	"SIGBUS"},
	{SIGSEGV,	"SIGSEGV"},
#ifdef SIGSYS
	{SIGSYS,	"SIGSYS"},
#endif
	{SIGPIPE,	"SIGPIPE"},
	{SIGALRM,	"SIGALRM"},
	{SIGTERM,	"SIGTERM"},
	{SIGUSR1,	"SIGUSR1"},
	{SIGUSR2,	"SIGUSR2"},
	{SIGCLD,	"SIGCLD"},
	{SIGPWR,	"SIGPWR"},
	{SIGURG,	"SIGURG"},
	{SIGPOLL,	"SIGPOLL"},
	{SIGXCPU,	"SIGXCPU"},
	{SIGXFSZ,	"SIGXFSZ"},
#if HIDDEN
	{SIGRTMIN,	"SIGRTMIN"},
	{SIGRTMAX,	"SIGRTMAX"},
#endif
	{0,		"???"}
};

static char *
sig_numstring( intgen_t num )
{
	sig_printmap_t *p = sig_printmap;
	sig_printmap_t *endp = sig_printmap
			       +
			       ( sizeof( sig_printmap )
			         /
			         sizeof( sig_printmap[ 0 ] ));
	for ( ; p < endp ; p++ ) {
		if ( p->num == num ) {
			return p->string;
		}
	}

	return "???";
}

static char *
strpbrkquotes( char *p, const char *sep )
{
	bool_t prevcharwasbackslash = BOOL_FALSE;
	bool_t inquotes = BOOL_FALSE;

	for ( ; ; p++ ) {
		if ( *p == 0 ) {
			return 0;
		}

		if ( *p == '\\' ) {
			if ( ! prevcharwasbackslash ) {
				prevcharwasbackslash = BOOL_TRUE;
			} else {
				prevcharwasbackslash = BOOL_FALSE;
			}
			continue;
		}

		if ( *p == '"' ) {
			if ( prevcharwasbackslash ) {
				prevcharwasbackslash = BOOL_FALSE;
				continue;
			}
			if ( inquotes ) {
				inquotes = BOOL_FALSE;
			} else {
				inquotes = BOOL_TRUE;
			}
			continue;
		}

		if ( ! inquotes ) {
			if ( strchr( sep, ( intgen_t )( *p ))) {
				return p;
			}
		}

		prevcharwasbackslash = BOOL_FALSE;
	}
	/* NOTREACHED */
}

static char *
stripquotes( char *p )
{
	size_t len = strlen( p );
	char *endp;
	char *nextp;
	bool_t justremovedbackslash;

	if ( len > 2 && p[ 0 ] == '"' ) {
		p++;
		len--;
		if ( len && p[ len - 1 ] == '"' ) {
			p[ len - 1 ] = 0;
			len--;
		}
	}

	endp = p + len;
	justremovedbackslash = BOOL_FALSE;

	for ( nextp = p ; nextp < endp ; ) {
		if ( *nextp == '\\' && ! justremovedbackslash ) {
			shiftleftby1( nextp, endp );
			endp--;
			justremovedbackslash = BOOL_TRUE;
		} else {
			justremovedbackslash = BOOL_FALSE;
			nextp++;
		}
	}

	return p;
}

static void
shiftleftby1( char *p, char *endp )
{
	for ( ; p < endp ; p++ ) {
		*p = p[ 1 ];
	}
}
