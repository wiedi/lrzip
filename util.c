/*
   Copyright (C) 2011 Serge Belyshev
   Copyright (C) 2006-2011 Con Kolivas
   Copyright (C) 2008, 2011 Peter Hyman
   Copyright (C) 1998 Andrew Tridgell

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/*
  Utilities used in rzip

  tridge, June 1996
  */

/*
 * Realloc removed
 * Functions added
 *    read_config()
 * Peter Hyman, December 2008
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdarg.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <termios.h>

#if defined(linux)
#include <linux/fs.h>
#elif defined(__APPLE__)
#include <sys/disk.h>
#endif

#ifdef _SC_PAGE_SIZE
# define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
#else
# define PAGE_SIZE (4096)
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "lrzip_private.h"
#include "liblrzip.h"
#include "util.h"
#include "sha4.h"
#include "aes.h"

static const char *infile = NULL;
static char delete_infile = 0;
static const char *outfile = NULL;
static char delete_outfile = 0;
static FILE *outputfile = NULL;

void register_infile(const char *name, char delete)
{
	infile = name;
	delete_infile = delete;
}

void register_outfile(const char *name, char delete)
{
	outfile = name;
	delete_outfile = delete;
}

void register_outputfile(FILE *f)
{
	outputfile = f;
}

void unlink_files(void)
{
	/* Delete temporary files generated for testing or faking stdio */
	if (outfile && delete_outfile)
		unlink(outfile);

	if (infile && delete_infile)
		unlink(infile);
}

static void fatal_exit(void)
{
	struct termios termios_p;

	/* Make sure we haven't died after disabling stdin echo */
	tcgetattr(fileno(stdin), &termios_p);
	termios_p.c_lflag |= ECHO;
	tcsetattr(fileno(stdin), 0, &termios_p);

	unlink_files();
	fprintf(outputfile, "Fatal error - exiting\n");
	fflush(outputfile);
	exit(1);
}

/* Failure when there is likely to be a meaningful error in perror */
void fatal(const char *format, ...)
{
	va_list ap;

	if (format) {
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
	}

	perror(NULL);
	fatal_exit();
}

void failure(const char *format, ...)
{
	va_list ap;

	if (format) {
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
	}

	fatal_exit();
}

void round_to_page(i64 *size)
{
	*size -= *size % PAGE_SIZE;
	if (unlikely(!*size))
		*size = PAGE_SIZE;
}

void get_rand(uchar *buf, int len)
{
	int fd, i;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1) {
		for (i = 0; i < len; i++)
			buf[i] = (uchar)random();
	} else {
		if (unlikely(read(fd, buf, len) != len))
			fatal("Failed to read fd in get_rand\n");
		if (unlikely(close(fd)))
			fatal("Failed to close fd in get_rand\n");
	}
}

static void xor128 (void *pa, const void *pb)
{
	i64 *a = pa;
	const i64 *b = pb;

	a [0] ^= b [0];
	a [1] ^= b [1];
}

static void lrz_keygen(const rzip_control *control, const uchar *salt, uchar *key, uchar *iv)
{
	uchar buf [HASH_LEN + SALT_LEN + PASS_LEN];
	mlock(buf, HASH_LEN + SALT_LEN + PASS_LEN);

	memcpy(buf, control->hash, HASH_LEN);
	memcpy(buf + HASH_LEN, salt, SALT_LEN);
	memcpy(buf + HASH_LEN + SALT_LEN, control->salt_pass, control->salt_pass_len);
	sha4(buf, HASH_LEN + SALT_LEN + control->salt_pass_len, key, 0);

	memcpy(buf, key, HASH_LEN);
	memcpy(buf + HASH_LEN, salt, SALT_LEN);
	memcpy(buf + HASH_LEN + SALT_LEN, control->salt_pass, control->salt_pass_len);
	sha4(buf, HASH_LEN + SALT_LEN + control->salt_pass_len, iv, 0);

	memset(buf, 0, sizeof(buf));
	munlock(buf, sizeof(buf));
}

