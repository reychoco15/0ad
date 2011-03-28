/* Copyright (c) 2010 Wildfire Games
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * memory allocator helper routines.
 */

#ifndef INCLUDED_MEM_UTIL
#define INCLUDED_MEM_UTIL

LIB_API bool mem_IsPageMultiple(uintptr_t x);

LIB_API size_t mem_RoundUpToPage(size_t size);
LIB_API size_t mem_RoundUpToAlignment(size_t size);


// very thin wrapper on top of sys/mman.h that makes the intent more obvious
// (its commit/decommit semantics are difficult to tell apart)
LIB_API LibError mem_Reserve(size_t size, u8** pp);
LIB_API LibError mem_Release(u8* p, size_t size);
LIB_API LibError mem_Commit(u8* p, size_t size, int prot);
LIB_API LibError mem_Decommit(u8* p, size_t size);
LIB_API LibError mem_Protect(u8* p, size_t size, int prot);


// "freelist" is a pointer to the first unused element or a sentinel.
// their memory holds a pointer to the previous element in the freelist
// (or its own address in the case of sentinels to avoid branches)
//
// rationale for the function-based interface: a class encapsulating the
// freelist pointer would force each header to include mem_util.h,
// whereas this approach only requires a void* pointer and calling
// mem_freelist_Init from the implementation.
//
// these functions are inlined because allocation is sometimes time-critical.

// @return the address of a sentinel element, suitable for initializing
// a freelist pointer. subsequent mem_freelist_Detach will return 0.
LIB_API void* mem_freelist_Sentinel();

static inline void mem_freelist_AddToFront(void*& freelist, void* el)
{
#ifndef NDEBUG
	debug_assert(freelist != 0);
	debug_assert(el != 0);
#endif

	memcpy(el, &freelist, sizeof(void*));
	freelist = el;
}

// @return 0 if the freelist is empty, else a pointer that had
// previously been passed to mem_freelist_AddToFront.
static inline void* mem_freelist_Detach(void*& freelist)
{
#ifndef NDEBUG
	debug_assert(freelist != 0);
#endif

	void* prev_el;
	memcpy(&prev_el, freelist, sizeof(void*));
	void* el = (freelist == prev_el)? 0 : freelist;
	freelist = prev_el;
	return el;
}

#endif	// #ifndef INCLUDED_MEM_UTIL
