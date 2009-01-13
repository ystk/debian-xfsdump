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
#ifndef STREAM_H
#define STREAM_H

/* stream.h	definitions pertaining to the dump/restore streams
 */

#define STREAM_SIMMAX	20
	/* maximum number of simultaneous streams.
	 */

/* stream exit codes
 */
#define STREAM_EXIT_SUCCESS	    0 /* stream completed successfully */
#define STREAM_EXIT_STOP	    1 /* thread requests a stop */
#define STREAM_EXIT_ABORT	    2 /* thread requests an abort */
#define STREAM_EXIT_CORE	    3 /* thread requests a core dump */


/* S_FREE: stream entry is available for use
 * S_RUNNING: stream is actively dumping/restoring
 * S_ZOMBIE: stream is dead but we're keeping it around because we want to
 * know something about its termination condition.
 */

typedef enum { S_FREE, S_RUNNING, S_ZOMBIE } stream_state_t;

extern void stream_init( void );
extern void stream_register( pid_t pid, intgen_t streamix );
extern void stream_dead( pid_t pid );
extern void stream_free( pid_t pid );
extern int stream_find_all( stream_state_t states[],
			    int nstates,
			    pid_t pids[],
			    int npids );
extern intgen_t stream_getix( pid_t pid );
extern void stream_set_code( pid_t pid, int code );
extern void stream_set_return( pid_t pid, rv_t rv );
extern void stream_set_hint( pid_t pid, rv_t rv );
extern bool_t stream_exists( pid_t pid );
extern bool_t stream_get_exit_status( pid_t pid,
				      stream_state_t states[],
				      int nstates,
				      stream_state_t *state,
				      intgen_t *ix,
				      int *exit_code,
				      rv_t *exit_return,
				      rv_t *exit_hint);
extern size_t stream_cnt( void );


#endif /* STREAM_H */
