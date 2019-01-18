/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/Common.h"
#include "renderer/tr_local.h"

#include "renderer/VertexCache.h"

static const int	FRAME_MEMORY_BYTES = 0x200000;
static const int	EXPAND_HEADERS = 1024;

idCVar idVertexCache::r_showVertexCache( "r_showVertexCache", "0", CVAR_INTEGER|CVAR_RENDERER, "" );
idCVar idVertexCache::r_vertexBufferMegs( "r_vertexBufferMegs", "64", CVAR_INTEGER|CVAR_RENDERER, "" );

idVertexCache		vertexCache;

/*
==============
R_ListVertexCache_f
==============
*/
static void R_ListVertexCache_f( const idCmdArgs &args ) {
	vertexCache.List();
}

/*
==============
idVertexCache::ActuallyFree
==============
*/
void idVertexCache::ActuallyFree( vertCache_t *block ) {
	if (!block) {
		common->Error( "idVertexCache Free: NULL pointer" );
	}

	if ( block->user ) {
		// let the owner know we have purged it
		*block->user = NULL;
		block->user = NULL;
	}

	// temp blocks are in a shared space that won't be freed
	if ( block->tag != TAG_TEMP ) {
		staticAllocTotal -= block->size;
		staticCountTotal--;
	}
	block->tag = TAG_FREE;		// mark as free

	// unlink stick it back on the free list
	block->next->prev = block->prev;
	block->prev->next = block->next;

  if (block->indexBuffer)
  {
    // stick it on the front of the free list so it will be reused immediately
    block->next = freeStaticIndexHeaders.next;
    block->prev = &freeStaticIndexHeaders;
  }
	else
  {
    // stick it on the front of the free list so it will be reused immediately
    block->next = freeStaticHeaders.next;
    block->prev = &freeStaticHeaders;
  }

	block->next->prev = block;
	block->prev->next = block;
}

/*
==============
idVertexCache::Position

this will be a real pointer with virtual memory,
but it will be an int offset cast to a pointer with
ARB_vertex_buffer_object

The ARB_vertex_buffer_object will be bound
==============
*/
void *idVertexCache::Position( vertCache_t *buffer ) {
	if ( !buffer || buffer->tag == TAG_FREE ) {
		common->FatalError( "idVertexCache::Position: bad vertCache_t" );
	}

	// the ARB vertex object just uses an offset
		if ( r_showVertexCache.GetInteger() == 2 ) {
			if ( buffer->tag == TAG_TEMP ) {
				common->Printf( "GL_ARRAY_BUFFER_ARB = %i + %zd (%i bytes)\n", buffer->vbo, buffer->offset, buffer->size );
			} else {
				common->Printf( "GL_ARRAY_BUFFER_ARB = %i (%i bytes)\n", buffer->vbo, buffer->size );
			}
		}
		if ( buffer->indexBuffer ) {
			qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, buffer->vbo );
		} else {
			qglBindBuffer( GL_ARRAY_BUFFER, buffer->vbo );
		}
		return (void *)buffer->offset;
}

//================================================================================

