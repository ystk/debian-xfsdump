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

#include <sys/types.h>
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>
#include <memory.h>
#include <errno.h>

#include "types.h"
#include "mlog.h"
#include "qlock.h"
#include "mmap.h"

extern size_t pgsz;
extern size_t pgmask;

/* number of entries to add to the segmap if
 * it needs to be resized
 */
#define SEGMAP_INCR	16

/*
 * critical region
 */
#define CRITICAL_BEGIN()  if (!locksoffpr) qlock_lock( tranp->t_qlockh )
#define CRITICAL_END()    if (!locksoffpr) qlock_unlock( tranp->t_qlockh )

/* window descriptor
 */
struct win {
	size_t w_segix;
		/* index of segment mapped by this window
		 */
	void *w_p;
		/* window virtual address
		 */
	size_t w_refcnt;
		/* reference count
		 */
	struct win *w_nextp;
		/* LRU list forward linkage
		 */
	struct win *w_prevp;
		/* LRU list backward linkage
		 */
};

typedef struct win win_t;

/* forward declarations
 */
static void win_segmap_resize( size_t segix );

/* transient state
 */
struct tran {
	intgen_t t_fd;
		/* file descriptor of backing store to be windowed
		 */
	off64_t t_firstoff;
		/* offset of first seg within backing store (for mmap( ))
		 */
	size_t t_segsz;
		/* backing store segment / window size
		 */
	size_t t_winmax;
		/* maximum number of windows to allocate
		 */
	size_t t_wincnt;
		/* number of windows allocated
		 */
	size_t t_winmmaps;
		/* number of window mmap calls made
		 */
	win_t *t_lruheadp;
		/* LRU head (re-use from this end)
		 */
	win_t *t_lrutailp;
		/* LRU tail (put here when no refs)
		 */
	win_t **t_segmap;
		/* mapping from segment index to window. an entry
		 * points to a win_t struct if segment is currently
		 * mapped, otherwise the entry is NULL.
		 */
	size_t	t_segmaplen;
		/* number of segments currently represented in
		 * t_segmap array.
		 */
	qlockh_t t_qlockh;
		/* for establishing critical regions
		 */
};

typedef struct tran tran_t;

static tran_t *tranp = 0;
static bool_t locksoffpr = BOOL_FALSE;

/*
 * assumes called in region where only 1 thread can execute it
 */
void
win_locks_off(void)
{
	locksoffpr = BOOL_TRUE;
}

/*
 * assumes called in region where only 1 thread can execute it
 */
void
win_locks_on(void)
{
	locksoffpr = BOOL_FALSE;
}

/*
 * tell me how many windows I used for the tree
 */
size_t
win_getnum_mmaps(void)
{
	return tranp->t_winmmaps;
}

void
win_init( intgen_t fd,
	  off64_t firstoff,
	  size_t segsz,
	  size64_t segtablesz,
	  size_t winmax )
{
	/* validate parameters
	 */
	ASSERT( ( firstoff & ( off64_t )pgmask ) == 0 );
	ASSERT( ( segsz & pgmask ) == 0 );

	/* allocate and initialize transient state
	 */
	ASSERT( tranp == 0 );
	tranp = ( tran_t * )calloc( 1, sizeof( tran_t ));
	ASSERT( tranp );

	tranp->t_fd = fd;
	tranp->t_firstoff = firstoff;
	tranp->t_segsz = segsz;
	tranp->t_winmax = winmax;

	tranp->t_segmaplen = (size_t)(segtablesz / segsz) + 1;
	tranp->t_segmap = (win_t **)
	calloc( tranp->t_segmaplen, sizeof(win_t *) );
	ASSERT( tranp->t_segmap );

	/* initialize critical region enforcer
	 */
	tranp->t_qlockh = qlock_alloc( QLOCK_ORD_WIN );
}

