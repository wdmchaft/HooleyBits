
#include <stdlib.h> /* for malloc() */
#include <string.h> /* for memcpy(), memset() */
#include <machine/endian.h> /* for ntohl() */
#include "bitmath.h"
#include "bitreader.h"
#include "crc.h"
#include "assert.h"
#include <assert.h>
#include "hooHacks.h"

//#define memmove custom_memmove
//#define memcpy custom_memcpy
//#define memset custom_memset

/* Things should be fastest when this matches the machine word size */
/* WATCHOUT: if you change this you must also change the following #defines down to COUNT_ZERO_MSBS below to match */
/* WATCHOUT: there are a few places where the code will not work unless brword is >= 32 bits wide */
/*           also, some sections currently only have fast versions for 4 or 8 bytes per word */
typedef FLAC__uint32 brword;
#define FLAC__BYTES_PER_WORD 4
#define FLAC__BITS_PER_WORD 32
#define FLAC__WORD_ALL_ONES ((FLAC__uint32)0xffffffff)
/* SWAP_BE_WORD_TO_HOST swaps bytes in a brword (which is always big-endian) if necessary to match host byte order */

//#define SWAP_BE_WORD_TO_HOST(x) ntohl(x)
#define SWAP_BE_WORD_TO_HOST(x) HooByteSwap(x)


/* counts the # of zero MSBs in a word */
#define COUNT_ZERO_MSBS(word) ( \
	(word) <= 0xffff ? \
		( (word) <= 0xff? byte_to_unary_table[word] + 24 : byte_to_unary_table[(word) >> 8] + 16 ) : \
		( (word) <= 0xffffff? byte_to_unary_table[word >> 16] + 8 : byte_to_unary_table[(word) >> 24] ) \
)
/* this alternate might be slightly faster on some systems/compilers: */
#define COUNT_ZERO_MSBS2(word) ( (word) <= 0xff ? byte_to_unary_table[word] + 24 : ((word) <= 0xffff ? byte_to_unary_table[(word) >> 8] + 16 : ((word) <= 0xffffff ? byte_to_unary_table[(word) >> 16] + 8 : byte_to_unary_table[(word) >> 24])) )


/*
 * This should be at least twice as large as the largest number of words
 * required to represent any 'number' (in any encoding) you are going to
 * read.  With FLAC this is on the order of maybe a few hundred bits.
 * If the buffer is smaller than that, the decoder won't be able to read
 * in a whole number that is in a variable length encoding (e.g. Rice).
 * But to be practical it should be at least 1K bytes.
 *
 * Increase this number to decrease the number of read callbacks, at the
 * expense of using more memory.  Or decrease for the reverse effect,
 * keeping in mind the limit from the first paragraph.  The optimal size
 * also depends on the CPU cache size and other factors; some twiddling
 * may be necessary to squeeze out the best performance.
 */
static const unsigned FLAC__BITREADER_DEFAULT_CAPACITY = 65536u / FLAC__BITS_PER_WORD; /* in words */