/*
===========
idVertexCache::Init
===========
*/
void idVertexCache::Init() {
	cmdSystem->AddCommand( "listVertexCache", R_ListVertexCache_f, CMD_FL_RENDERER, "lists vertex cache" );

	if ( r_vertexBufferMegs.GetInteger() < 8 ) {
		r_vertexBufferMegs.SetInteger( 8 );
	}

	// initialize the cache memory blocks
	freeStaticHeaders.next = freeStaticHeaders.prev = &freeStaticHeaders;
	staticHeaders.next = staticHeaders.prev = &staticHeaders;
  freeStaticIndexHeaders.next = freeStaticIndexHeaders.prev = &freeStaticIndexHeaders;
  staticIndexHeaders.next = staticIndexHeaders.prev = &staticIndexHeaders;

	freeDynamicHeaders.next = freeDynamicHeaders.prev = &freeDynamicHeaders;
	dynamicHeaders.next = dynamicHeaders.prev = &dynamicHeaders;
	freeDynamicIndexHeaders.next = freeDynamicIndexHeaders.prev = &freeDynamicIndexHeaders;
	dynamicIndexHeaders.next = dynamicIndexHeaders.prev = &dynamicIndexHeaders;

	deferredFreeList.next = deferredFreeList.prev = &deferredFreeList;

	// set up the dynamic frame memory
	frameBytes = FRAME_MEMORY_BYTES;
	staticAllocTotal = 0;

	byte	*junk = (byte *)Mem_Alloc( frameBytes );
	for ( int i = 0 ; i < NUM_VERTEX_FRAMES ; i++ ) {
		allocatingTempBuffer = true;	// force the alloc to use GL_STREAM_DRAW_ARB
		Alloc( junk, frameBytes, &tempBuffers[i], false );
		Alloc( junk, frameBytes, &tempIndexBuffers[i], true );
		allocatingTempBuffer = false;
		tempBuffers[i]->tag = TAG_FIXED;
		tempIndexBuffers[i]->tag = TAG_FIXED;
		// unlink these from the static list, so they won't ever get purged
		tempBuffers[i]->next->prev = tempBuffers[i]->prev;
		tempBuffers[i]->prev->next = tempBuffers[i]->next;
		tempIndexBuffers[i]->next->prev = tempIndexBuffers[i]->prev;
		tempIndexBuffers[i]->prev->next = tempIndexBuffers[i]->next;
	}
	Mem_Free( junk );

	EndFrame();
}

/*
===========
idVertexCache::PurgeAll

Used when toggling vertex programs on or off, because
the cached data isn't valid
===========
*/
void idVertexCache::PurgeAll() {
	while( staticHeaders.next != &staticHeaders ) {
		ActuallyFree( staticHeaders.next );
	}
  while( staticIndexHeaders.next != &staticIndexHeaders ) {
    ActuallyFree( staticIndexHeaders.next );
  }
}

/*
===========
idVertexCache::Shutdown
===========
*/
void idVertexCache::Shutdown() {
//	PurgeAll();	// !@#: also purge the temp buffers

	headerAllocator.Shutdown();
}

/*
===========
idVertexCache::Alloc
===========
*/
void idVertexCache::Alloc( void *data, int size, vertCache_t **buffer, bool indexBuffer ) {
	vertCache_t	*block;

	if ( size <= 0 ) {
		common->Error( "idVertexCache::Alloc: size = %i\n", size );
	}

	// if we can't find anything, it will be NULL
	*buffer = NULL;


	if (indexBuffer)
  {
// if we don't have any remaining unused headers, allocate some more
    if ( freeStaticIndexHeaders.next == &freeStaticIndexHeaders ) {

      for ( int i = 0; i < EXPAND_HEADERS; i++ ) {
        block = headerAllocator.Alloc();
        block->next = freeStaticIndexHeaders.next;
        block->prev = &freeStaticIndexHeaders;
        block->next->prev = block;
        block->prev->next = block;

        qglGenBuffers( 1, & block->vbo );
      }
    }
  }
	else
  {
    // if we don't have any remaining unused headers, allocate some more
    if ( freeStaticHeaders.next == &freeStaticHeaders ) {

      for ( int i = 0; i < EXPAND_HEADERS; i++ ) {
        block = headerAllocator.Alloc();
        block->next = freeStaticHeaders.next;
        block->prev = &freeStaticHeaders;
        block->next->prev = block;
        block->prev->next = block;

        qglGenBuffers( 1, & block->vbo );
      }
    }

  }

	if (indexBuffer)
  {
    // move it from the freeStaticIndexHeaders list to the staticIndexHeaders list
    block = freeStaticIndexHeaders.next;
    block->next->prev = block->prev;
    block->prev->next = block->next;
    block->next = staticIndexHeaders.next;
    block->prev = &staticIndexHeaders;
    block->next->prev = block;
    block->prev->next = block;
  }
	else
  {
    // move it from the freeStaticHeaders list to the staticHeaders list
    block = freeStaticHeaders.next;
    block->next->prev = block->prev;
    block->prev->next = block->next;
    block->next = staticHeaders.next;
    block->prev = &staticHeaders;
    block->next->prev = block;
    block->prev->next = block;
  }

	block->size = size;
	block->offset = 0;
	block->tag = TAG_USED;

	// save data for debugging
	if ( indexBuffer )
	{
		staticAllocThisFrame_Index += block->size;
		staticCountThisFrame_Index++;
	}
	else {
		staticAllocThisFrame += block->size;
		staticCountThisFrame++;
	}
	staticCountTotal++;
	staticAllocTotal += block->size;

	// this will be set to zero when it is purged
	block->user = buffer;
	*buffer = block;

	// allocation doesn't imply used-for-drawing, because at level
	// load time lots of things may be created, but they aren't
	// referenced by the GPU yet, and can be purged if needed.
	block->frameUsed = currentFrame - NUM_VERTEX_FRAMES;

	block->indexBuffer = indexBuffer;

	// copy the data
		if ( indexBuffer ) {
			qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, block->vbo );
			if ( allocatingTempBuffer ) {
				qglBufferData( GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)size, data, GL_STREAM_DRAW );
			} else {
				qglBufferData( GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)size, data, GL_STATIC_DRAW );
			}
		} else {
			qglBindBuffer( GL_ARRAY_BUFFER, block->vbo );
			if ( allocatingTempBuffer ) {
				qglBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)size, data, GL_STREAM_DRAW );
			} else {
				qglBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)size, data, GL_STATIC_DRAW );
			}
		}
}

