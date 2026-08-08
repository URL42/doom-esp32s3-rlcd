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
//      Zone Memory Allocation, perhaps NeXT ObjectiveC inspired.
//	Remark: this was the only stuff that, according
//	 to John Carmack, might have been useful for
//	 Quake.
//



#ifndef __Z_ZONE__
#define __Z_ZONE__

#include <stdio.h>

//
// ZONE MEMORY
// PU - purge tags.

enum
{
    PU_STATIC = 1,                  // static entire execution time
    PU_SOUND,                       // static while playing
    PU_MUSIC,                       // static while playing
    PU_FREE,                        // a free block
    PU_LEVEL,                       // static until level exited
    PU_LEVSPEC,                     // a special thinker in a level
    
    // Tags >= PU_PURGELEVEL are purgable whenever needed.

    PU_PURGELEVEL,
    PU_CACHE,

    // Total number of different tag types

    PU_NUM_TAGS
};
        

//
// ZONE_DEBUG
//
// Compiles in per-block allocation provenance (an allocation sequence number
// and the return address of whoever called Z_Malloc) plus Z_ValidateHeap, a
// walk of the block list that reports rather than faults.
//
// The point is diagnosis, not safety. When a memblock header gets trampled,
// the vanilla failure mode is a LoadProhibited somewhere inside Z_FreeTags --
// which tells you the heap is broken but nothing about who broke it. With
// this on, the walk stops at the first inconsistency and dumps both the
// damaged header and the header of the block physically preceding it. If the
// damaged header is trashed from offset 0 onwards, the block in front of it
// overran its buffer, and its recorded caller names the culprit. If only a
// single field is wrong, nothing overran anything -- a stray pointer wrote
// four bytes, which is a different hunt entirely.
//
// Costs 8 bytes per block (24-byte header -> 32, which keeps the 8-byte
// alignment the header already had) and a list walk wherever it is called.
// Set to 0 for release builds.
//
#define ZONE_DEBUG 1

void	Z_Init (void);
void*	Z_Malloc (int size, int tag, void *ptr);
void    Z_Free (void *ptr);
void    Z_FreeTags (int lowtag, int hightag);
void    Z_DumpHeap (int lowtag, int hightag);
void    Z_FileDumpHeap (FILE *f);
void    Z_CheckHeap (void);
void    Z_ChangeTag2 (void *ptr, int tag, char *file, int line);
void    Z_ChangeUser(void *ptr, void **user);
int     Z_FreeMemory (void);
unsigned int Z_ZoneSize(void);

// Walk the block list without trusting it. Returns 1 if the heap is intact.
// On the first failure it prints a full report tagged with 'when' and then
// stays quiet: subsequent calls return 0 immediately without re-walking, so
// dropping this in a per-frame path costs nothing once the damage is found
// and does not turn the console into a firehose. Compiled to a constant 1
// when ZONE_DEBUG is 0.
int     Z_ValidateHeap (const char *when);

//
// This is used to get the local FILE:LINE info from CPP
// prior to really call the function in question.
//
#define Z_ChangeTag(p,t)                                       \
    Z_ChangeTag2((p), (t), __FILE__, __LINE__)


#endif
