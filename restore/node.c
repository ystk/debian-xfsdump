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

#include <sys/mman.h>
#include <errno.h>
#include <memory.h>
#include <limits.h>

#include "types.h"
#include "mlog.h"
#include "win.h"
#include "node.h"
#include "mmap.h"

#define max( a, b )	( ( ( a ) > ( b ) ) ? ( a ) : ( b ) )
#define min( a, b )	( ( ( a ) < ( b ) ) ? ( a ) : ( b ) )

extern size_t pgsz;
extern size_t pgmask;

/* node handle limits
 */
#ifdef NODECHK

/* macros for manipulating node handles when handle consistency
 * checking is enabled. the upper bits of a handle will be loaded
 * with the node gen count, described below.
 */
#define HDLGENCNT		4
#define	HDLGENSHIFT		( NBBY * sizeof ( nh_t ) - HDLGENCNT )
#define	HDLGENLOMASK		( ( 1 << HDLGENCNT ) - 1 )
#define	HDLGENMASK		( HDLGENLOMASK << HDLGENSHIFT )
#define HDLNIXCNT		HDLGENSHIFT
#define HDLNIXMASK		( ( 1 << HDLNIXCNT ) - 1 )
#define HDLGETGEN( h )		( ( u_char_t )				\
				  ( ( ( int )h >> HDLGENSHIFT )		\
				    &					\
				    HDLGENLOMASK ))
#define HDLGETNIX( h )		( ( nix_t )( ( int )h & HDLNIXMASK ))
#define HDLMKHDL( g, n )	( ( nh_t )( ( ( ( int )g << HDLGENSHIFT )\
					      &				\
					      HDLGENMASK )		\
					  |				\
					  ( ( int )n & HDLNIXMASK )))
#define NIX_MAX			( ( off64_t )HDLNIXMASK )

/* the housekeeping byte of each node will hold two check fields:
 * a gen count, initialized to the node ix and incremented each time a node
 * is allocated, to catch re-use of stale handles; and unique pattern, to
 * differentiate a valid node from random memory. two unique patterns will
 * be used; one when the node is on the free list, another when it is
 * allocated. this allows me to detect use of handles to nodes which have
 * be freed.
 */
#define HKPGENCNT		HDLGENCNT
#define HKPGENSHIFT		( NBBY - HKPGENCNT )
#define HKPGENLOMASK		( ( 1 << HKPGENCNT ) - 1 )
#define HKPGENMASK		( HKPGENLOMASK << HKPGENSHIFT )
#define HKPUNQCNT		( NBBY - HKPGENCNT )
#define HKPUNQMASK		( ( 1 << HKPUNQCNT ) - 1 )
#define HKPGETGEN( b )		( ( u_char_t )				\
				  ( ( ( int )b >> HKPGENSHIFT )		\
				    &					\
				    HKPGENLOMASK ))
#define HKPGETUNQ( b )		( ( u_char_t )( ( int )b & HKPUNQMASK ))
#define HKPMKHKP( g, u )	( ( u_char_t )				\
				  ( ( ( ( int )g << HKPGENSHIFT )	\
				      &					\
				      HKPGENMASK )			\
				    |					\
				    ( ( int )u & HKPUNQMASK )))

/* simple patterns for detecting a node
 */
#define NODEUNQFREE			0x9
#define NODEUNQALCD			0x6

#else /* NODECHK */

#define NIX_MAX			( ( ( off64_t )1			\
				    <<					\
				    ( ( off64_t )NBBY			\
				      *					\
				      ( off64_t )sizeof( nh_t )))	\
				  -					\
				  ( off64_t )2 ) /* 2 to avoid NH_NULL */

#endif /* NODECHK */

/* window constraints
 */
#define NODESPERSEGMIN	1000000

/* how many nodes to place on free list at a time
 */
#define VIRGSACRMAX	8192 /* fudged: 8192 48 byte nodes (24 or 96 pages) */

/* a node is identified internally by its index into the backing store.
 * this index is the offset of the node into the segmented portion of
 * backing store (follows the abstraction header page) divided by the
 * size of a node. a special index is reserved to represent the null
 * index. a type is defined for node index (nix_t). it is a 64 bit
 * unsigned to facilitate conversion from index to 64 bit offset.
 */