/*
===========
idVertexCache::Touch
===========
*/
void idVertexCache::Touch( vertCache_t *block ) {
	if ( !block ) {
		common->Error( "idVertexCache Touch: NULL pointer" );
	}

	if ( block->tag == TAG_FREE ) {
		common->FatalError( "idVertexCache Touch: freed pointer" );
	}
	if ( block->tag == TAG_TEMP ) {
		common->FatalError( "idVertexCache Touch: temporary pointer" );
	}

	block->frameUsed = currentFrame;

	// move to the head of the LRU list
	block->next->prev = block->prev;
	block->prev->next = block->next;

	if (block->indexBuffer)
  {
    block->next = staticIndexHeaders.next;
    block->prev = &staticIndexHeaders;
    staticIndexHeaders.next->prev = block;
    staticIndexHeaders.next = block;
  }
	else
  {
    block->next = staticHeaders.next;
    block->prev = &staticHeaders;
    staticHeaders.next->prev = block;
    staticHeaders.next = block;
  }
}

/*
===========
idVertexCache::Free
===========
*/
void idVertexCache::Free( vertCache_t *block ) {
	if (!block) {
		return;
	}

	if ( block->tag == TAG_FREE ) {
		common->FatalError( "idVertexCache Free: freed pointer" );
	}
	if ( block->tag == TAG_TEMP ) {
		common->FatalError( "idVertexCache Free: temporary pointer" );
	}

	// this block still can't be purged until the frame count has expired,
	// but it won't need to clear a user pointer when it is
	block->user = NULL;

	block->next->prev = block->prev;
	block->prev->next = block->next;

	block->next = deferredFreeList.next;
	block->prev = &deferredFreeList;
	deferredFreeList.next->prev = block;
	deferredFreeList.next = block;
}

