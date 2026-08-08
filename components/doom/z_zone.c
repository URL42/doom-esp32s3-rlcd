//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Zone Memory Allocation. Neat.
//


#include "z_zone.h"
#include "i_system.h"
#include "doomtype.h"


//
// ZONE MEMORY ALLOCATION
//
// There is never any space between memblocks,
//  and there will never be two contiguous free memblocks.
// The rover can be left pointing at a non-empty block.
//
// It is of no value to free a cachable block,
//  because it will get overwritten automatically if needed.
// 
 
#define MEM_ALIGN sizeof(void *)
#define ZONEID	0x1d4a11

typedef struct memblock_s
{
    int			size;	// including the header and possibly tiny fragments
    void**		user;
    int			tag;	// PU_FREE if this is free
    int			id;	// should be ZONEID
    struct memblock_s*	next;
    struct memblock_s*	prev;
#if ZONE_DEBUG
    // Provenance, see the ZONE_DEBUG comment in z_zone.h. 'caller' is the
    // return address of whoever called Z_Malloc -- run it through
    // xtensa-esp32s3-elf-addr2line to get file:line. 'seq' is a monotonic
    // allocation counter, which makes it possible to say whether a block is
    // ancient (allocated at startup) or was handed out moments before the
    // heap broke.
    void*		caller;
    unsigned		seq;
#endif
} memblock_t;


typedef struct
{
    // total bytes malloced, including header
    int		size;

    // start / end cap for linked list
    memblock_t	blocklist;
    
    memblock_t*	rover;
    
} memzone_t;



memzone_t*	mainzone;

#if ZONE_DEBUG
static unsigned	zone_alloc_seq;
static int	zone_reported;		// only ever print the first failure

// Snapshot of the zone's extent, taken in Z_Init and never touched again.
//
// The validator must not take its bounds from mainzone->size, because that
// lives in the first four bytes of the zone -- inside the object it is trying
// to distrust. A wild write that enlarges it would widen the accepted address
// range and let the walk follow pointers out of the allocation, which is
// precisely the failure this function exists to not have.
static const byte*	zone_lo;
static const byte*	zone_hi;
#endif



//
// Z_ClearZone
//
void Z_ClearZone (memzone_t* zone)
{
    memblock_t*		block;
	
    // set the entire zone to one free block
    zone->blocklist.next =
	zone->blocklist.prev =
	block = (memblock_t *)( (byte *)zone + sizeof(memzone_t) );
    
    zone->blocklist.user = (void *)zone;
    zone->blocklist.tag = PU_STATIC;
    zone->rover = block;
	
    block->prev = block->next = &zone->blocklist;
    
    // a free block.
    block->tag = PU_FREE;

    block->size = zone->size - sizeof(memzone_t);
}



//
// Z_Init
//
void Z_Init (void)
{
    memblock_t*	block;
    int		size;

    mainzone = (memzone_t *)I_ZoneBase (&size);
    mainzone->size = size;

#if ZONE_DEBUG
    zone_lo = (const byte *)mainzone;
    zone_hi = zone_lo + size;
#endif

    // set the entire zone to one free block
    mainzone->blocklist.next =
	mainzone->blocklist.prev =
	block = (memblock_t *)( (byte *)mainzone + sizeof(memzone_t) );

    mainzone->blocklist.user = (void *)mainzone;
    mainzone->blocklist.tag = PU_STATIC;
    mainzone->rover = block;
	
    block->prev = block->next = &mainzone->blocklist;

    // free block
    block->tag = PU_FREE;
    
    block->size = mainzone->size - sizeof(memzone_t);
}


//
// Z_Free
//
void Z_Free (void* ptr)
{
    memblock_t*		block;
    memblock_t*		other;
	
    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
	I_Error ("Z_Free: freed a pointer without ZONEID");
		
    if (block->tag != PU_FREE && block->user != NULL)
    {
    	// clear the user's mark
	    *block->user = 0;
    }

    // mark as free
    block->tag = PU_FREE;
    block->user = NULL;
    block->id = 0;
	
    other = block->prev;

    if (other->tag == PU_FREE)
    {
        // merge with previous free block
        other->size += block->size;
        other->next = block->next;
        other->next->prev = other;

        if (block == mainzone->rover)
            mainzone->rover = other;

        block = other;
    }
	
    other = block->next;
    if (other->tag == PU_FREE)
    {
        // merge the next free block onto the end
        block->size += other->size;
        block->next = other->next;
        block->next->prev = block;

        if (other == mainzone->rover)
            mainzone->rover = block;
    }
}