typedef off64_t nix_t;
#define NIX_NULL OFF64MAX
#define NIX2OFF( nix )	( nix * ( nix_t )node_hdrp->nh_nodesz )
#define OFF2NIX( noff )	( noff / ( nix_t )node_hdrp->nh_nodesz )

/* reserve the firstpage for a header to save persistent context
 */
#define NODE_HDRSZ	pgsz

struct node_hdr {
	size_t nh_nodesz;
		/* internal node size - user may see something smaller
		 */
	ix_t nh_nodehkix;
		/* index of byte in each node the user has reserved
		 * for use by me
		 */
	size_t nh_nodesperseg;
		/* an integral number of internal nodes must fit into a
		 * segment
		 */
	size_t nh_segsz;
		/* the backing store is partitoned into segment, which
		 * can be mapped into VM windows  by the win abstraction
		 */
	size64_t nh_segtblsz;
		/* the estimated size of the entire segment table.
		 */
	size_t nh_winmapmax;
		/* maximum number of windows which can be mapped
		 */
	size_t nh_nodealignsz;
		/* user's constraint on node alignment
		 */
	nix_t nh_freenix;
		/* index into backing store of first node of singly-linked
		 * list of free nodes
		 */
	off64_t nh_firstsegoff;
		/* offset into backing store of the first segment
		 */
	off64_t nh_virgsegreloff;
		/* offset (relative to beginning of first segment) into
		 * backing store of segment containing one or
		 * more virgin nodes. relative to beginning of segmented
		 * portion of backing store. bumped only when all of the
		 * nodes in the segment have been placed on the free list.
		 * when bumped, nh_virginrelnix is simultaneously set back
		 * to zero.
		 */
	nix_t nh_virgrelnix;
		/* relative node index within the segment identified by
		 * nh_virgsegreloff of the next node not yet placed on the
		 * free list. never reaches nh_nodesperseg: instead set
		 * to zero and bump nh_virgsegreloff by one segment.
		 */
};

typedef struct node_hdr node_hdr_t;

static node_hdr_t *node_hdrp;
static intgen_t node_fd;

