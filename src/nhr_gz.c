/*
 *   Copyright (c) 2016 Kulykov Oleh <info@resident.name>
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *   THE SOFTWARE.
 */


#include "nhr_gz.h"

#if defined(NHR_GZIP)

#include "nhr_memory.h"
#include "nhr_string.h"

#include <zlib.h>
#include <time.h>

#define NHR_GZ_CHUNK_SIZE 512
#define NHR_GZ_HEADER_SIZE 10
#define NHR_GZ_FOOTER_SIZE 8
#define NHR_GZ_WINDOWS_BITS -15

void * nhr_gz_extend(void * buff, size_t * buff_size, const size_t increment) {
	const size_t new_size = *buff_size + increment;
	void * new_buff = nhr_malloc(new_size);
	memcpy(new_buff, buff, *buff_size);
	nhr_free(buff);
	*buff_size = new_size;
	return new_buff;
}

void nhr_gz_write_header(unsigned char * buff) {
	*buff++ = 0x1f; // magic header
	*buff++ = 0x8b; // magic header
	*buff++ = 8; // deflate
	*buff++ = 0; // bit 0 set: file probably ascii text

	// 4 bytes file modification time in Unix format
	// used `uLong` as unsigned 32 bit integer
	const uLong curr_time = (uLong)time(NULL);
	memcpy(buff, &curr_time, 4);
	buff += 4;

	*buff++ = 0; //2; // XFL = 2 - compressor used maximum compression, slowest algorithm
	*buff = 0; //0xff; // OS type: unknown
}

void * nhr_gz_write_footer(void * buff,
						   size_t buff_size,
						   size_t writed,
						   const void * src_buff,
						   const size_t src_size) {
	// used `uLong` as unsigned 32 bit integer
	const size_t left = buff_size - writed;
	if (left < 8) buff = nhr_gz_extend(buff, &buff_size, 8);
	uLong * footer = buff + writed;

	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, src_buff, (uInt)src_size);

	*footer++ = (uLong)crc;
	*footer = (uLong)src_size;

	return buff;
}

void * nhr_gz_compress(const void * buff,
					   const size_t buff_size,
					   size_t * compressed_size,
					   const unsigned char method) {
	if (!buff || buff_size == 0) return NULL;

	z_stream zip;
	void * out_buff = NULL;
	size_t writed = 0, out_size = NHR_GZ_CHUNK_SIZE;

	memset(&zip, 0, sizeof(z_stream));

	int result = Z_ERRNO;
	switch (method) {
		case NHR_GZ_METHOD_GZIP:
			result = deflateInit2(&zip, Z_BEST_COMPRESSION, Z_DEFLATED, NHR_GZ_WINDOWS_BITS, 8, Z_DEFAULT_STRATEGY);
			break;
		case NHR_GZ_METHOD_DEFLATE:
			result = deflateInit(&zip, Z_BEST_COMPRESSION);
			break;
		default:
			break;
	}

	if (result != Z_OK) return NULL;

	if (method == NHR_GZ_METHOD_GZIP) {
		out_size += NHR_GZ_HEADER_SIZE;
		out_buff = nhr_malloc(out_size);
		nhr_gz_write_header(out_buff);
		writed += NHR_GZ_HEADER_SIZE;
	} else {
		out_buff = nhr_malloc(out_size);
	}

	zip.avail_in = (uInt)buff_size;
	zip.next_in = (Bytef *)buff;
	zip.avail_out = NHR_GZ_CHUNK_SIZE;
	zip.next_out = out_buff + writed;

	while (zip.avail_in != 0) {
		if (deflate(&zip, Z_NO_FLUSH) != Z_OK) {
			deflateEnd(&zip);
			nhr_free(out_buff);
			return NULL;
		}
		if (zip.avail_out == 0) {
			out_buff = nhr_gz_extend(out_buff, &out_size, NHR_GZ_CHUNK_SIZE);
			writed += NHR_GZ_CHUNK_SIZE;
			zip.next_out = out_buff + writed;
			zip.avail_out = NHR_GZ_CHUNK_SIZE;
		}
	}

	result = Z_OK;
	while (result == Z_OK) {
		if (zip.avail_out == 0) {
			out_buff = nhr_gz_extend(out_buff, &out_size, NHR_GZ_CHUNK_SIZE);
			writed += NHR_GZ_CHUNK_SIZE;
			zip.next_out = out_buff + writed;
			zip.avail_out = NHR_GZ_CHUNK_SIZE;
		}
		result = deflate(&zip, Z_FINISH);
	}

	if (result != Z_STREAM_END) {
		deflateEnd(&zip);
		nhr_free(out_buff);
		return NULL;
	}

	writed = zip.total_out; // write total from zip stream
	if (method == NHR_GZ_METHOD_GZIP) {
		writed += NHR_GZ_HEADER_SIZE;
		out_buff = nhr_gz_write_footer(out_buff, out_size, writed, buff, buff_size);
		writed += NHR_GZ_FOOTER_SIZE;
	}

	if (compressed_size) *compressed_size = writed;

	deflateEnd(&zip);
	return out_buff;
}