/*
===========
idVertexCache::AllocFrameTemp

A frame temp allocation must never be allowed to fail due to overflow.
We can't simply sync with the GPU and overwrite what we have, because
there may still be future references to dynamically created surfaces.
===========
*/
vertCache_t	*idVertexCache::AllocFrameTemp( void *data, int size, bool indexBuffer ) {
	vertCache_t	*block;

	if ( size <= 0 ) {
		common->Error( "idVertexCache::AllocFrameTemp: size = %i\n", size );
	}

	if (indexBuffer)
	{
		if (dynamicAllocThisFrame_Index + size > frameBytes) {
			// if we don't have enough room in the temp block, allocate a static block,
			// but immediately free it so it will get freed at the next frame
			tempOverflow = true;
			Alloc(data, size, &block, indexBuffer);
			Free(block);
			return block;
		}
	}
	else
	{
		if (dynamicAllocThisFrame + size > frameBytes) {
			// if we don't have enough room in the temp block, allocate a static block,
			// but immediately free it so it will get freed at the next frame
			tempOverflow = true;
			Alloc(data, size, &block, indexBuffer);
			Free(block);
			return block;
		}
	}

	// this data is just going on the shared dynamic list

	if (indexBuffer)
	{
		// if we don't have any remaining unused headers, allocate some more
		if ( freeDynamicIndexHeaders.next == &freeDynamicIndexHeaders ) {

			for ( int i = 0; i < EXPAND_HEADERS; i++ ) {
				block = headerAllocator.Alloc();
				block->next = freeDynamicIndexHeaders.next;
				block->prev = &freeDynamicIndexHeaders;
				block->next->prev = block;
				block->prev->next = block;
			}
		}
	}
	else
	{
		// if we don't have any remaining unused headers, allocate some more
		if ( freeDynamicHeaders.next == &freeDynamicHeaders ) {

			for ( int i = 0; i < EXPAND_HEADERS; i++ ) {
				block = headerAllocator.Alloc();
				block->next = freeDynamicHeaders.next;
				block->prev = &freeDynamicHeaders;
				block->next->prev = block;
				block->prev->next = block;
			}
		}
	}

	if (indexBuffer)
	{
		// move it from the freeIndexDynamicHeaders list to the dynamicIndexHeaders list
		block = freeDynamicIndexHeaders.next;
		block->next->prev = block->prev;
		block->prev->next = block->next;
		block->next = dynamicIndexHeaders.next;
		block->prev = &dynamicIndexHeaders;
		block->next->prev = block;
		block->prev->next = block;

	}
	else
	{
		// move it from the freeDynamicHeaders list to the dynamicHeaders list
		block = freeDynamicHeaders.next;
		block->next->prev = block->prev;
		block->prev->next = block->next;
		block->next = dynamicHeaders.next;
		block->prev = &dynamicHeaders;
		block->next->prev = block;
		block->prev->next = block;
	}

	block->size = size;
	block->tag = TAG_TEMP;
  block->indexBuffer = indexBuffer;
  if (indexBuffer)
	{
		block->offset = dynamicAllocThisFrame_Index;
		dynamicAllocThisFrame_Index += block->size;
		dynamicCountThisFrame_Index++;
	}
  else {
		block->offset = dynamicAllocThisFrame;
		dynamicAllocThisFrame += block->size;
		dynamicCountThisFrame++;

	}
	block->user = NULL;
	block->frameUsed = 0;

	// copy the data

  if ( indexBuffer ) {
		block->vbo = tempIndexBuffers[listNum]->vbo;
		qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, block->vbo );
    qglBufferSubData( GL_ELEMENT_ARRAY_BUFFER, block->offset, (GLsizeiptr)size, data );
  } else {
		block->vbo = tempBuffers[listNum]->vbo;
		qglBindBuffer( GL_ARRAY_BUFFER, block->vbo );
    qglBufferSubData( GL_ARRAY_BUFFER, block->offset, (GLsizeiptr)size, data );
  }

	return block;
}

