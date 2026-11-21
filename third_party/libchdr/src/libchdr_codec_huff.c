/* Modified by Nakagawa Recomp contributors, 2026-11-20. The local changes are marked
 * LOCAL DELTA and listed in third_party/libchdr/PROVENANCE.md. */

#include "codec_huff.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../include/libchdr/huffman.h"

chd_error huff_codec_init(void* codec, uint32_t hunkbytes)
{
	huff_codec_data* huff_codec = (huff_codec_data*) codec;
	(void)hunkbytes;
	huff_codec->decoder = create_huffman_decoder(256, 16);
	/* Nakagawa LOCAL DELTA (2026-11-20): report a decoder that could not be
	 * allocated (upstream returned CHDERR_NONE and decoded through NULL). */
	if (huff_codec->decoder == NULL)
		return CHDERR_OUT_OF_MEMORY;
	return CHDERR_NONE;
}

void huff_codec_free(void *codec)
{
	huff_codec_data* huff_codec = (huff_codec_data*) codec;
	delete_huffman_decoder(huff_codec->decoder);
}

chd_error huff_codec_decompress(void *codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen)
{
	huff_codec_data* huff_codec = (huff_codec_data*) codec;
	struct bitstream* bitbuf = create_bitstream(src, complen);
	uint32_t cur;
	chd_error result;
	enum huffman_error err;

	/* Nakagawa LOCAL DELTA (2026-11-20): the bitstream may not have been
	 * allocated, and the tree import reports its own allocation failure. */
	if (bitbuf == NULL)
		return CHDERR_OUT_OF_MEMORY;

	/* first import the tree */
	err = huffman_import_tree_huffman(huff_codec->decoder, bitbuf);
	if (err != HUFFERR_NONE)
	{
		free(bitbuf);
		return (err == HUFFERR_OUT_OF_MEMORY) ? CHDERR_OUT_OF_MEMORY : CHDERR_DECOMPRESSION_ERROR;
	}

	/* then decode the data */
	for (cur = 0; cur < destlen; cur++)
		dest[cur] = huffman_decode_one(huff_codec->decoder, bitbuf);
	bitstream_flush(bitbuf);
	result = bitstream_overflow(bitbuf) ? CHDERR_DECOMPRESSION_ERROR : CHDERR_NONE;

	free(bitbuf);
	return result;
}