/* ARGSUSED */
bool_t
node_init( intgen_t fd,
	   off64_t off,
	   size_t usrnodesz,
	   ix_t nodehkix,
	   size_t nodealignsz,
	   size64_t vmsz,
	   size64_t dirs_nondirs_cnt )
{
	size64_t nodesz;
	size64_t winmap_mem;
	size64_t segsz;
	size64_t segtablesz;
	size64_t nodesperseg;
	size64_t minsegsz;
	size64_t winmapmax;
	intgen_t rval;

	/* sanity checks
	 */
	ASSERT( sizeof( node_hdr_t ) <= NODE_HDRSZ );
	ASSERT( sizeof( nh_t ) < sizeof( off64_t ));
	ASSERT( nodehkix < usrnodesz );
	ASSERT( usrnodesz >= sizeof( char * ) + 1 );
		/* so node is at least big enough to hold
		 * the free list linkage and the housekeeping byte
		 */
	ASSERT( nodehkix > sizeof( char * ));
		/* since beginning of each node is used to
		 * link it in the free list.
		 */

	/* adjust the user's node size to meet user's alignment constraint
	*/
	nodesz = ( usrnodesz + nodealignsz - 1 ) & ~( nodealignsz - 1 );

#define	WINMAP_MAX	20	/* maximum number of windows to use */
#define	WINMAP_MIN	4	/* minimum number of windows to use */
#define	HARDLINK_FUDGE	1.2	/* approx 1.2 hard links per file */

	/* Calculate the expected size of the segment table using the number
	 * of dirs and non-dirs.  Since we don't know how many hard-links
	 * there will be, scale the size upward using HARDLINK_FUDGE.
	 */

	segtablesz = ( (size64_t)(HARDLINK_FUDGE * (double)dirs_nondirs_cnt) * nodesz);

	/* Figure out how much memory is available for use by winmaps, and
	 * use that to pick an appropriate winmapmax, segsz, and nodesperseg,
	 * the goal being that if at all possible we want the entire segment
	 * table to be mapped so that we aren't constantly mapping and
	 * unmapping winmaps.  There must be at least WINMAP_MIN winmaps
	 * because references can be held on more than one winmap at the
	 * same time.  More winmaps are generally better to reduce the
	 * number of nodes that are unmapped if unmapping does occur.
	 */

	minsegsz = pgsz * nodesz;	/* must be pgsz and nodesz multiple */
	winmap_mem = min(vmsz, segtablesz);
	segsz = (((winmap_mem / WINMAP_MAX) + minsegsz - 1) / minsegsz) * minsegsz;
	segsz = max(segsz, minsegsz);

	nodesperseg = segsz / nodesz;

	winmapmax = min(WINMAP_MAX, vmsz / segsz);
	winmapmax = max(winmapmax, WINMAP_MIN);

	/* map the abstraction header
	 */
	ASSERT( ( NODE_HDRSZ & pgmask ) == 0 );
	ASSERT( ! ( NODE_HDRSZ % pgsz ));
	ASSERT( off <= OFF64MAX );
	ASSERT( ! ( off % ( off64_t )pgsz ));
	node_hdrp = ( node_hdr_t * )mmap_autogrow(
					    NODE_HDRSZ,
					    fd,
					    off );
	if ( node_hdrp == (node_hdr_t *)-1 ) {
	    mlog( MLOG_NORMAL | MLOG_ERROR, _(
		  "unable to map node hdr of size %d: %s\n"),
		  NODE_HDRSZ,
		  strerror( errno ));
	    return BOOL_FALSE;
	}

	/* initialize and save persistent context.
	 */
	node_hdrp->nh_nodesz = nodesz;
	node_hdrp->nh_nodehkix = nodehkix;
	node_hdrp->nh_segsz = segsz;
	node_hdrp->nh_segtblsz = segtablesz;
	node_hdrp->nh_winmapmax = winmapmax;
	node_hdrp->nh_nodesperseg = nodesperseg;
	node_hdrp->nh_nodealignsz = nodealignsz;
	node_hdrp->nh_freenix = NIX_NULL;
	node_hdrp->nh_firstsegoff = off + ( off64_t )NODE_HDRSZ;
	node_hdrp->nh_virgsegreloff = 0;
	node_hdrp->nh_virgrelnix = 0;

	/* save transient context
	 */
	node_fd = fd;

	/* autogrow the first segment
	 */
	mlog( MLOG_DEBUG,
	      "pre-growing new node array segment at %lld "
	      "size %lld\n",
	      node_hdrp->nh_firstsegoff,
	      ( off64_t )node_hdrp->nh_segsz );
	rval = ftruncate64( node_fd,
			    node_hdrp->nh_firstsegoff
			    +
			    ( off64_t )node_hdrp->nh_segsz );
	if ( rval ) {
		mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_TREE, _(
		      "unable to autogrow first node segment: %s (%d)\n"),
		      strerror( errno ),
		      errno );
		return BOOL_FALSE;
	}

	/* initialize the window abstraction
	 */
	win_init( fd,
		  node_hdrp->nh_firstsegoff,
		  segsz,
		  segtablesz,
		  winmapmax );
	
	/* announce the results
	 */
	mlog( MLOG_DEBUG | MLOG_TREE,
	      "node_init:"
	      " vmsz = %llu (0x%llx)"
	      " segsz = %u (0x%x)"
	      " segtblsz = %llu (0x%llx)"
	      " nodesperseg = %u (0x%x)"
	      " winmapmax = %llu (0x%llx)"
	      "\n",
	      vmsz, vmsz,
	      segsz, segsz,
	      segtablesz, segtablesz,
	      nodesperseg, nodesperseg,
	      winmapmax, winmapmax );

	return BOOL_TRUE;
}