void
win_map( off64_t off, void **pp )
{
	size_t offwithinseg;
	size_t segix;
	off64_t segoff;
	win_t *winp;

	CRITICAL_BEGIN();

	/* calculate offset within segment
	 */
	offwithinseg = ( size_t )( off % ( off64_t )tranp->t_segsz );

	/* calculate segment index
	 */
	segix = (size_t)( off / ( off64_t )tranp->t_segsz );

	/* calculate offset of segment
	 */
	segoff = off - ( off64_t )offwithinseg;

#ifdef TREE_DEBUG
	mlog(MLOG_DEBUG | MLOG_TREE | MLOG_NOLOCK,
	     "win_map(off=%lld,addr=%x): off within = %llu, segoff = %lld\n",
	      off, pp, offwithinseg, segoff);
#endif
	/* resize the array if necessary */
	if ( segix >= tranp->t_segmaplen )
		win_segmap_resize( segix );

	/* see if segment already mapped. if ref cnt zero,
	 * remove from LRU list.
	 */
	winp = tranp->t_segmap[segix];
	if ( winp ) {
#ifdef TREE_DEBUG
		mlog(MLOG_DEBUG | MLOG_TREE | MLOG_NOLOCK,
		     "win_map(): requested segment already mapped\n");
#endif
		if ( winp->w_refcnt == 0 ) {
			ASSERT( tranp->t_lruheadp );
			ASSERT( tranp->t_lrutailp );
			if ( tranp->t_lruheadp == winp ) {
				if ( tranp->t_lrutailp == winp ) {
					tranp->t_lruheadp = 0;
					tranp->t_lrutailp = 0;
				} else {
					tranp->t_lruheadp = winp->w_nextp;
					tranp->t_lruheadp->w_prevp = 0;
				}
			} else {
				if ( tranp->t_lrutailp == winp ) {
					tranp->t_lrutailp = winp->w_prevp;
					tranp->t_lrutailp->w_nextp = 0;
				} else {
					winp->w_prevp->w_nextp = winp->w_nextp;
					winp->w_nextp->w_prevp = winp->w_prevp;
				}
			}
			winp->w_prevp = 0;
			winp->w_nextp = 0;
		} else {
			ASSERT( ! winp->w_prevp );
			ASSERT( ! winp->w_nextp );
		}
		winp->w_refcnt++;
		*pp = ( void * )( ( char * )( winp->w_p ) + offwithinseg );
		CRITICAL_END();
		return;
	}

	/* Allocate a new descriptor if we haven't yet hit the maximum,
	 * otherwise reuse any descriptor on the LRU list.
	 */
	if ( tranp->t_wincnt < tranp->t_winmax ) {
#ifdef TREE_DEBUG
		mlog(MLOG_DEBUG | MLOG_TREE | MLOG_NOLOCK,
		     "win_map(): create a new window\n");
#endif
		winp = ( win_t * )calloc( 1, sizeof( win_t ));
		ASSERT( winp );
		tranp->t_wincnt++;
	} else if ( tranp->t_lruheadp ) {
		/* REFERENCED */
		intgen_t rval;
#ifdef TREE_DEBUG
		mlog(MLOG_DEBUG | MLOG_TREE | MLOG_NOLOCK,
		     "win_map(): get head from lru freelist & unmap\n");
#endif
		ASSERT( tranp->t_lrutailp );
		winp = tranp->t_lruheadp;
		tranp->t_lruheadp = winp->w_nextp;
		if ( tranp->t_lruheadp ) {
			tranp->t_lruheadp->w_prevp = 0;
		} else {
			tranp->t_lrutailp = 0;
		}
		tranp->t_segmap[winp->w_segix] = NULL;
		rval = munmap( winp->w_p, tranp->t_segsz );
		ASSERT( ! rval );
		memset( ( void * )winp, 0, sizeof( win_t ));
	} else {
		ASSERT( tranp->t_wincnt == tranp->t_winmax );
		*pp = NULL;
		CRITICAL_END();
		mlog( MLOG_NORMAL | MLOG_WARNING, _(
		      "all map windows in use. Check virtual memory limits\n"));
		return;
	}

	/* map the window
	 */
	ASSERT( tranp->t_segsz >= 1 );
	ASSERT( tranp->t_firstoff
		<=
		OFF64MAX - segoff - ( off64_t )tranp->t_segsz + 1ll );
	ASSERT( ! ( tranp->t_segsz % pgsz ));
	ASSERT( ! ( ( tranp->t_firstoff + segoff ) % ( off64_t )pgsz ));
#ifdef TREE_DEBUG
	mlog(MLOG_DEBUG | MLOG_TREE | MLOG_NOLOCK,
	     "win_map(): mmap segment at %lld, size = %llu\n",
	    ( off64_t )( tranp->t_firstoff + segoff ), tranp->t_segsz);
#endif
	tranp->t_winmmaps++;
	winp->w_p = mmap_autogrow(
			    tranp->t_segsz,
			    tranp->t_fd,
			    ( off64_t )( tranp->t_firstoff + segoff ));
	if ( winp->w_p == (void *)-1 ) {
		int	error = errno;
		mlog( MLOG_NORMAL | MLOG_ERROR, _(
		      "win_map(): unable to map a node segment of size %d at %d: %s\n"),
		      tranp->t_segsz, tranp->t_firstoff + segoff,
		      strerror( error ));

		tranp->t_wincnt--;
		tranp->t_winmax--;
		CRITICAL_END();
		free(winp);

		if (error == ENOMEM && tranp->t_lruheadp) {
			mlog( MLOG_NORMAL | MLOG_ERROR,
		      		_("win_map(): try to select a different win_t\n"));
			win_map(off, pp);
			return;
		}
		*pp = NULL;
		return;
	}
	winp->w_segix  = segix;
	ASSERT( winp->w_refcnt == 0 );
	winp->w_refcnt++;
	tranp->t_segmap[winp->w_segix] = winp;

	*pp = ( void * )( ( char * )( winp->w_p ) + offwithinseg );

	CRITICAL_END();
}

