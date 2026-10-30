/* Copyright (C) 2002-2003 Jean-Marc Valin
   Copyright (C) 2007-2009 Xiph.Org Foundation */
/**
   @file stack_alloc.h
   @brief Temporary memory allocation on stack
*/
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef STACK_ALLOC_H
#define STACK_ALLOC_H

#include "opus_types.h"
#include "opus_defines.h"

#include <stddef.h>

/* maiopus: no alloca, so no stack probes and no dynamic frames. Scratch is a
   bump arena in decoder or encoder state memory. A function that allocates
   scratch receives the arena's free pointer as its `char *scratch` parameter
   and passes its current value to the callees that allocate; ALLOC carves
   from that local copy, so the space returns to the caller when the function
   returns. Allocations are 16-byte aligned, as alloca's were; a zero size
   (ALLOC_NONE) takes nothing. The arena size is a static bound: the sum of
   the largest allocations along the deepest call path. */

/**
 * @def VARDECL(var)
 *
 * Declare variable on stack
 *
 * @param var Variable to declare
 */

/**
 * @def ALLOC(var, size, type)
 *
 * Allocate 'size' elements of 'type' from the scratch arena
 *
 * @param var  Name of variable to allocate
 * @param size Number of elements
 * @param type Type of element
 */


#define VARDECL(type, var) type *var

#define SCRATCH_BYTES(size, type) ((sizeof(type)*(size_t)(size)+15)&~(size_t)15)
#define ALLOC(var, size, type) ((var) = (type*)scratch, scratch += SCRATCH_BYTES(size, type))

#define SAVE_STACK
#define RESTORE_STACK
#define ALLOC_STACK
#define ALLOC_NONE 0


static OPUS_INLINE int _opus_false(void) {return 0;}
#define OPUS_CHECK_ARRAY(ptr, len) _opus_false()
#define OPUS_CHECK_VALUE(value) _opus_false()
#define OPUS_PRINT_INT(value) do{}while(0)
#define OPUS_FPRINTF (void)


#endif /* STACK_ALLOC_H */
