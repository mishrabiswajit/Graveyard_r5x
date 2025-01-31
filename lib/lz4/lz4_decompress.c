/*
 * LZ4 - Fast LZ compression algorithm
 * Copyright (C) 2011 - 2016, Yann Collet.
 * BSD 2 - Clause License (http://www.opensource.org/licenses/bsd - license.php)
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *	* Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *	* Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * You can contact the author at :
 *	- LZ4 homepage : http://www.lz4.org
 *	- LZ4 source repository : https://github.com/lz4/lz4
 *
 *	Changed for kernel usage by:
 *	Sven Schmidt <4sschmid@informatik.uni-hamburg.de>
 */

/*-************************************
 *	Dependencies
 **************************************/
#include <linux/lz4.h>
#include "lz4defs.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/unaligned.h>

/*-*****************************
 *	Decompression functions
 *******************************/

#define DEBUGLOG(l, ...) {}	/* disabled */

#ifndef assert
#define assert(condition) ((void)0)
#endif

#ifndef LZ4_FAST_DEC_LOOP
#if defined(__i386__) || defined(__x86_64__)
#define LZ4_FAST_DEC_LOOP 1
#elif defined(__aarch64__)
     /* On aarch64, we disable this optimization for clang because on certain
      * mobile chipsets and clang, it reduces performance. For more information
      * refer to https://github.com/lz4/lz4/pull/707. */
#define LZ4_FAST_DEC_LOOP 1
#else
#define LZ4_FAST_DEC_LOOP 0
#endif
#endif

#if LZ4_FAST_DEC_LOOP
#define FASTLOOP_SAFE_DISTANCE 64
FORCE_O2_INLINE_GCC_PPC64LE void
LZ4_memcpy_using_offset_base(BYTE * dstPtr, const BYTE * srcPtr, BYTE * dstEnd,
			     const size_t offset)
{
	if (offset < 8) {
		dstPtr[0] = srcPtr[0];

		dstPtr[1] = srcPtr[1];
		dstPtr[2] = srcPtr[2];
		dstPtr[3] = srcPtr[3];
		srcPtr += inc32table[offset];
		memcpy(dstPtr + 4, srcPtr, 4);
		srcPtr -= dec64table[offset];
		dstPtr += 8;
	} else {
		memcpy(dstPtr, srcPtr, 8);
		dstPtr += 8;
		srcPtr += 8;
	}

	LZ4_wildCopy8(dstPtr, srcPtr, dstEnd);
}

/* customized variant of memcpy, which can overwrite up to 32 bytes beyond dstEnd
 * this version copies two times 16 bytes (instead of one time 32 bytes)
 * because it must be compatible with offsets >= 16. */
FORCE_O2_INLINE_GCC_PPC64LE void
LZ4_wildCopy32(void *dstPtr, const void *srcPtr, void *dstEnd)
{
	BYTE *d = (BYTE *) dstPtr;
	const BYTE *s = (const BYTE *)srcPtr;
	BYTE *const e = (BYTE *) dstEnd;

	do {
		memcpy(d, s, 16);
		memcpy(d + 16, s + 16, 16);
		d += 32;
		s += 32;
	} while (d < e);
}

FORCE_O2_INLINE_GCC_PPC64LE void
LZ4_memcpy_using_offset(BYTE *dstPtr, const BYTE *srcPtr, BYTE *dstEnd,
			const size_t offset)
{
	BYTE v[8];
	switch (offset) {

	case 1:
		memset(v, *srcPtr, 8);
		goto copy_loop;
	case 2:
		memcpy(v, srcPtr, 2);
		memcpy(&v[2], srcPtr, 2);
		memcpy(&v[4], &v[0], 4);
		goto copy_loop;
	case 4:
		memcpy(v, srcPtr, 4);
		memcpy(&v[4], srcPtr, 4);
		goto copy_loop;
	default:
		LZ4_memcpy_using_offset_base(dstPtr, srcPtr, dstEnd, offset);
		return;
	}

      copy_loop:
	memcpy(dstPtr, v, 8);
	dstPtr += 8;
	while (dstPtr < dstEnd) {
		memcpy(dstPtr, v, 8);
		dstPtr += 8;
	}
}
#endif