//
// Z_Malloc
// You can pass a NULL user if the tag is < PU_PURGELEVEL.
//
#define MINFRAGMENT		64


void*
Z_Malloc
( int		size,
  int		tag,
  void*		user )
{
    int		extra;
    memblock_t*	start;
    memblock_t* rover;
    memblock_t* newblock;
    memblock_t*	base;
    void *result;

    size = (size + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
    
    // scan through the block list,
    // looking for the first free block
    // of sufficient size,
    // throwing out any purgable blocks along the way.

    // account for size of block header
    size += sizeof(memblock_t);
    
    // if there is a free block behind the rover,
    //  back up over them
    base = mainzone->rover;
    
    if (base->prev->tag == PU_FREE)
        base = base->prev;
	
    rover = base;
    start = base->prev;
	
    do
    {
        if (rover == start)
        {
            // scanned all the way around the list
            I_Error ("Z_Malloc: failed on allocation of %i bytes", size);
        }
	
        if (rover->tag != PU_FREE)
        {
            if (rover->tag < PU_PURGELEVEL)
            {
                // hit a block that can't be purged,
                // so move base past it
                base = rover = rover->next;
            }
            else
            {
                // free the rover block (adding the size to base)

                // the rover can be the base block
                base = base->prev;
                Z_Free ((byte *)rover+sizeof(memblock_t));
                base = base->next;
                rover = base->next;
            }
        }
        else
        {
            rover = rover->next;
        }

    } while (base->tag != PU_FREE || base->size < size);

    
    // found a block big enough
    extra = base->size - size;
    
    if (extra >  MINFRAGMENT)
    {
        // there will be a free fragment after the allocated block
        newblock = (memblock_t *) ((byte *)base + size );
        newblock->size = extra;
	
        newblock->tag = PU_FREE;
        newblock->user = NULL;
#if ZONE_DEBUG
        // A fresh fragment has never had an owner, so it gets none. Note
        // this differs deliberately from Z_Free, which leaves caller/seq
        // alone: a freed block keeps its last owner, which is usually the
        // more informative answer when one turns up next to damage.
        newblock->caller = NULL;
        newblock->seq = 0;
#endif
        newblock->prev = base;
        newblock->next = base->next;
        newblock->next->prev = newblock;

        base->next = newblock;
        base->size = size;
    }
	
	if (user == NULL && tag >= PU_PURGELEVEL)
	    I_Error ("Z_Malloc: an owner is required for purgable blocks");

    base->user = user;
    base->tag = tag;

#if ZONE_DEBUG
    // __builtin_return_address(0) is the address Z_Malloc will return to,
    // i.e. the instruction after the call site. GCC gives us this for free
    // from the frame, so there is no need to macro-wrap Z_Malloc and touch
    // its ~200 call sites just to record __FILE__/__LINE__.
    base->caller = __builtin_return_address(0);
    base->seq = ++zone_alloc_seq;
#endif

    result  = (void *) ((byte *)base + sizeof(memblock_t));

    if (base->user)
    {
        *base->user = result;
    }

    // next allocation will start looking here
    mainzone->rover = base->next;	
	
    base->id = ZONEID;
    
    return result;
}



//
// Z_FreeTags
//
void
Z_FreeTags
( int		lowtag,
  int		hightag )
{
    memblock_t*	block;
    memblock_t*	next;
	
    for (block = mainzone->blocklist.next ;
	 block != &mainzone->blocklist ;
	 block = next)
    {
	// get link before freeing
	next = block->next;

	// free block?
	if (block->tag == PU_FREE)
	    continue;
	
	if (block->tag >= lowtag && block->tag <= hightag)
	    Z_Free ( (byte *)block+sizeof(memblock_t));
    }
}



//
// Z_DumpHeap
// Note: TFileDumpHeap( stdout ) ?
//
void
Z_DumpHeap
( int		lowtag,
  int		hightag )
{
    memblock_t*	block;
	
    printf ("zone size: %i  location: %p\n",
	    mainzone->size,mainzone);
    
    printf ("tag range: %i to %i\n",
	    lowtag, hightag);
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	if (block->tag >= lowtag && block->tag <= hightag)
	    printf ("block:%p    size:%7i    user:%p    tag:%3i\n",
		    block, block->size, block->user, block->tag);
		
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    printf ("ERROR: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    printf ("ERROR: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    printf ("ERROR: two consecutive free blocks\n");
    }
}


//
// Z_FileDumpHeap
//
void Z_FileDumpHeap (FILE* f)
{
    memblock_t*	block;
	
    fprintf (f,"zone size: %i  location: %p\n",mainzone->size,mainzone);
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	fprintf (f,"block:%p    size:%7i    user:%p    tag:%3i\n",
		 block, block->size, block->user, block->tag);
		
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    fprintf (f,"ERROR: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    fprintf (f,"ERROR: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    fprintf (f,"ERROR: two consecutive free blocks\n");
    }
}



//
// Z_CheckHeap
//
void Z_CheckHeap (void)
{
    memblock_t*	block;
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    I_Error ("Z_CheckHeap: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    I_Error ("Z_CheckHeap: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    I_Error ("Z_CheckHeap: two consecutive free blocks\n");
    }
}


#if ZONE_DEBUG

//
// Z_ValidateHeap
//
// Z_CheckHeap trusts the list it is checking: it dereferences block->next
// before it has established that block->next is a plausible pointer, so a
// heap that has already been damaged kills the checker rather than being
// reported by it. This version validates every pointer before following it.
//

static int Z_BlockInZone (const memblock_t *block)
{
    const byte *p = (const byte *)block;

    if (p < zone_lo || p + sizeof(memblock_t) > zone_hi)
	return 0;

    if (((size_t)p & (MEM_ALIGN - 1)) != 0)
	return 0;

    return 1;
}

static void Z_DumpBlock (const char *label, const memblock_t *block)
{
    if (block == NULL)
    {
	printf ("  %-4s @ (null)\n", label);
	return;
    }

    if (block == &mainzone->blocklist)
    {
	// The list head is not an allocation. Z_Init never initialises its
	// size or id, so dumping it prints noise, and "the block in front
	// overran its buffer" is meaningless for a sentinel.
	printf ("  %-4s @ %p  the blocklist head -- the damage is at the "
		"first block, nothing precedes it\n", label,
		(const void *)block);
	return;
    }

    if (!Z_BlockInZone (block))
    {
	printf ("  %-4s @ %p  outside the zone -- not dereferenced\n",
		label, (const void *)block);
	return;
    }

    printf ("  %-4s @ %p  size=%-7d tag=%d id=0x%06x user=%p\n",
	    label, (const void *)block, block->size, block->tag,
	    (unsigned)block->id, (const void *)block->user);
    printf ("         next=%p prev=%p  data=%p..%p  seq=%u caller=%p\n",
	    (const void *)block->next, (const void *)block->prev,
	    (const void *)((const byte *)block + sizeof(memblock_t)),
	    (const void *)((const byte *)block + block->size),
	    block->seq, block->caller);

    // The raw header matters as much as the decoded one: it shows which
    // fields survived. A linear overrun smears every word from the top; a
    // stray write leaves all but one intact.
    {
	const unsigned *w = (const unsigned *)block;
	unsigned i;

	printf ("         raw ");
	for (i = 0 ; i < sizeof(memblock_t) / sizeof(unsigned) ; i++)
	    printf ("%08x ", w[i]);
	printf ("\n");
    }
}

int Z_ValidateHeap (const char *when)
{
    memblock_t*		block;
    memblock_t*		prev;
    const char*		reason = NULL;
    int			count = 0;

    // One report is the whole point. After that this is a no-op so it can sit
    // in a per-frame path without flooding the console or slowing the game
    // down while we read the first one.
    if (zone_reported)
	return 0;

    if (mainzone == NULL)
	return 1;

    prev = &mainzone->blocklist;
    block = mainzone->blocklist.next;

    while (block != &mainzone->blocklist)
    {
	if (!Z_BlockInZone (block))
	{
	    reason = "next does not point at a block inside the zone";
	    break;
	}

	if (block->prev != prev)
	{
	    reason = "back link does not point at the previous block";
	    break;
	}

	// Z_Malloc(0) is legal and produces a block of exactly the header
	// size, so the predicate is < rather than <=. A false positive here
	// would latch zone_reported and silence the validator for the rest
	// of the run on a perfectly healthy heap.
	if (block->size < (int)sizeof(memblock_t)
	 || (const byte *)block + block->size > zone_hi)
	{
	    reason = "size is impossible";
	    break;
	}

	if (block->tag < PU_STATIC || block->tag >= PU_NUM_TAGS)
	{
	    reason = "tag is out of range";
	    break;
	}

	if (block->tag != PU_FREE && block->id != ZONEID)
	{
	    reason = "allocated block has lost its ZONEID";
	    break;
	}

	// Blocks are contiguous, except that the last one runs to the end of
	// the zone and wraps to the list head rather than to its neighbour.
	if (block->next == &mainzone->blocklist)
	{
	    if ((const byte *)block + block->size != zone_hi)
	    {
		reason = "last block does not run to the end of the zone";
		break;
	    }
	}
	else
	{
	    if ((const byte *)block + block->size != (byte *)block->next)
	    {
		reason = "block does not touch the next block";
		break;
	    }

	    // Z_Free merges only what it sees tagged PU_FREE, so a tag
	    // trampled into or out of PU_FREE leaves a list that is
	    // structurally perfect and semantically wrong. This is the one
	    // invariant that catches that.
	    if (block->tag == PU_FREE
	     && Z_BlockInZone (block->next)
	     && block->next->tag == PU_FREE)
	    {
		reason = "two consecutive free blocks";
		break;
	    }
	}

	// A corrupt next could point backwards and loop forever.
	if (++count > (1 << 20))
	{
	    reason = "block list does not terminate";
	    break;
	}

	prev = block;
	block = block->next;
    }

    // The rover is not on the walk, and Z_Malloc dereferences it twice
    // (base = rover; base->prev->tag) before it walks anything. A trampled
    // rover therefore crashes inside the allocator while a blocklist-only
    // check reports the heap intact.
    if (reason == NULL
     && mainzone->rover != &mainzone->blocklist
     && !Z_BlockInZone (mainzone->rover))
    {
	reason = "rover is not a block in the zone";
	block = mainzone->rover;
	prev = NULL;
    }

    if (reason == NULL)
	return 1;

    zone_reported = 1;

    printf ("\n===== ZONE HEAP CORRUPT (%s) =====\n", when);
    printf ("  zone @ %p size=%d  intact blocks=%d  allocations so far=%u\n",
	    (void *)mainzone, mainzone->size, count, zone_alloc_seq);
    printf ("  reason: %s\n", reason);
    Z_DumpBlock ("bad", block);
    Z_DumpBlock ("prev", prev);
    printf ("  Reading this: if 'bad' is damaged from its first word onwards,\n"
	    "  'prev' overran its buffer -- decode prev's caller with\n"
	    "  addr2line. If only one field of 'bad' is wrong and the rest is\n"
	    "  intact, nothing overran anything: a stray pointer wrote those\n"
	    "  four bytes and 'prev' is innocent.\n");
    printf ("===== end =====\n\n");
    fflush (stdout);

    return 0;
}

#else

int Z_ValidateHeap (const char *when)
{
    (void) when;
    return 1;
}

#endif // ZONE_DEBUG




//
// Z_ChangeTag
//
void Z_ChangeTag2(void *ptr, int tag, char *file, int line)
{
    memblock_t*	block;
	
    block = (memblock_t *) ((byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
        I_Error("%s:%i: Z_ChangeTag: block without a ZONEID!",
                file, line);

    if (tag >= PU_PURGELEVEL && block->user == NULL)
        I_Error("%s:%i: Z_ChangeTag: an owner is required "
                "for purgable blocks", file, line);

    block->tag = tag;
}

void Z_ChangeUser(void *ptr, void **user)
{
    memblock_t*	block;

    block = (memblock_t *) ((byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
    {
        I_Error("Z_ChangeUser: Tried to change user for invalid block!");
    }

    block->user = user;
    *user = ptr;
}



//
// Z_FreeMemory
//
int Z_FreeMemory (void)
{
    memblock_t*		block;
    int			free;
	
    free = 0;
    
    for (block = mainzone->blocklist.next ;
         block != &mainzone->blocklist;
         block = block->next)
    {
        if (block->tag == PU_FREE || block->tag >= PU_PURGELEVEL)
            free += block->size;
    }

    return free;
}

unsigned int Z_ZoneSize(void)
{
    return mainzone->size;
}