int nhr_gz_is_gzip_file(const unsigned char * buff, const size_t buff_size) {
	if (buff_size <= NHR_GZ_HEADER_SIZE + NHR_GZ_FOOTER_SIZE) return 0;
	return buff[0] == 0x1f && buff[1] == 0x8b;
}

void * nhr_gz_decompress(const void * buff,
						 const size_t buff_size,
						 size_t * decompressed_size,
						 const unsigned char method) {
	//TODO: process gz header
	if (!buff || buff_size == 0) return NULL;

	z_stream zip;
	memset(&zip, 0, sizeof(z_stream));
	void * out_buff = NULL;
	size_t out_size = NHR_GZ_CHUNK_SIZE;
	size_t writed = 0;

	int available_size = 0;

	// The fields next_in, avail_in, zalloc, zfree and opaque must be initialized before by the caller.

	int result = Z_ERRNO;
	switch (method) {
		case NHR_GZ_METHOD_GZIP:
			if (nhr_gz_is_gzip_file(buff, buff_size)) {
				zip.avail_in = (uInt)buff_size - NHR_GZ_HEADER_SIZE - NHR_GZ_FOOTER_SIZE;
				zip.next_in = (Bytef *)buff + NHR_GZ_HEADER_SIZE;
				result = inflateInit2(&zip, NHR_GZ_WINDOWS_BITS);
			}
			break;
		case NHR_GZ_METHOD_DEFLATE:
			zip.avail_in = (uInt)buff_size;
			zip.next_in = (Bytef *)buff;
			result = inflateInit(&zip);
			break;
		default:
			break;
	}

	if (result != Z_OK) return NULL;

	out_buff = nhr_malloc(NHR_GZ_CHUNK_SIZE);

	zip.avail_out = NHR_GZ_CHUNK_SIZE;

	do {
		zip.next_out = out_buff + writed;

		result = inflate(&zip, Z_NO_FLUSH);
		switch (result) {
			case Z_NEED_DICT:
				result = Z_DATA_ERROR;
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
			case Z_STREAM_ERROR:
				nhr_free(out_buff);
				inflateEnd(&zip);
				return NULL;
		}
		available_size = ((int)NHR_GZ_CHUNK_SIZE) - zip.avail_out;
		if (available_size > 0) {
			writed += available_size;
			if (writed < out_size) {
				zip.avail_out = ((int)NHR_GZ_CHUNK_SIZE) - available_size;
			} else {
				out_buff = nhr_gz_extend(out_buff, &out_size, NHR_GZ_CHUNK_SIZE);
				zip.avail_out = NHR_GZ_CHUNK_SIZE;
			}
		} else {
			inflateEnd(&zip);
			nhr_free(out_buff);
			return NULL;
		}
	} while (result != Z_STREAM_END);

	inflateEnd(&zip);

	if (decompressed_size) *decompressed_size = writed;
	return out_buff;
}

#endif