void lrz_crypt(const rzip_control *control, uchar *buf, i64 len, const uchar *salt, int encrypt)
{
	/* Encryption requires CBC_LEN blocks so we can use ciphertext
	* stealing to not have to pad the block */
	uchar key[HASH_LEN], iv[HASH_LEN];
	uchar tmp0[CBC_LEN], tmp1[CBC_LEN];
	aes_context aes_ctx;
	i64 N, M;

	/* Generate unique key and IV for each block of data based on salt */
	mlock(&aes_ctx, sizeof(aes_ctx));
	mlock(key, HASH_LEN);
	mlock(iv, HASH_LEN);

	lrz_keygen(control, salt, key, iv);

	M = len % CBC_LEN;
	N = len - M;

	if (encrypt == LRZ_ENCRYPT) {
		print_maxverbose("Encrypting data        \n");
		if (unlikely(aes_setkey_enc(&aes_ctx, key, 128)))
			failure("Failed to aes_setkey_enc in lrz_crypt\n");
		aes_crypt_cbc(&aes_ctx, AES_ENCRYPT, N, iv, buf, buf);
		
		if (M) {
			memset(tmp0, 0, CBC_LEN);
			memcpy(tmp0, buf + N, M);
			aes_crypt_cbc(&aes_ctx, AES_ENCRYPT, CBC_LEN,
				iv, tmp0, tmp1);
			memcpy(buf + N, buf + N - CBC_LEN, M);
			memcpy(buf + N - CBC_LEN, tmp1, CBC_LEN);
		}
	} else {
		if (unlikely(aes_setkey_dec(&aes_ctx, key, 128)))
			failure("Failed to aes_setkey_dec in lrz_crypt\n");
		print_maxverbose("Decrypting data        \n");
		if (M) {
			aes_crypt_cbc(&aes_ctx, AES_DECRYPT, N - CBC_LEN,
				      iv, buf, buf);
			aes_crypt_ecb(&aes_ctx, AES_DECRYPT,
				      buf + N - CBC_LEN, tmp0);
			memset(tmp1, 0, CBC_LEN);
			memcpy(tmp1, buf + N, M);
			xor128(tmp0, tmp1);
			memcpy(buf + N, tmp0, M);
			memcpy(tmp1 + M, tmp0 + M, CBC_LEN - M);
			aes_crypt_ecb(&aes_ctx, AES_DECRYPT, tmp1,
				      buf + N - CBC_LEN);
			xor128(buf + N - CBC_LEN, iv);
		} else
			aes_crypt_cbc(&aes_ctx, AES_DECRYPT, len,
				      iv, buf, buf);
	}

	memset(&aes_ctx, 0, sizeof(aes_ctx));
	memset(iv, 0, HASH_LEN);
	memset(key, 0, HASH_LEN);
	munlock(&aes_ctx, sizeof(aes_ctx));
	munlock(iv, HASH_LEN);
	munlock(key, HASH_LEN);
}

void lrz_stretch(rzip_control *control)
{
	sha4_context ctx;
	i64 j, n, counter;

	mlock(&ctx, sizeof(ctx));
	sha4_starts(&ctx, 0);

	n = control->encloops * HASH_LEN / (control->salt_pass_len + sizeof(i64));
	print_maxverbose("Hashing passphrase %lld (%lld) times \n", control->encloops, n);
	for (j = 0; j < n; j ++) {
		counter = htole64(j);
		sha4_update(&ctx, (uchar *)&counter, sizeof(counter));
		sha4_update(&ctx, control->salt_pass, control->salt_pass_len);
	}
	sha4_finish(&ctx, control->hash);
	memset(&ctx, 0, sizeof(ctx));
	munlock(&ctx, sizeof(ctx));
}

i64 device_size(int fd)
{
	#if defined(linux)
		i64 size = 0;
		ioctl(fd, BLKGETSIZE64, &size);
		return size;
	#elif defined(__APPLE__)
		int ret;
		i32 block_size  = 0;
		i64 block_count = 0;

		ret = ioctl(fd, DKIOCGETBLOCKSIZE, &block_size);
		if (ret) return 0;
	
		ret = ioctl(fd, DKIOCGETBLOCKCOUNT, &block_count);
		if (ret) return 0;

		return block_size * block_count;
	#else
		return 0;
	#endif
}