static const unsigned char byte_to_unary_table[] = {
	8, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

#ifdef min
#undef min
#endif
#define min(x,y) ((x)<(y)?(x):(y))
#ifdef max
#undef max
#endif
#define max(x,y) ((x)>(y)?(x):(y))

/* adjust for compilers that can't understand using LLU suffix for uint64_t literals */
#define FLAC__U64L(x) x##LLU

#define FLaC__INLINE

/* WATCHOUT: assembly routines rely on the order in which these fields are declared */
struct FLAC__BitReader {
	/* any partially-consumed word at the head will stay right-justified as bits are consumed from the left */
	/* any incomplete word at the tail will be left-justified, and bytes from the read callback are added on the right */
	brword *buffer;
	unsigned capacity; /* in words */
	unsigned words; /* # of completed words in buffer */
	unsigned bytes; /* # of bytes in incomplete word at buffer[words] */
	unsigned consumed_words; /* #words ... */
	unsigned consumed_bits; /* ... + (#bits of head word) already consumed from the front of buffer */
	unsigned read_crc16; /* the running frame CRC */
	unsigned crc16_align; /* the number of bits in the current consumed word that should not be CRC'd */
	FLAC__BitReaderReadCallback read_callback;
	void *client_data;
	FLAC__CPUInfo cpu_info;
};


static FLaC__INLINE void crc16_update_word_(FLAC__BitReader *br, brword word)
{
	register unsigned crc = br->read_crc16;
	switch(br->crc16_align) {
		case  0: crc = FLAC__CRC16_UPDATE((unsigned)(word >> 24), crc);
		case  8: crc = FLAC__CRC16_UPDATE((unsigned)((word >> 16) & 0xff), crc);
		case 16: crc = FLAC__CRC16_UPDATE((unsigned)((word >> 8) & 0xff), crc);
		case 24: br->read_crc16 = FLAC__CRC16_UPDATE((unsigned)(word & 0xff), crc);
	}

	br->crc16_align = 0;
}

/* would be static except it needs to be called by asm routines */
FLAC__bool bitreader_read_from_client_(FLAC__BitReader *br)
{
	unsigned start, end;
	size_t bytes;
	FLAC__byte *target;

	/* first shift the unconsumed buffer data toward the front as much as possible */
	if(br->consumed_words > 0) {
		start = br->consumed_words;
		end = br->words + (br->bytes? 1:0);
		memmove(br->buffer, br->buffer+start, FLAC__BYTES_PER_WORD * (end - start));

		br->words -= start;
		br->consumed_words = 0;
	}

	/*
	 * set the target for reading, taking into account word alignment and endianness
	 */
	bytes = (br->capacity - br->words) * FLAC__BYTES_PER_WORD - br->bytes;
	if(bytes == 0)
		return false; /* no space left, buffer is too small; see note for FLAC__BITREADER_DEFAULT_CAPACITY  */
	target = ((FLAC__byte*)(br->buffer+br->words)) + br->bytes;

	/* before reading, if the existing reader looks like this (say brword is 32 bits wide)
	 *   bitstream :  11 22 33 44 55            br->words=1 br->bytes=1 (partial tail word is left-justified)
	 *   buffer[BE]:  11 22 33 44 55 ?? ?? ??   (shown layed out as bytes sequentially in memory)
	 *   buffer[LE]:  44 33 22 11 ?? ?? ?? 55   (?? being don't-care)
	 *                               ^^-------target, bytes=3
	 * on LE machines, have to byteswap the odd tail word so nothing is
	 * overwritten:
	 */

	if(br->bytes)
		br->buffer[br->words] = SWAP_BE_WORD_TO_HOST(br->buffer[br->words]);

	/* now it looks like:
	 *   bitstream :  11 22 33 44 55            br->words=1 br->bytes=1
	 *   buffer[BE]:  11 22 33 44 55 ?? ?? ??
	 *   buffer[LE]:  44 33 22 11 55 ?? ?? ??
	 *                               ^^-------target, bytes=3
	 */

	/* read in the data; note that the callback may return a smaller number of bytes */
	if(!br->read_callback(target, &bytes, br->client_data))
		return false;

	/* after reading bytes 66 77 88 99 AA BB CC DD EE FF from the client:
	 *   bitstream :  11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF
	 *   buffer[BE]:  11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF ??
	 *   buffer[LE]:  44 33 22 11 55 66 77 88 99 AA BB CC DD EE FF ??
	 * now have to byteswap on LE machines:
	 */

	end = (br->words*FLAC__BYTES_PER_WORD + br->bytes + bytes + (FLAC__BYTES_PER_WORD-1)) / FLAC__BYTES_PER_WORD;

	for(start = br->words; start < end; start++)
		br->buffer[start] = SWAP_BE_WORD_TO_HOST(br->buffer[start]);

	/* now it looks like:
	 *   bitstream :  11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF
	 *   buffer[BE]:  11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF ??
	 *   buffer[LE]:  44 33 22 11 88 77 66 55 CC BB AA 99 ?? FF EE DD
	 * finally we'll update the reader values:
	 */
	end = br->words*FLAC__BYTES_PER_WORD + br->bytes + bytes;
	br->words = end / FLAC__BYTES_PER_WORD;
	br->bytes = end % FLAC__BYTES_PER_WORD;

	return true;
}

/***********************************************************************
 *
 * Class constructor/destructor
 *
 ***********************************************************************/

FLAC__BitReader *FLAC__bitreader_new(void)
{
	FLAC__BitReader *br = (FLAC__BitReader*)calloc(1, sizeof(FLAC__BitReader));

	/* calloc() implies:
		memset(br, 0, sizeof(FLAC__BitReader));
		br->buffer = 0;
		br->capacity = 0;
		br->words = br->bytes = 0;
		br->consumed_words = br->consumed_bits = 0;
		br->read_callback = 0;
		br->client_data = 0;
	*/
	return br;
}

void FLAC__bitreader_delete(FLAC__BitReader *br)
{
	FLAC__ASSERT(0 != br);
	FLAC__bitreader_free(br);
	free(br);
}

/***********************************************************************
 *
 * Public class methods
 *
 ***********************************************************************/

FLAC__bool FLAC__bitreader_init(FLAC__BitReader *br, FLAC__CPUInfo cpu, FLAC__BitReaderReadCallback rcb, void *cd)
{
	FLAC__ASSERT(0 != br);

	br->words = br->bytes = 0;
	br->consumed_words = br->consumed_bits = 0;
	br->capacity = FLAC__BITREADER_DEFAULT_CAPACITY;
	br->buffer = (brword*)malloc(sizeof(brword) * br->capacity);
	if(br->buffer == 0)
		return false;
	br->read_callback = rcb;
	br->client_data = cd;
	br->cpu_info = cpu;

	return true;
}

void FLAC__bitreader_free(FLAC__BitReader *br)
{
	FLAC__ASSERT(0 != br);
	if(0 != br->buffer)
		free(br->buffer);
	br->buffer = 0;
	br->capacity = 0;
	br->words = br->bytes = 0;
	br->consumed_words = br->consumed_bits = 0;
	br->read_callback = 0;
	br->client_data = 0;
}

FLAC__bool FLAC__bitreader_clear(FLAC__BitReader *br)
{
	br->words = br->bytes = 0;
	br->consumed_words = br->consumed_bits = 0;
	return true;
}

//void FLAC__bitreader_dump(const FLAC__BitReader *br, FILE *out)
//{
//	unsigned i, j;
//	if(br == 0) {
//		fprintf(out, "bitreader is NULL\n");
//	}
//	else {
//		fprintf(out, "bitreader: capacity=%u words=%u bytes=%u consumed: words=%u, bits=%u\n", br->capacity, br->words, br->bytes, br->consumed_words, br->consumed_bits);
//
//		for(i = 0; i < br->words; i++) {
//			fprintf(out, "%08X: ", i);
//			for(j = 0; j < FLAC__BITS_PER_WORD; j++)
//				if(i < br->consumed_words || (i == br->consumed_words && j < br->consumed_bits))
//					fprintf(out, ".");
//				else
//					fprintf(out, "%01u", br->buffer[i] & (1 << (FLAC__BITS_PER_WORD-j-1)) ? 1:0);
//			fprintf(out, "\n");
//		}
//		if(br->bytes > 0) {
//			fprintf(out, "%08X: ", i);
//			for(j = 0; j < br->bytes*8; j++)
//				if(i < br->consumed_words || (i == br->consumed_words && j < br->consumed_bits))
//					fprintf(out, ".");
//				else
//					fprintf(out, "%01u", br->buffer[i] & (1 << (br->bytes*8-j-1)) ? 1:0);
//			fprintf(out, "\n");
//		}
//	}
//}

void FLAC__bitreader_reset_read_crc16(FLAC__BitReader *br, FLAC__uint16 seed)
{
	FLAC__ASSERT(0 != br);
	FLAC__ASSERT(0 != br->buffer);
	FLAC__ASSERT((br->consumed_bits & 7) == 0);

	br->read_crc16 = (unsigned)seed;
	br->crc16_align = br->consumed_bits;
}

FLAC__uint16 FLAC__bitreader_get_read_crc16(FLAC__BitReader *br)
{
	FLAC__ASSERT(0 != br);
	FLAC__ASSERT(0 != br->buffer);
	FLAC__ASSERT((br->consumed_bits & 7) == 0);
	FLAC__ASSERT(br->crc16_align <= br->consumed_bits);

	/* CRC any tail bytes in a partially-consumed word */
	if(br->consumed_bits) {
		const brword tail = br->buffer[br->consumed_words];
		for( ; br->crc16_align < br->consumed_bits; br->crc16_align += 8)
			br->read_crc16 = FLAC__CRC16_UPDATE((unsigned)((tail >> (FLAC__BITS_PER_WORD-8-br->crc16_align)) & 0xff), br->read_crc16);
	}
	return br->read_crc16;
}

FLaC__INLINE FLAC__bool FLAC__bitreader_is_consumed_byte_aligned(const FLAC__BitReader *br)
{
	return ((br->consumed_bits & 7) == 0);
}

FLaC__INLINE unsigned FLAC__bitreader_bits_left_for_byte_alignment(const FLAC__BitReader *br)
{
	return 8 - (br->consumed_bits & 7);
}

FLaC__INLINE unsigned FLAC__bitreader_get_input_bits_unconsumed(const FLAC__BitReader *br)
{
	return (br->words-br->consumed_words)*FLAC__BITS_PER_WORD + br->bytes*8 - br->consumed_bits;
}

FLaC__INLINE FLAC__bool FLAC__bitreader_read_raw_uint32(FLAC__BitReader *br, FLAC__uint32 *val, unsigned bits)
{
	FLAC__ASSERT(0 != br);
	FLAC__ASSERT(0 != br->buffer);

	FLAC__ASSERT(bits <= 32);
	FLAC__ASSERT((br->capacity*FLAC__BITS_PER_WORD) * 2 >= bits);
	FLAC__ASSERT(br->consumed_words <= br->words);

	/* WATCHOUT: code does not work with <32bit words; we can make things much faster with this assertion */
	FLAC__ASSERT(FLAC__BITS_PER_WORD >= 32);

	if(bits == 0) { /* OPT: investigate if this can ever happen, maybe change to assertion */
		*val = 0;
		return true;
	}

	while((br->words-br->consumed_words)*FLAC__BITS_PER_WORD + br->bytes*8 - br->consumed_bits < bits) {
		if(!bitreader_read_from_client_(br))
			return false;
	}
	if(br->consumed_words < br->words) { /* if we've not consumed up to a partial tail word... */
		/* OPT: taking out the consumed_bits==0 "else" case below might make things faster if less code allows the compiler to inline this function */
		if(br->consumed_bits) {
			/* this also works when consumed_bits==0, it's just a little slower than necessary for that case */
			const unsigned n = FLAC__BITS_PER_WORD - br->consumed_bits;
			const brword word = br->buffer[br->consumed_words];
			if(bits < n) {
				*val = (word & (FLAC__WORD_ALL_ONES >> br->consumed_bits)) >> (n-bits);
				br->consumed_bits += bits;
				return true;
			}
			*val = word & (FLAC__WORD_ALL_ONES >> br->consumed_bits);
			bits -= n;
			crc16_update_word_(br, word);
			br->consumed_words++;
			br->consumed_bits = 0;
			if(bits) { /* if there are still bits left to read, there have to be less than 32 so they will all be in the next word */
				*val <<= bits;
				*val |= (br->buffer[br->consumed_words] >> (FLAC__BITS_PER_WORD-bits));
				br->consumed_bits = bits;
			}
			return true;
		}
		else {
			const brword word = br->buffer[br->consumed_words];
			if(bits < FLAC__BITS_PER_WORD) {
				*val = word >> (FLAC__BITS_PER_WORD-bits);
				br->consumed_bits = bits;
				return true;
			}
			/* at this point 'bits' must be == FLAC__BITS_PER_WORD; because of previous assertions, it can't be larger */
			*val = word;
			crc16_update_word_(br, word);
			br->consumed_words++;
			return true;
		}
	}
	else {
		/* in this case we're starting our read at a partial tail word;
		 * the reader has guaranteed that we have at least 'bits' bits
		 * available to read, which makes this case simpler.
		 */
		/* OPT: taking out the consumed_bits==0 "else" case below might make things faster if less code allows the compiler to inline this function */
		if(br->consumed_bits) {
			/* this also works when consumed_bits==0, it's just a little slower than necessary for that case */
			FLAC__ASSERT(br->consumed_bits + bits <= br->bytes*8);
			*val = (br->buffer[br->consumed_words] & (FLAC__WORD_ALL_ONES >> br->consumed_bits)) >> (FLAC__BITS_PER_WORD-br->consumed_bits-bits);
			br->consumed_bits += bits;
			return true;
		}
		else {
			*val = br->buffer[br->consumed_words] >> (FLAC__BITS_PER_WORD-bits);
			br->consumed_bits += bits;
			return true;
		}
	}
}

FLAC__bool FLAC__bitreader_read_raw_int32(FLAC__BitReader *br, FLAC__int32 *val, unsigned bits)
{
	/* OPT: inline raw uint32 code here, or make into a macro if possible in the .h file */
	if(!FLAC__bitreader_read_raw_uint32(br, (FLAC__uint32*)val, bits))
		return false;
	/* sign-extend: */
	*val <<= (32-bits);
	*val >>= (32-bits);
	return true;
}

FLAC__bool FLAC__bitreader_read_raw_uint64(FLAC__BitReader *br, FLAC__uint64 *val, unsigned bits)
{
	FLAC__uint32 hi, lo;

	if(bits > 32) {
		if(!FLAC__bitreader_read_raw_uint32(br, &hi, bits-32))
			return false;
		if(!FLAC__bitreader_read_raw_uint32(br, &lo, 32))
			return false;
		*val = hi;
		*val <<= 32;
		*val |= lo;
	}
	else {
		if(!FLAC__bitreader_read_raw_uint32(br, &lo, bits))
			return false;
		*val = lo;
	}
	return true;
}

FLaC__INLINE FLAC__bool FLAC__bitreader_read_uint32_little_endian(FLAC__BitReader *br, FLAC__uint32 *val)
{
	FLAC__uint32 x8, x32 = 0;

	/* this doesn't need to be that fast as currently it is only used for vorbis comments */

	if(!FLAC__bitreader_read_raw_uint32(br, &x32, 8))
		return false;

	if(!FLAC__bitreader_read_raw_uint32(br, &x8, 8))
		return false;
	x32 |= (x8 << 8);

	if(!FLAC__bitreader_read_raw_uint32(br, &x8, 8))
		return false;
	x32 |= (x8 << 16);

	if(!FLAC__bitreader_read_raw_uint32(br, &x8, 8))
		return false;
x32 |= (x8 << 24);

	*val = x32;
	return true;
}

FLAC__bool FLAC__bitreader_skip_bits_no_crc(FLAC__BitReader *br, unsigned bits)
{
	/*
	 * OPT: a faster implementation is possible but probably not that useful
	 * since this is only called a couple of times in the metadata readers.
	 */
	FLAC__ASSERT(0 != br);
	FLAC__ASSERT(0 != br->buffer);

	if(bits > 0) {
		const unsigned n = br->consumed_bits & 7;
		unsigned m;
		FLAC__uint32 x;

		if(n != 0) {
			m = min(8-n, bits);
			if(!FLAC__bitreader_read_raw_uint32(br, &x, m))
				return false;
			bits -= m;
		}
		m = bits / 8;
		if(m > 0) {
			if(!FLAC__bitreader_skip_byte_block_aligned_no_crc(br, m))
				return false;
			bits %= 8;
		}
		if(bits > 0) {
			if(!FLAC__bitreader_read_raw_uint32(br, &x, bits))
				return false;
		}
	}

	return true;
}

FLAC__bool FLAC__bitreader_skip_byte_block_aligned_no_crc(FLAC__BitReader *br, unsigned nvals)
{
	FLAC__uint32 x;

	FLAC__ASSERT(0 != br);
	FLAC__ASSERT(0 != br->buffer);
	FLAC__ASSERT(FLAC__bitreader_is_consumed_byte_aligned(br));

	/* step 1: skip over partial head word to get word aligned */
	while(nvals && br->consumed_bits) { /* i.e. run until we read 'nvals' bytes or we hit the end of the head word */
		if(!FLAC__bitreader_read_raw_uint32(br, &x, 8))
			return false;
		nvals--;
	}
	if(0 == nvals)
		return true;
	/* step 2: skip whole words in chunks */
	while(nvals >= FLAC__BYTES_PER_WORD) {
		if(br->consumed_words < br->words) {
			br->consumed_words++;
			nvals -= FLAC__BYTES_PER_WORD;
		}
		else if(!bitreader_read_from_client_(br))
			return false;
	}
	/* step 3: skip any remainder from partial tail bytes */
	while(nvals) {
		if(!FLAC__bitreader_read_raw_uint32(br, &x, 8))
			return false;
		nvals--;
	}

	return true;
}

FLAC__bool FLAC__bitreader_read_byte_block_aligned_no_crc(FLAC__BitReader *br, FLAC__byte *val, unsigned nvals)
{
	FLAC__uint32 x;

	FLAC__ASSERT(0 != br);
	FLAC__ASSERT(0 != br->buffer);
	FLAC__ASSERT(FLAC__bitreader_is_consumed_byte_aligned(br));

	/* step 1: read from partial head word to get word aligned */
	while(nvals && br->consumed_bits) { /* i.e. run until we read 'nvals' bytes or we hit the end of the head word */
		if(!FLAC__bitreader_read_raw_uint32(br, &x, 8))
			return false;
		*val++ = (FLAC__byte)x;
		nvals--;
	}
	if(0 == nvals)
		return true;
	/* step 2: read whole words in chunks */
	while(nvals >= FLAC__BYTES_PER_WORD) {
		if(br->consumed_words < br->words) {
			const brword word = br->buffer[br->consumed_words++];

			val[0] = (FLAC__byte)(word >> 24);
			val[1] = (FLAC__byte)(word >> 16);
			val[2] = (FLAC__byte)(word >> 8);
			val[3] = (FLAC__byte)word;

			val += FLAC__BYTES_PER_WORD;
			nvals -= FLAC__BYTES_PER_WORD;
		}
		else if(!bitreader_read_from_client_(br))
			return false;
	}
	/* step 3: read any remainder from partial tail bytes */
	while(nvals) {
		if(!FLAC__bitreader_read_raw_uint32(br, &x, 8))
			return false;
		*val++ = (FLAC__byte)x;
		nvals--;
	}

	return true;
}

FLaC__INLINE FLAC__bool FLAC__bitreader_read_unary_unsigned(FLAC__BitReader *br, unsigned *val)

{
	unsigned i;

	FLAC__ASSERT(0 != br);
	FLAC__ASSERT(0 != br->buffer);

	*val = 0;
	while(1) {
		while(br->consumed_words < br->words) { /* if we've not consumed up to a partial tail word... */
			brword b = br->buffer[br->consumed_words] << br->consumed_bits;
			if(b) {
				i = COUNT_ZERO_MSBS(b);
				*val += i;
				i++;
				br->consumed_bits += i;
				if(br->consumed_bits >= FLAC__BITS_PER_WORD) { /* faster way of testing if(br->consumed_bits == FLAC__BITS_PER_WORD) */
					crc16_update_word_(br, br->buffer[br->consumed_words]);
					br->consumed_words++;
					br->consumed_bits = 0;
				}
				return true;
			}
			else {
				*val += FLAC__BITS_PER_WORD - br->consumed_bits;
				crc16_update_word_(br, br->buffer[br->consumed_words]);
				br->consumed_words++;
				br->consumed_bits = 0;
				/* didn't find stop bit yet, have to keep going... */
			}
		}
		/* at this point we've eaten up all the whole words; have to try
		 * reading through any tail bytes before calling the read callback.
		 * this is a repeat of the above logic adjusted for the fact we
		 * don't have a whole word.  note though if the client is feeding
		 * us data a byte at a time (unlikely), br->consumed_bits may not
		 * be zero.
		 */
		if(br->bytes) {
			const unsigned end = br->bytes * 8;
			brword b = (br->buffer[br->consumed_words] & (FLAC__WORD_ALL_ONES << (FLAC__BITS_PER_WORD-end))) << br->consumed_bits;
			if(b) {
				i = COUNT_ZERO_MSBS(b);
				*val += i;
				i++;
				br->consumed_bits += i;
				FLAC__ASSERT(br->consumed_bits < FLAC__BITS_PER_WORD);
				return true;
			}
			else {
				*val += end - br->consumed_bits;
				br->consumed_bits += end;
				FLAC__ASSERT(br->consumed_bits < FLAC__BITS_PER_WORD);
				/* didn't find stop bit yet, have to keep going... */
			}
		}
		if(!bitreader_read_from_client_(br))
			return false;
	}
}

FLAC__bool FLAC__bitreader_read_rice_signed(FLAC__BitReader *br, int *val, unsigned parameter)
{
	FLAC__uint32 lsbs = 0, msbs = 0;
	unsigned uval;

	FLAC__ASSERT(0 != br);
	FLAC__ASSERT(0 != br->buffer);
	FLAC__ASSERT(parameter <= 31);

	/* read the unary MSBs and end bit */
	if(!FLAC__bitreader_read_unary_unsigned(br, &msbs))
		return false;

	/* read the binary LSBs */
	if(!FLAC__bitreader_read_raw_uint32(br, &lsbs, parameter))
		return false;

	/* compose the value */
	uval = (msbs << parameter) | lsbs;
	if(uval & 1)
		*val = -((int)(uval >> 1)) - 1;
	else
		*val = (int)(uval >> 1);

	return true;
}

/* this is by far the most heavily used reader call.  it ain't pretty but it's fast */
/* a lot of the logic is copied, then adapted, from FLAC__bitreader_read_unary_unsigned() and FLAC__bitreader_read_raw_uint32() */
FLAC__bool FLAC__bitreader_read_rice_signed_block(FLAC__BitReader *br, int vals[], unsigned nvals, unsigned parameter)
/* OPT: possibly faster version for use with MSVC */

{

	unsigned i;
	unsigned uval = 0;

	/* try and get br->consumed_words and br->consumed_bits into register;
	 * must remember to flush them back to *br before calling other
	 * bitwriter functions that use them, and before returning */
	register unsigned cwords;
	register unsigned cbits;
	unsigned ucbits; /* keep track of the number of unconsumed bits in the buffer */

	FLAC__ASSERT(0 != br);
	FLAC__ASSERT(0 != br->buffer);
	/* WATCHOUT: code does not work with <32bit words; we can make things much faster with this assertion */
	FLAC__ASSERT(FLAC__BITS_PER_WORD >= 32);
	FLAC__ASSERT(parameter < 32);
	/* the above two asserts also guarantee that the binary part never straddles more than 2 words, so we don't have to loop to read it */

	if(nvals == 0)
		return true;

	cbits = br->consumed_bits;
	cwords = br->consumed_words;
	ucbits = (br->words-cwords)*FLAC__BITS_PER_WORD + br->bytes*8 - cbits;

	while(1) {

		/* read unary part */
		while(1) {
			while(cwords < br->words) { /* if we've not consumed up to a partial tail word... */
				brword b = br->buffer[cwords] << cbits;
				if(b) {

					i = COUNT_ZERO_MSBS(b);
					uval += i;
					cbits += i;
					cbits++; /* skip over stop bit */
					if(cbits >= FLAC__BITS_PER_WORD) { /* faster way of testing if(cbits == FLAC__BITS_PER_WORD) */
						crc16_update_word_(br, br->buffer[cwords]);
						cwords++;
						cbits = 0;
					}
					goto break1;
				}
				else {
					uval += FLAC__BITS_PER_WORD - cbits;
					crc16_update_word_(br, br->buffer[cwords]);
					cwords++;
					cbits = 0;
					/* didn't find stop bit yet, have to keep going... */
				}
			}
			/* at this point we've eaten up all the whole words; have to try
			 * reading through any tail bytes before calling the read callback.
			 * this is a repeat of the above logic adjusted for the fact we
			 * don't have a whole word.  note though if the client is feeding
			 * us data a byte at a time (unlikely), br->consumed_bits may not
			 * be zero.
			 */
			if(br->bytes) {
				const unsigned end = br->bytes * 8;
				brword b = (br->buffer[cwords] & ~(FLAC__WORD_ALL_ONES >> end)) << cbits;
				if(b) {
					i = COUNT_ZERO_MSBS(b);
					uval += i;
					cbits += i;
					cbits++; /* skip over stop bit */
					FLAC__ASSERT(cbits < FLAC__BITS_PER_WORD);
					goto break1;
				}
				else {
					uval += end - cbits;
					cbits += end;
					FLAC__ASSERT(cbits < FLAC__BITS_PER_WORD);
					/* didn't find stop bit yet, have to keep going... */
				}
			}
			/* flush registers and read; bitreader_read_from_client_() does
			 * not touch br->consumed_bits at all but we still need to set
			 * it in case it fails and we have to return false.
			 */
			br->consumed_bits = cbits;
			br->consumed_words = cwords;
			if(!bitreader_read_from_client_(br))
				return false;
			cwords = br->consumed_words;
			ucbits = (br->words-cwords)*FLAC__BITS_PER_WORD + br->bytes*8 - cbits + uval;
			/* + uval to offset our count by the # of unary bits already
			 * consumed before the read, because we will add these back
			 * in all at once at break1
			 */
		}
break1:
		ucbits -= uval;
		ucbits--; /* account for stop bit */

		/* read binary part */
		FLAC__ASSERT(cwords <= br->words);

		if(parameter) {
			while(ucbits < parameter) {
				/* flush registers and read; bitreader_read_from_client_() does
				 * not touch br->consumed_bits at all but we still need to set
				 * it in case it fails and we have to return false.
				 */
				br->consumed_bits = cbits;
				br->consumed_words = cwords;
				if(!bitreader_read_from_client_(br))
					return false;
				cwords = br->consumed_words;
				ucbits = (br->words-cwords)*FLAC__BITS_PER_WORD + br->bytes*8 - cbits;
			}
			if(cwords < br->words) { /* if we've not consumed up to a partial tail word... */
				if(cbits) {
					/* this also works when consumed_bits==0, it's just slower than necessary for that case */
					const unsigned n = FLAC__BITS_PER_WORD - cbits;
					const brword word = br->buffer[cwords];
					if(parameter < n) {
						uval <<= parameter;
						uval |= (word & (FLAC__WORD_ALL_ONES >> cbits)) >> (n-parameter);
						cbits += parameter;
					}
					else {
						uval <<= n;
						uval |= word & (FLAC__WORD_ALL_ONES >> cbits);
						crc16_update_word_(br, word);
						cwords++;
						cbits = parameter - n;
						if(cbits) { /* parameter > n, i.e. if there are still bits left to read, there have to be less than 32 so they will all be in the next word */
							uval <<= cbits;
							uval |= (br->buffer[cwords] >> (FLAC__BITS_PER_WORD-cbits));
						}
					}
				}
				else {
					cbits = parameter;
					uval <<= parameter;
					uval |= br->buffer[cwords] >> (FLAC__BITS_PER_WORD-cbits);
				}
			}
			else {
				/* in this case we're starting our read at a partial tail word;
				 * the reader has guaranteed that we have at least 'parameter'
				 * bits available to read, which makes this case simpler.
				 */
				uval <<= parameter;
				if(cbits) {
					/* this also works when consumed_bits==0, it's just a little slower than necessary for that case */
					FLAC__ASSERT(cbits + parameter <= br->bytes*8);
					uval |= (br->buffer[cwords] & (FLAC__WORD_ALL_ONES >> cbits)) >> (FLAC__BITS_PER_WORD-cbits-parameter);
					cbits += parameter;
				}
				else {
					cbits = parameter;
					uval |= br->buffer[cwords] >> (FLAC__BITS_PER_WORD-cbits);
				}
			}
		}

		ucbits -= parameter;

		/* compose the value */
		*vals = (int)(uval >> 1 ^ -(int)(uval & 1));

		/* are we done? */
		--nvals;
		if(nvals == 0) {
			br->consumed_bits = cbits;
			br->consumed_words = cwords;
			return true;
		}

		uval = 0;
		++vals;

	}
}



/* on return, if *val == 0xffffffff then the utf-8 sequence was invalid, but the return value will be true */
FLAC__bool FLAC__bitreader_read_utf8_uint32(FLAC__BitReader *br, FLAC__uint32 *val, FLAC__byte *raw, unsigned *rawlen)
{
	FLAC__uint32 v = 0;
	FLAC__uint32 x;
	unsigned i;

	if(!FLAC__bitreader_read_raw_uint32(br, &x, 8))
		return false;
	if(raw)
		raw[(*rawlen)++] = (FLAC__byte)x;
	if(!(x & 0x80)) { /* 0xxxxxxx */
		v = x;
		i = 0;
	}
	else if(x & 0xC0 && !(x & 0x20)) { /* 110xxxxx */
		v = x & 0x1F;
		i = 1;
	}
	else if(x & 0xE0 && !(x & 0x10)) { /* 1110xxxx */
		v = x & 0x0F;
		i = 2;
	}
	else if(x & 0xF0 && !(x & 0x08)) { /* 11110xxx */
		v = x & 0x07;
		i = 3;
	}
	else if(x & 0xF8 && !(x & 0x04)) { /* 111110xx */
		v = x & 0x03;
		i = 4;
	}
	else if(x & 0xFC && !(x & 0x02)) { /* 1111110x */
		v = x & 0x01;
		i = 5;
	}
	else {
		*val = 0xffffffff;
		return true;
	}
	for( ; i; i--) {
		if(!FLAC__bitreader_read_raw_uint32(br, &x, 8))
			return false;
		if(raw)
			raw[(*rawlen)++] = (FLAC__byte)x;
		if(!(x & 0x80) || (x & 0x40)) { /* 10xxxxxx */
			*val = 0xffffffff;
			return true;
		}
		v <<= 6;
		v |= (x & 0x3F);
	}
	*val = v;
	return true;
}

/* on return, if *val == 0xffffffffffffffff then the utf-8 sequence was invalid, but the return value will be true */
FLAC__bool FLAC__bitreader_read_utf8_uint64(FLAC__BitReader *br, FLAC__uint64 *val, FLAC__byte *raw, unsigned *rawlen)
{
	FLAC__uint64 v = 0;
	FLAC__uint32 x;
	unsigned i;

	if(!FLAC__bitreader_read_raw_uint32(br, &x, 8))
		return false;
	if(raw)
		raw[(*rawlen)++] = (FLAC__byte)x;
	if(!(x & 0x80)) { /* 0xxxxxxx */
		v = x;
		i = 0;
	}
	else if(x & 0xC0 && !(x & 0x20)) { /* 110xxxxx */
		v = x & 0x1F;
		i = 1;
	}
	else if(x & 0xE0 && !(x & 0x10)) { /* 1110xxxx */
		v = x & 0x0F;
		i = 2;
	}
	else if(x & 0xF0 && !(x & 0x08)) { /* 11110xxx */
		v = x & 0x07;
		i = 3;
	}
	else if(x & 0xF8 && !(x & 0x04)) { /* 111110xx */
		v = x & 0x03;
		i = 4;
	}
	else if(x & 0xFC && !(x & 0x02)) { /* 1111110x */
		v = x & 0x01;
		i = 5;
	}
	else if(x & 0xFE && !(x & 0x01)) { /* 11111110 */
		v = 0;
		i = 6;
	}
	else {
		*val = FLAC__U64L(0xffffffffffffffff);
		return true;
	}
	for( ; i; i--) {
		if(!FLAC__bitreader_read_raw_uint32(br, &x, 8))
			return false;
		if(raw)
			raw[(*rawlen)++] = (FLAC__byte)x;
		if(!(x & 0x80) || (x & 0x40)) { /* 10xxxxxx */
			*val = FLAC__U64L(0xffffffffffffffff);
			return true;
		}
		v <<= 6;
		v |= (x & 0x3F);
	}
	*val = v;
	return true;
}