/*
 * LZ4_decompress_generic() :
 * This generic decompression function covers all use cases.
 * It shall be instantiated several times, using different sets of directives.
 * Note that it is important for performance that this function really get inlined,
 * in order to remove useless branches during compilation optimization.
 */
static FORCE_INLINE int LZ4_decompress_generic(
	 const char * const src,
	 char * const dst,
	 int srcSize,
		/*
		 * If endOnInput == endOnInputSize,
		 * this value is `dstCapacity`
		 */
	 int outputSize,
	 /* endOnOutputSize, endOnInputSize */
	 endCondition_directive endOnInput,
	 /* full, partial */
	 earlyEnd_directive partialDecoding,
	 /* noDict, withPrefix64k */
	 dict_directive dict,
	 /* always <= dst, == dst when no prefix */
	 const BYTE * const lowPrefix,
	 /* only if dict == usingExtDict */
	 const BYTE * const dictStart,
	 /* note : = 0 if noDict */
	 const size_t dictSize
	 )
{
	const BYTE *ip = (const BYTE *)src;
	const BYTE *const iend = ip + srcSize;

	BYTE *op = (BYTE *) dst;
	BYTE *const oend = op + outputSize;
	BYTE *cpy;

	const int safeDecode = (endOnInput == endOnInputSize);
	const int checkOffset = ((safeDecode) && (dictSize < (int)(64 * KB)));

	/* Set up the "end" pointers for the shortcut. */
	const BYTE *const shortiend = iend -
	    (endOnInput ? 14 : 8) /*maxLL*/ - 2 /*offset*/;
	const BYTE *const shortoend = oend -
	    (endOnInput ? 14 : 8) /*maxLL*/ - 18 /*maxML*/;

	const BYTE *match;
	size_t offset;
	unsigned int token;
	size_t length;

	DEBUGLOG(5, "%s (srcSize:%i, dstSize:%i)", __func__,
		 srcSize, outputSize);

	/* Special cases */
	assert(lowPrefix <= op);
	assert(src != NULL);

	/* Empty output buffer */
	if ((endOnInput) && (unlikely(outputSize == 0)))
		return ((srcSize == 1) && (*ip == 0)) ? 0 : -1;

	if ((!endOnInput) && (unlikely(outputSize == 0)))
		return (*ip == 0 ? 1 : -1);

	if ((endOnInput) && unlikely(srcSize == 0))
		return -1;

#if LZ4_FAST_DEC_LOOP
	if ((oend - op) < FASTLOOP_SAFE_DISTANCE) {
		DEBUGLOG(6, "skip fast decode loop");
		goto safe_decode;
	}

	/* Fast loop : decode sequences as long as output < iend-FASTLOOP_SAFE_DISTANCE */
	while (1) {
		/* Main fastloop assertion: We can always wildcopy FASTLOOP_SAFE_DISTANCE */
		assert(oend - op >= FASTLOOP_SAFE_DISTANCE);
		if (endOnInput) {
			assert(ip < iend);
		}
		token = *ip++;
		length = token >> ML_BITS;	/* literal length */

		assert(!endOnInput || ip <= iend);	/* ip < iend before the increment */

		/* decode literal length */
		if (length == RUN_MASK) {
			variable_length_error error = ok;
			length +=
			    read_variable_length(&ip, iend - RUN_MASK,
						 endOnInput, endOnInput,
						 &error);
			if (error == initial_error) {
				goto _output_error;
			}
			if ((safeDecode)
			    && unlikely((uptrval) (op) + length <
					(uptrval) (op))) {
				goto _output_error;
			}	/* overflow detection */
			if ((safeDecode)
			    && unlikely((uptrval) (ip) + length <
					(uptrval) (ip))) {
				goto _output_error;
			}

			/* overflow detection */
			/* copy literals */
			cpy = op + length;
			LZ4_STATIC_ASSERT(MFLIMIT >= WILDCOPYLENGTH);
			if (endOnInput) {	/* LZ4_decompress_safe() */
				if ((cpy > oend - 32)
				    || (ip + length > iend - 32)) {
					goto safe_literal_copy;
				}
				LZ4_wildCopy32(op, ip, cpy);
			} else {	/* LZ4_decompress_fast() */
				if (cpy > oend - 8) {
					goto safe_literal_copy;
				}
				LZ4_wildCopy8(op, ip, cpy);
				/* LZ4_decompress_fast() cannot copy more than 8 bytes at a time */
				/* it doesn't know input length, and only relies on end-of-block */
				/* properties */
			}
			ip += length;
			op = cpy;
		} else {
			cpy = op + length;
			if (endOnInput) {	/* LZ4_decompress_safe() */
				DEBUGLOG(7,
					 "copy %u bytes in a 16-bytes stripe",
					 (unsigned)length);
				/* We don't need to check oend */
				/* since we check it once for each loop below */
				if (ip > iend - (16 + 1)) {	/*max lit + offset + nextToken */
					goto safe_literal_copy;
				}
				/* Literals can only be 14, but hope compilers optimize */
				/*if we copy by a register size */
				memcpy(op, ip, 16);
			} else {
				/* LZ4_decompress_fast() cannot copy more than 8 bytes at a time */
				/* it doesn't know input length, and relies on end-of-block */
				/* properties */
				memcpy(op, ip, 8);
				if (length > 8) {
					memcpy(op + 8, ip + 8, 8);
				}
			}
			ip += length;
			op = cpy;
		}

		/* get offset */
		offset = LZ4_readLE16(ip);
		ip += 2;	/* end-of-block condition violated */
		match = op - offset;

		/* get matchlength */
		length = token & ML_MASK;

		if ((checkOffset) && (unlikely(match + dictSize < lowPrefix))) {
			goto _output_error;
		}
		/* Error : offset outside buffers */
		if (length == ML_MASK) {
			variable_length_error error = ok;
			length +=
			    read_variable_length(&ip, iend - LASTLITERALS + 1,
						 endOnInput, 0, &error);
			if (error != ok) {
				goto _output_error;
			}
			if ((safeDecode)
			    && unlikely((uptrval) (op) + length < (uptrval) op)) {
				goto _output_error;
			}	/* overflow detection */
			length += MINMATCH;
			if (op + length >= oend - FASTLOOP_SAFE_DISTANCE) {
				goto safe_match_copy;
			}
		} else {
			length += MINMATCH;
			if (op + length >= oend - FASTLOOP_SAFE_DISTANCE) {
				goto safe_match_copy;
			}

			/* Fastpath check: Avoids a branch in LZ4_wildCopy32 if true */
			if ((match >= lowPrefix)) {
				if (offset >= 8) {
					memcpy(op, match, 8);
					memcpy(op + 8, match + 8, 8);
					memcpy(op + 16, match + 16, 2);
					op += length;
					continue;
				}
			}
		}

		/* copy match within block */
		cpy = op + length;

		assert((op <= oend) && (oend - op >= 32));
		if (unlikely(offset < 16)) {
			LZ4_memcpy_using_offset(op, match, cpy, offset);
		} else {
			LZ4_wildCopy32(op, match, cpy);
		}

		op = cpy;	/* wildcopy correction */
	}
      safe_decode:
#endif
	/* Main Loop : decode sequences */
	while (1) {
		token = *ip++;
		length = token >> ML_BITS;

		/* ip < iend before the increment */
		assert(!endOnInput || ip <= iend);

		/*
		 * A two-stage shortcut for the most common case:
		 * 1) If the literal length is 0..14, and there is enough
		 * space, enter the shortcut and copy 16 bytes on behalf
		 * of the literals (in the fast mode, only 8 bytes can be
		 * safely copied this way).
		 * 2) Further if the match length is 4..18, copy 18 bytes
		 * in a similar manner; but we ensure that there's enough
		 * space in the output for those 18 bytes earlier, upon
		 * entering the shortcut (in other words, there is a
		 * combined check for both stages).
		 *
		 * The & in the likely() below is intentionally not && so that
		 * some compilers can produce better parallelized runtime code
		 */
		if ((endOnInput ? length != RUN_MASK : length <= 8)
		    /*
		     * strictly "less than" on input, to re-enter
		     * the loop with at least one byte
		     */
		    && likely((endOnInput ? ip < shortiend : 1) &
			      (op <= shortoend))) {
			/* Copy the literals */
			memcpy(op, ip, endOnInput ? 16 : 8);
			op += length;
			ip += length;

			/*
			 * The second stage:
			 * prepare for match copying, decode full info.
			 * If it doesn't work out, the info won't be wasted.
			 */
			length = token & ML_MASK;	/* match length */
			offset = LZ4_readLE16(ip);
			ip += 2;
			match = op - offset;
			assert(match <= op);	/* check overflow */

			/* Do not deal with overlapping matches. */
			if ((length != ML_MASK) &&
			    (offset >= 8) &&
			    (dict == withPrefix64k || match >= lowPrefix)) {
				/* Copy the match. */
				memcpy(op + 0, match + 0, 8);
				memcpy(op + 8, match + 8, 8);
				memcpy(op + 16, match + 16, 2);
				op += length + MINMATCH;
				/* Both stages worked, load the next token. */
				continue;
			}

			/*
			 * The second stage didn't work out, but the info
			 * is ready. Propel it right to the point of match
			 * copying.
			 */
			goto _copy_match;
		}

		/* decode literal length */
		if (length == RUN_MASK) {

			variable_length_error error = ok;
			length +=
			    read_variable_length(&ip, iend - RUN_MASK,
						 endOnInput, endOnInput,
						 &error);
			if (error == initial_error)
				goto _output_error;

			if ((safeDecode)
			    && unlikely((uptrval) (op) +
					length < (uptrval) (op))) {
				/* overflow detection */
				goto _output_error;
			}
			if ((safeDecode)
			    && unlikely((uptrval) (ip) +
					length < (uptrval) (ip))) {
				/* overflow detection */
				goto _output_error;
			}
		}

		/* copy literals */
		cpy = op + length;
#if LZ4_FAST_DEC_LOOP
	      safe_literal_copy:
#endif
		LZ4_STATIC_ASSERT(MFLIMIT >= WILDCOPYLENGTH);

		if (((endOnInput) && ((cpy > oend - MFLIMIT)
				      || (ip + length >
					  iend - (2 + 1 + LASTLITERALS))))
		    || ((!endOnInput) && (cpy > oend - WILDCOPYLENGTH))) {
			if (partialDecoding) {
				if (cpy > oend) {
					/*
					 * Partial decoding :
					 * stop in the middle of literal segment
					 */
					cpy = oend;
					length = oend - op;
				}
				if ((endOnInput)
				    && (ip + length > iend)) {
					/*
					 * Error :
					 * read attempt beyond
					 * end of input buffer
					 */
					goto _output_error;
				}
			} else {
				if ((!endOnInput)
				    && (cpy != oend)) {
					/*
					 * Error :
					 * block decoding must
					 * stop exactly there
					 */
					goto _output_error;
				}
				if ((endOnInput)
				    && ((ip + length != iend)
					|| (cpy > oend))) {
					/*
					 * Error :
					 * input must be consumed
					 */
					goto _output_error;
				}
			}

			/*
			 * supports overlapping memory regions; only matters
			 * for in-place decompression scenarios
			 */
			LZ4_memmove(op, ip, length);
			ip += length;
			op += length;

			/* Necessarily EOF when !partialDecoding.
			 * When partialDecoding, it is EOF if we've either
			 * filled the output buffer or
			 * can't proceed with reading an offset for following match.
			 */
			if (!partialDecoding || (cpy == oend) || (ip >= (iend - 2)))
				break;
		} else {
			/* may overwrite up to WILDCOPYLENGTH beyond cpy */
			LZ4_wildCopy8(op, ip, cpy);
			ip += length;
			op = cpy;
		}

		/* get offset */
		offset = LZ4_readLE16(ip);
		ip += 2;
		match = op - offset;

		/* get matchlength */
		length = token & ML_MASK;

_copy_match:
		if ((checkOffset) && (unlikely(match + dictSize < lowPrefix))) {
			/* Error : offset outside buffers */
			goto _output_error;
		}

		if (length == ML_MASK) {

			variable_length_error error = ok;
			length +=
			    read_variable_length(&ip, iend - LASTLITERALS + 1,
						 endOnInput, 0, &error);
			if (error != ok)
				goto _output_error;

			if ((safeDecode)
				&& unlikely(
					(uptrval)(op) + length < (uptrval)op)) {
				/* overflow detection */
				goto _output_error;
			}
		}

		length += MINMATCH;

#if LZ4_FAST_DEC_LOOP
safe_match_copy:
#endif

		/* copy match within block */
		cpy = op + length;

		/*
		 * partialDecoding :
		 * may not respect endBlock parsing restrictions
		 */
		assert(op <= oend);
		if (partialDecoding &&
		    (cpy > oend - MATCH_SAFEGUARD_DISTANCE)) {
			size_t const mlen = min(length, (size_t)(oend - op));
			const BYTE * const matchEnd = match + mlen;
			BYTE * const copyEnd = op + mlen;

			if (matchEnd > op) {
				/* overlap copy */
				while (op < copyEnd)
					*op++ = *match++;
			} else {
				memcpy(op, match, mlen);
			}
			op = copyEnd;
			if (op == oend)
				break;
			continue;
		}

		if (unlikely(offset < 8)) {
			op[0] = match[0];
			op[1] = match[1];
			op[2] = match[2];
			op[3] = match[3];
			match += inc32table[offset];
			memcpy(op + 4, match, 4);
			match -= dec64table[offset];
		} else {
			LZ4_copy8(op, match);
			match += 8;
		}

		op += 8;

		if (unlikely(cpy > oend - MATCH_SAFEGUARD_DISTANCE)) {
			BYTE * const oCopyLimit = oend - (WILDCOPYLENGTH - 1);

			if (cpy > oend - LASTLITERALS) {
				/*
				 * Error : last LASTLITERALS bytes
				 * must be literals (uncompressed)
				 */
				goto _output_error;
			}

			if (op < oCopyLimit) {
				LZ4_wildCopy8(op, match, oCopyLimit);
				match += oCopyLimit - op;
				op = oCopyLimit;
			}
			while (op < cpy)
				*op++ = *match++;
		} else {
			LZ4_copy8(op, match);
			if (length > 16)
				LZ4_wildCopy8(op + 8, match + 8, cpy);
		}
		op = cpy; /* wildcopy correction */
	}

	/* end of decoding */
	if (endOnInput) {
		/* Nb of output bytes decoded */
		return (int) (((char *)op) - dst);
	} else {
		/* Nb of input bytes read */
		return (int) (((const char *)ip) - src);
	}

	/* Overflow error detected */
_output_error:
	return (int) (-(((const char *)ip) - src)) - 1;
}

int LZ4_decompress_safe(const char *source, char *dest,
	int compressedSize, int maxDecompressedSize)
{
	return LZ4_decompress_generic(source, dest,
				      compressedSize, maxDecompressedSize,
				      endOnInputSize, decode_full_block,
				      noDict, (BYTE *)dest, NULL, 0);
}

int LZ4_decompress_fast(const char *source, char *dest, int originalSize)
{
	return LZ4_decompress_generic(source, dest, 0, originalSize,
				      endOnOutputSize, decode_full_block,
				      withPrefix64k,
				      (BYTE *)dest - 64 * KB, NULL, 0);
}

#ifndef STATIC
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4 decompressor");
#endif