bool_t
node_sync( intgen_t fd, off64_t off )
{
	/* sanity checks
	 */
	ASSERT( sizeof( node_hdr_t ) <= NODE_HDRSZ );

	/* map the abstraction header
	 */
	ASSERT( ( NODE_HDRSZ & pgmask ) == 0 );
	ASSERT( off <= ( off64_t )OFF64MAX );
	ASSERT( ! ( off % ( off64_t )pgsz ));
	node_hdrp = ( node_hdr_t * )mmap_autogrow(
					    NODE_HDRSZ,
					    fd,
					    off );
	if ( node_hdrp == (node_hdr_t *)-1 ) {
		mlog( MLOG_NORMAL | MLOG_ERROR, _(
		      "unable to map node hdr of size %d: %s\n"),
		      NODE_HDRSZ,
		      strerror( errno ));
		return BOOL_FALSE;
	}

	/* save transient context
	 */
	node_fd = fd;

	/* initialize the window abstraction
	 */
	win_init( fd,
		  node_hdrp->nh_firstsegoff,
		  node_hdrp->nh_segsz,
		  node_hdrp->nh_segtblsz,
		  node_hdrp->nh_winmapmax );

	return BOOL_TRUE;
}

nh_t
node_alloc( void )
{
	nix_t nix;
	u_char_t *p;
	nh_t nh;
	register nix_t *linkagep;
#ifdef NODECHK
	register u_char_t *hkpp;
	register u_char_t gen;
	register u_char_t unq;
#endif /* NODECHK */

	/* if free list is depleted, map in a new window at the
	 * end of backing store. put all nodes on free list.
	 * initialize the gen count to the node index, and the unique
	 * pattern to the free pattern.
	 */
	if ( node_hdrp->nh_freenix == NIX_NULL ) {
		nix_t virgbegnix; /* abs. nix of first node in virg seg */
		nix_t virgendnix; /* abs. nix of next node after last */
		nix_t sacrcnt; /* how many virgins to put on free list */
		nix_t sacrnix; 

		ASSERT( node_hdrp->nh_virgrelnix
			<
			( nix_t )node_hdrp->nh_nodesperseg );
		virgbegnix = OFF2NIX( node_hdrp->nh_virgsegreloff )
			     +
			     node_hdrp->nh_virgrelnix;
		virgendnix =
		      OFF2NIX( ( node_hdrp->nh_virgsegreloff
			       +
			       ( off64_t )node_hdrp->nh_segsz ) );
#ifdef TREE_DEBUG
		mlog(MLOG_DEBUG | MLOG_TREE,
		   "node_alloc(): create freelist - "
		   "virg_begin=%lld virg_end=%lld\n",
		   virgbegnix, virgendnix); 
#endif
		ASSERT( virgendnix > virgbegnix );
		sacrcnt = min( VIRGSACRMAX, virgendnix - virgbegnix );
		ASSERT( sacrcnt >= 1 );
		p = 0; /* keep lint happy */
		win_map( NIX2OFF( virgbegnix ), ( void ** )&p );
		if (p == NULL)
		    return NH_NULL;
		node_hdrp->nh_freenix = virgbegnix;
		for ( sacrnix = virgbegnix
		      ;
		      sacrnix < virgbegnix + sacrcnt - 1
		      ;
		      p += node_hdrp->nh_nodesz, sacrnix++ ) {
			linkagep = ( nix_t * )p;
			*linkagep = sacrnix + 1;
#ifdef NODECHK
			hkpp = p + node_hdrp->nh_nodehkix;
			gen = ( u_char_t )sacrnix;
			*hkpp = ( u_char_t )HKPMKHKP( ( size_t )gen,
						      NODEUNQFREE );
#endif /* NODECHK */
		}
		linkagep = ( nix_t * )p;
		*linkagep = NIX_NULL;
#ifdef NODECHK
		hkpp = p + node_hdrp->nh_nodehkix;
		gen = ( u_char_t )sacrnix;
		*hkpp = HKPMKHKP( gen, NODEUNQFREE );
#endif /* NODECHK */
		node_hdrp->nh_virgrelnix += sacrcnt;
		win_unmap( node_hdrp->nh_virgsegreloff, ( void ** )&p );

		if ( node_hdrp->nh_virgrelnix
		     >=
		     ( nix_t )node_hdrp->nh_nodesperseg ) {
			intgen_t rval;
			ASSERT( node_hdrp->nh_virgrelnix
				==
				( nix_t )node_hdrp->nh_nodesperseg );
			ASSERT( node_hdrp->nh_virgsegreloff
				<=
				OFF64MAX - ( off64_t )node_hdrp->nh_segsz );
#ifdef TREE_DEBUG
			mlog(MLOG_DEBUG | MLOG_TREE,
			    "node_alloc(): runout of nodes for freelist in "
                            "this segment - nodes used = %lld\n", 
                            node_hdrp->nh_virgrelnix);
#endif
			node_hdrp->nh_virgsegreloff +=
					( off64_t )node_hdrp->nh_segsz;
			node_hdrp->nh_virgrelnix = 0;
			mlog( MLOG_DEBUG,
			      "pre-growing new node array segment at %lld "
			      "size %lld\n",
			      node_hdrp->nh_firstsegoff 
			      +
			      node_hdrp->nh_virgsegreloff
			      +
			      ( off64_t )node_hdrp->nh_segsz,
			      ( off64_t )node_hdrp->nh_segsz );
			rval = ftruncate64( node_fd,
					    node_hdrp->nh_firstsegoff 
					    +
					    node_hdrp->nh_virgsegreloff
					    +
					    ( off64_t )node_hdrp->nh_segsz );
			if ( rval ) {
				mlog( MLOG_NORMAL | MLOG_WARNING | MLOG_TREE, _(
				      "unable to autogrow node segment %llu: "
				      "%s (%d)\n"),
				      node_hdrp->nh_virgsegreloff
				      /
				      ( off64_t )node_hdrp->nh_segsz,
				      strerror( errno ),
				      errno );
			}
		}
	}

	/* map in window containing node at top of free list,
	 * and adjust free list.
	 */
	nix = node_hdrp->nh_freenix;
#ifdef TREE_DEBUG
	mlog(MLOG_DEBUG | MLOG_TREE,
	   "node_alloc(): win_map(%llu) and get head from node freelist\n",
           NIX2OFF(nix));
#endif
	win_map( NIX2OFF( nix ), ( void ** )&p );
	if (p == NULL)
	    return NH_NULL;
#ifdef NODECHK
	hkpp = p + node_hdrp->nh_nodehkix;
	unq = HKPGETUNQ( *hkpp );
	ASSERT( unq != NODEUNQALCD );
	ASSERT( unq == NODEUNQFREE );
#endif /* NODECHK */
	linkagep = ( nix_t * )p;
	node_hdrp->nh_freenix = *linkagep;

	/* clean the node
	 */
	memset( ( void * )p, 0, node_hdrp->nh_nodesz );

	/* build a handle for node
	 */
	ASSERT( nix <= NIX_MAX );
#ifdef NODECHK
	hkpp = p + ( int )node_hdrp->nh_nodehkix;
	gen = ( u_char_t )( HKPGETGEN( *p ) + ( u_char_t )1 );
	nh = HDLMKHDL( gen, nix );
	*hkpp = HKPMKHKP( gen, NODEUNQALCD );
#else /* NODECHK */
	nh = ( nh_t )nix;
#endif /* NODECHK */

	/* unmap window
	 */
#ifdef TREE_DEBUG
	mlog(MLOG_DEBUG | MLOG_TREE,
	   "node_alloc(): win_unmap(%llu)\n", NIX2OFF(nix));
#endif
	win_unmap( NIX2OFF( nix ), ( void ** )&p );

	return nh;
}