void
win_unmap( off64_t off, void **pp )
{
	size_t segix;
	win_t *winp;

	CRITICAL_BEGIN();

	/* calculate segment index
	 */
	segix = (size_t)( off / ( off64_t )tranp->t_segsz );

	/* verify window mapped
	 */
	ASSERT( segix < tranp->t_segmaplen );
	winp = tranp->t_segmap[segix];
	ASSERT( winp );

	/* validate p
	 */
	ASSERT( pp );
	ASSERT( *pp );
	ASSERT( *pp >= winp->w_p );
	ASSERT( *pp < ( void * )( ( char * )( winp->w_p ) + tranp->t_segsz ));

	/* decrement the reference count. if zero, place at tail of LRU list.
	 */
	ASSERT( winp->w_refcnt > 0 );
	winp->w_refcnt--;
	ASSERT( ! winp->w_prevp );
	ASSERT( ! winp->w_nextp );
	if ( winp->w_refcnt == 0 ) {
		if ( tranp->t_lrutailp ) {
			ASSERT( tranp->t_lruheadp );
			winp->w_prevp = tranp->t_lrutailp;
			tranp->t_lrutailp->w_nextp = winp;
			tranp->t_lrutailp = winp;
		} else {
			ASSERT( ! tranp->t_lruheadp );
			ASSERT( ! winp->w_prevp );
			tranp->t_lruheadp = winp;
			tranp->t_lrutailp = winp;
		}
		ASSERT( ! winp->w_nextp );
	}

	/* zero the caller's pointer
	 */
	*pp = 0;

	CRITICAL_END();
}

static void
win_segmap_resize(size_t segix)
{
	size_t oldlen;
	win_t **new_part;

	oldlen = tranp->t_segmaplen;

	tranp->t_segmaplen = segix + SEGMAP_INCR;
	tranp->t_segmap = (win_t **)
		realloc( tranp->t_segmap, tranp->t_segmaplen * sizeof(win_t *) );
	ASSERT( tranp->t_segmap );

	/* clear the new portion of the array */
	new_part = tranp->t_segmap + oldlen;
	memset( new_part, 0, (tranp->t_segmaplen - oldlen) * sizeof(win_t *) );
}