/*
===========
idVertexCache::EndFrame
===========
*/
void idVertexCache::EndFrame() {
	// display debug information
	if ( r_showVertexCache.GetBool() ) {
		int	staticUseCount = 0;
		int staticUseSize = 0;

		for ( vertCache_t *block = staticHeaders.next ; block != &staticHeaders ; block = block->next ) {
			if ( block->frameUsed == currentFrame ) {
				staticUseCount++;
				staticUseSize += block->size;
			}
		}

    for ( vertCache_t *block = staticIndexHeaders.next ; block != &staticIndexHeaders ; block = block->next ) {
      if ( block->frameUsed == currentFrame ) {
        staticUseCount++;
        staticUseSize += block->size;
      }
    }

		const char *frameOverflow = tempOverflow ? "(OVERFLOW)" : "";

		common->Printf( "vertex dynamic:%i=%ik%s, static alloc:%i=%ik used:%i=%ik total:%i=%ik\n",
			dynamicCountThisFrame + dynamicCountThisFrame_Index, (dynamicAllocThisFrame + dynamicAllocThisFrame_Index)/1024, frameOverflow,
			staticCountThisFrame + staticCountThisFrame_Index, (staticAllocThisFrame + staticAllocThisFrame_Index)/1024,
			staticUseCount, staticUseSize/1024,
			staticCountTotal, staticAllocTotal/1024 );
	}

  if ( staticAllocTotal > r_vertexBufferMegs.GetInteger() * 1024 * 1024 ) {
    static bool bOnce = true;
    if (bOnce) {
      common->Printf("VBO size exceeds %dMB. Consider updating r_vertexBufferMegs.\n", r_vertexBufferMegs.GetInteger());
      bOnce = false;
    }
  }

#if 0
	// if our total static count is above our working memory limit, start purging things
	while ( staticAllocTotal > r_vertexBufferMegs.GetInteger() * 1024 * 1024 ) {
		// free the least recently used

	}
#endif

	currentFrame = tr.frameCount;
	listNum = currentFrame % NUM_VERTEX_FRAMES;
	staticAllocThisFrame = 0;
	staticCountThisFrame = 0;
	staticAllocThisFrame_Index  = 0;
	staticCountThisFrame_Index  = 0;
	dynamicAllocThisFrame_Index = 0;
	dynamicCountThisFrame_Index = 0;
	dynamicAllocThisFrame = 0;
	dynamicCountThisFrame = 0;
	tempOverflow = false;

	// free all the deferred free headers
	while( deferredFreeList.next != &deferredFreeList ) {
		ActuallyFree( deferredFreeList.next );
	}

	// free all the frame temp headers
	vertCache_t	*block = dynamicHeaders.next;
	if ( block != &dynamicHeaders ) {
		block->prev = &freeDynamicHeaders;
		dynamicHeaders.prev->next = freeDynamicHeaders.next;
		freeDynamicHeaders.next->prev = dynamicHeaders.prev;
		freeDynamicHeaders.next = block;

		dynamicHeaders.next = dynamicHeaders.prev = &dynamicHeaders;
	}

	block = dynamicIndexHeaders.next;
	if ( block != &dynamicIndexHeaders ) {
		block->prev = &freeDynamicIndexHeaders;
		dynamicIndexHeaders.prev->next = freeDynamicIndexHeaders.next;
		freeDynamicIndexHeaders.next->prev = dynamicIndexHeaders.prev;
		freeDynamicIndexHeaders.next = block;

		dynamicIndexHeaders.next = dynamicIndexHeaders.prev = &dynamicIndexHeaders;
	}
}

/*
=============
idVertexCache::List
=============
*/
void idVertexCache::List( void ) {
	int	numActive = 0;
	int frameStatic = 0;
	int	totalStatic = 0;

	vertCache_t *block;
	for ( block = staticHeaders.next ; block != &staticHeaders ; block = block->next) {
		numActive++;

		totalStatic += block->size;
		if ( block->frameUsed == currentFrame ) {
			frameStatic += block->size;
		}
	}


  for ( block = staticIndexHeaders.next ; block != &staticIndexHeaders; block = block->next) {
    numActive++;

    totalStatic += block->size;
    if ( block->frameUsed == currentFrame ) {
      frameStatic += block->size;
    }
  }

	int	numFreeStaticHeaders = 0;
	for ( block = freeStaticHeaders.next ; block != &freeStaticHeaders ; block = block->next ) {
		numFreeStaticHeaders++;
	}


  int	numFreeStaticIndexHeaders = 0;
  for ( block = freeStaticIndexHeaders.next ; block != &freeStaticIndexHeaders; block = block->next ) {
    numFreeStaticIndexHeaders++;
  }

	int	numFreeDynamicHeaders = 0;
	for ( block = freeDynamicHeaders.next ; block != &freeDynamicHeaders ; block = block->next ) {
		numFreeDynamicHeaders++;
	}

	int	numFreeDynamicIndexHeaders = 0;
	for ( block = freeDynamicIndexHeaders.next ; block != &freeDynamicIndexHeaders ; block = block->next ) {
		numFreeDynamicIndexHeaders++;
	}

	common->Printf( "%i megs working set\n", r_vertexBufferMegs.GetInteger() );
	common->Printf( "%i dynamic temp buffers of %ik\n", NUM_VERTEX_FRAMES, frameBytes / 1024 );
	common->Printf( "%5i active static headers\n", numActive );
	common->Printf( "%5i free static headers\n", numFreeStaticHeaders + numFreeStaticIndexHeaders);
	common->Printf( "%5i free dynamic headers\n", numFreeDynamicHeaders + numFreeDynamicIndexHeaders );
}