void *
node_map( nh_t nh )
{
	nix_t nix;
	u_char_t *p;
#ifdef NODECHK
	register u_char_t hkp;
	register u_char_t hdlgen;
	register u_char_t nodegen;
	register u_char_t nodeunq;
#endif /* NODECHK */

	ASSERT( nh != NH_NULL );

	/* convert the handle into an index
	 */
#ifdef NODECHK
	hdlgen = HDLGETGEN( nh );
	nix = HDLGETNIX( nh );
#else /* NODECHK */
	nix = ( nix_t )nh;
#endif /* NODECHK */

	ASSERT( nix <= NIX_MAX );

	/* map in
	 */
	p = 0; /* keep lint happy */
	win_map( NIX2OFF( nix ), ( void ** )&p );
	if (p == NULL)
	    return NULL;

#ifdef NODECHK
	hkp = *( p + node_hdrp->nh_nodehkix );
	nodegen = HKPGETGEN( hkp );
	nodeunq = HKPGETUNQ( hkp );
	ASSERT( nodegen == hdlgen );
	ASSERT( nodeunq != NODEUNQFREE );
	ASSERT( nodeunq == NODEUNQALCD );
#endif /* NODECHK */

	return ( void * )p;
}

/* ARGSUSED */
static void
node_unmap_internal( nh_t nh, void **pp, bool_t internalpr )
{
	nix_t nix;
#ifdef NODECHK
	register u_char_t hkp;
	register u_char_t hdlgen;
	register u_char_t nodegen;
	register u_char_t nodeunq;
#endif /* NODECHK */

	ASSERT( pp );
	ASSERT( *pp );
	ASSERT( nh != NH_NULL );

	/* convert the handle into an index
	 */
#ifdef NODECHK
	hdlgen = HDLGETGEN( nh );
	nix = HDLGETNIX( nh );
#else /* NODECHK */
	nix = ( nix_t )nh;
#endif /* NODECHK */

	ASSERT( nix <= NIX_MAX );

#ifdef NODECHK
	hkp = *( *( u_char_t ** )pp + node_hdrp->nh_nodehkix );
	nodegen = HKPGETGEN( hkp );
	ASSERT( nodegen == hdlgen );
	nodeunq = HKPGETUNQ( hkp );
	if ( ! internalpr ) {
		ASSERT( nodeunq != NODEUNQFREE );
		ASSERT( nodeunq == NODEUNQALCD );
	} else {
		ASSERT( nodeunq != NODEUNQALCD );
		ASSERT( nodeunq == NODEUNQFREE );
	}
#endif /* NODECHK */

	/* unmap the window containing the node
	 */
	win_unmap( NIX2OFF( nix ), pp ); /* zeros *pp */
}

