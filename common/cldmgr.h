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
#ifndef CLDMGR_H
#define CLDMGR_H

/* cldmgr.[hc] - manages all child threads
 */

/* cldmgr_init - initializes child management
 * returns FALSE if trouble encountered.
 */
extern bool_t cldmgr_init( void );

/* cldmgr_create - creates a child thread. returns FALSE if trouble
 * encountered
 */
extern bool_t cldmgr_create( int ( * entry )( void *arg1 ),
			     u_intgen_t inh,
			     ix_t streamix,
			     char *descstr,
			     void *arg1 );

/* cldmgr_stop - asks all children to gracefully terminate, at the next
 * convenient point in their normal processing loop.
 */
extern void cldmgr_stop( void );

/* cldmgr_killall - kills all children
 */
extern void cldmgr_killall( void );

/* cldmgr_died - tells the child manager that the child died
 */
extern void cldmgr_died( pid_t pid );

/* cldmgr_stop_requested - returns TRUE if the child should gracefully
 * terminate.
 */
extern bool_t cldmgr_stop_requested( void );

/* cldmgr_pid2streamix - retrieves the stream index. returns -1 if
 * not associated with any stream.
 */
extern intgen_t cldmgr_pid2streamix( pid_t pid );

/* cldmgr_remainingcnt - returns number of children remaining
 */
extern size_t cldmgr_remainingcnt( void );

/* checks if any children serving any other streams are still alive
 */
extern bool_t cldmgr_otherstreamsremain( ix_t streamix );

#endif /* CLDMGR_H */