void
node_unmap( nh_t nh, void **pp )
{
	node_unmap_internal( nh, pp, BOOL_FALSE );
}

void
node_free( nh_t *nhp )
{
	nh_t nh;
	nix_t nix;
	u_char_t *p;
	register nix_t *linkagep;
#ifdef NODECHK
	register u_char_t *hkpp;
	register u_char_t hdlgen;
	register u_char_t nodegen;
	register u_char_t nodeunq;
#endif /* NODECHK */

	ASSERT( nhp );
	nh = *nhp;
	ASSERT( nh != NH_NULL );

	/* convert the handle into an index
	 */
#ifdef NODECHK
	hdlgen = HDLGETGEN( nh );
	nix = HDLGETNIX( nh );
#else /* NODECHK */
	nix = ( nix_t )nh;
#endif /* NODECHK */

	ASSERT( nix <= NIX_MAX );

	/* map in
	 */
	p = ( u_char_t * )node_map( nh );
	if (p == NULL){
	    *nhp = NH_NULL;
	    return;
	}

#ifdef NODECHK
	/* fix up unique field
	 */
	hkpp = p + node_hdrp->nh_nodehkix;
	nodegen = HKPGETGEN( *hkpp );
	nodeunq = HKPGETUNQ( *hkpp );
	ASSERT( nodegen == hdlgen );
	ASSERT( nodeunq != NODEUNQFREE );
	ASSERT( nodeunq == NODEUNQALCD );
	*hkpp = HKPMKHKP( nodegen, NODEUNQFREE );
#endif /* NODECHK */

	/* put node on free list
	 */
	linkagep = ( nix_t * )p;
	*linkagep = node_hdrp->nh_freenix;
	node_hdrp->nh_freenix = nix;

	/* map out
	 */
	node_unmap_internal( nh, ( void ** )&p, BOOL_TRUE );

	/* invalidate caller's handle
	 */
	*nhp = NH_NULL;
}
