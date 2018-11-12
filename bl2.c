#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <unistd.h>
#include <fcntl.h>

#include "gxlimg.h"
#include "bl2.h"
#include "ssl.h"

#define FOUT_MODE_DFT (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)

#define htole8(val) (val)
#define le8toh(val) (val)
#define bh_wr(h, sz, off, val)						\
	(*(uint ## sz ## _t *)((h) + off) = htole ## sz(val))
#define bh_rd(h, sz, off)						\
	(le ## sz ## toh(*(uint ## sz ## _t *)((h) + off)))

#define BL2IMG_TOTSZ (0xc000)
#define BL2RAND_SZ 0x10
#define BL2HDR_SZ 0x40
#define BL2HASH_SZ 0x200
#define BL2KEY_SZ 0xD80
#define BL2KEYHDR_SZ 0x30
#define BL2BIN_SZ (BL2IMG_TOTSZ -					\
		(BL2RAND_SZ + BL2HDR_SZ + BL2HASH_SZ + BL2KEYHDR_SZ +	\
			BL2KEY_SZ))
#define BL2SHA2_LEN 0x20
#define BL2HDR_MAGIC (*(uint32_t *)"@AML")
/**
 * BL2 binary file context
 */
struct bl2 {
	size_t payloadsz;
	size_t totlen;
	size_t hash_start; /* Start of Hashed payload */
	size_t hash_end; /* End of hashed payload */
	uint8_t flag;
};
#define BF_RSA (1 << 0)
#define BF_IS_RSA(h) ((h)->flag & BF_RSA)
#define BF_SET_RSA(h) ((h)->flag |= BF_RSA)

/**
 * Initialize bl2 context from BL2 binary file descriptor
 *
 * @param bl2: BL2 binary descriptor to init
 * @param fd: BL2 binary file descriptor
 * @return: 0 on success, negative number otherwise
 */
static int gi_bl2_init(struct bl2 *bl2, int fd)
{
	off_t fsz;

	fsz = lseek(fd, 0, SEEK_END);
	bl2->payloadsz = fsz;
	bl2->flag = 0; /* Not RSA signature support yet */
	bl2->hash_start = BL2HDR_SZ + BL2SHA2_LEN;
	bl2->hash_end = BL2HASH_SZ + BL2KEYHDR_SZ + BL2KEY_SZ + BL2BIN_SZ -
		BL2SHA2_LEN;
	bl2->totlen = BL2HDR_SZ + BL2HASH_SZ + BL2KEYHDR_SZ +BL2KEY_SZ +
		BL2BIN_SZ;

	return 0;
}

/**
 * Read a block of data from a file
 *
 * @param fd: File descriptor to read a block from
 * @param blk: Filled with read data
 * @param sz: Size of block to read from file
 * @return: Negative number on error, read size otherwise. The only reason that
 * return value could be different from sz on success is when EOF has been
 * encountered while reading the file.
 */
static ssize_t gi_bl2_read_blk(int fd, uint8_t *blk, size_t sz)
{
	size_t i;
	ssize_t nr = 1;

	for(i = 0; (i < sz) && (nr != 0); i += nr) {
		nr = read(fd, blk + i, sz - i);
		if(nr < 0)
			goto out;
	}
	nr = i;
out:
	return nr;
}

/**
 * Write a block of data into a file
 *
 * @param fd: File descriptor to write a block into
 * @param blk: Actual block data
 * @param sz: Size of block to write into file
 * @return: Negative number on error, sz otherwise.
 */
static ssize_t gi_bl2_write_blk(int fd, uint8_t *blk, size_t sz)
{
	size_t i;
	ssize_t nr;

	for(i = 0; i < sz; i += nr) {
		nr = write(fd, blk + i, sz - i);
		if(nr < 0)
			goto out;
	}
	nr = i;
out:
	return nr;
}

static int gi_bl2_dump_hdr(struct bl2 const *bl2, int fd)
{
	uint8_t hdr[BL2HDR_SZ] = {};
	uint8_t rd[BL2RAND_SZ];
	size_t i;
	ssize_t nr;

	if(BF_IS_RSA(bl2)) {
		ERR("BL2 RSA signature not supported yet\n");
		return -EINVAL;
	}

	srand(time(NULL));
	for(i = 0; i < BL2RAND_SZ; ++i)
		rd[i] = rand();

	lseek(fd, 0, SEEK_SET);
	bh_wr(hdr, 32, 0x00, BL2HDR_MAGIC);
	bh_wr(hdr, 8, 0x0a, 1);
	bh_wr(hdr, 8, 0x0b, 1);
	bh_wr(hdr, 16, 0x08, BL2HDR_SZ);
	bh_wr(hdr, 32, 0x10, 0); /* SHA256 signature, no RSA */
	bh_wr(hdr, 32, 0x20, 0); /* Null RSA KEY type */
	bh_wr(hdr, 32, 0x28, BL2KEYHDR_SZ + BL2KEY_SZ);
	bh_wr(hdr, 32, 0x18, BL2HASH_SZ);
	bh_wr(hdr, 32, 0x14, BL2HDR_SZ); /* HDR size */
	bh_wr(hdr, 16, 0x1c, bl2->hash_start); /* Beginning of hashed payload */
	bh_wr(hdr, 16, 0x24, BL2HDR_SZ + BL2HASH_SZ); /* RSA KEY Offset */
	bh_wr(hdr, 16, 0x38, BL2BIN_SZ);
	bh_wr(hdr, 16, 0x34, BL2HDR_SZ + BL2HASH_SZ + BL2KEYHDR_SZ + BL2KEY_SZ); /* Payload offset */
	bh_wr(hdr, 16, 0x04, bl2->totlen);
	bh_wr(hdr, 16, 0x2c, bl2->hash_end);

	nr = gi_bl2_write_blk(fd, rd, sizeof(rd));
	if(nr != sizeof(rd)) {
		PERR("Failed to write random number in bl2 boot img: ");
		return (int)nr;
	}

	nr = gi_bl2_write_blk(fd, hdr, sizeof(hdr));
	if(nr != sizeof(hdr)) {
		PERR("Failed to write header in bl2 boot img: ");
		return (int)nr;
	}
	return 0;
}

static int gi_bl2_dump_key(struct bl2 const *bl2, int fd)
{
	uint32_t val;
	if(BF_IS_RSA(bl2)) {
		ERR("BL2 RSA signature not supported yet\n");
		return -EINVAL;
	}

	lseek(fd, BL2RAND_SZ + BL2HDR_SZ + BL2HASH_SZ + 0x18, SEEK_SET);
	val = htole32(0x298);
	gi_bl2_write_blk(fd, (uint8_t *)(&val), 4);

	lseek(fd, BL2RAND_SZ + 0x8ec, SEEK_SET); /* TODO What is this offset */
	val = htole32(0x240);
	gi_bl2_write_blk(fd, (uint8_t *)(&val), 4);

	lseek(fd, BL2RAND_SZ + 0xb20, SEEK_SET); /* TODO What is this offset */
	val = htole32(0x298);
	gi_bl2_write_blk(fd, (uint8_t *)(&val), 4);
	return 0;
}

static int gi_bl2_dump_binary(struct bl2 const *bl2, int fout, int fin)
{
	uint8_t block[1024];
	size_t nr;
	ssize_t rd, wr;
	int ret;

	(void)bl2;

	lseek(fin, 0, SEEK_SET);
	lseek(fout, BL2RAND_SZ + BL2HDR_SZ + BL2HASH_SZ + BL2KEYHDR_SZ +
			BL2KEY_SZ, SEEK_SET);

	for(nr = 0; nr < BL2BIN_SZ; nr += rd) {
		rd = gi_bl2_read_blk(fin, block, sizeof(block));
		if(rd <= 0) {
			ret = (int)rd;
			goto out;
		}
		wr = gi_bl2_write_blk(fout, block, sizeof(block));
		if(wr != rd) {
			ret = (int)wr;
			goto out;
		}
	}

	ret = 0;

out:
	return ret;
}

static int gi_bl2_sign(struct bl2 const *bl2, int fd)
{
	EVP_MD_CTX *ctx;
	uint8_t tmp[1024];
	uint8_t hash[BL2SHA2_LEN];
	size_t i;
	ssize_t nr;
	int ret;

	ctx = EVP_MD_CTX_new();
	if(ctx == NULL) {
		SSLERR(ret, "Cannot create digest context: ");
		goto out;
	}

	ret = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
	if(ret != 1) {
		SSLERR(ret, "Cannot init digest context: ");
		goto out;
	}

	/* Hash header */
	lseek(fd, BL2RAND_SZ, SEEK_SET);
	nr = gi_bl2_read_blk(fd, tmp, BL2HDR_SZ);
	if((nr < 0) || (nr != BL2HDR_SZ)) {
		PERR("Cannot read header from fd %d: ", fd);
		ret = (int)nr;
		goto out;
	}
	ret = EVP_DigestUpdate(ctx, tmp, nr);
	if(ret != 1) {
		SSLERR(ret, "Cannot hash header block: ");
		goto out;
	}

	/* Hash payload */
	lseek(fd, BL2RAND_SZ + bl2->hash_start, SEEK_SET);
	for(i = 0; i < bl2->hash_end - bl2->hash_start; i += nr) {
		nr = gi_bl2_read_blk(fd, tmp, sizeof(tmp));
		if(nr < 0) {
			PERR("Cannot read fd %d:", fd);
			ret = (int)nr;
			goto out;
		}
		ret = EVP_DigestUpdate(ctx, tmp, nr);
		if(ret != 1) {
			SSLERR(ret, "Cannot hash data block: ");
			goto out;
		}
	}

	ret = EVP_DigestFinal_ex(ctx, hash, NULL);
	if(ret != 1) {
		SSLERR(ret, "Cannot finalize hash: ");
		goto out;
	}

	/* Only SHA256 signature is supported so far */
	lseek(fd, BL2RAND_SZ + BL2HDR_SZ, SEEK_SET);
	nr = gi_bl2_write_blk(fd, hash, BL2SHA2_LEN);
	if(nr != BL2SHA2_LEN) {
		PERR("Cannot write SHA sig in fd %d:", fd);
		ret = (int)nr;
		goto out;
	}

	ret = 0;
out:
	EVP_MD_CTX_free(ctx);
	return ret;
}

int gi_bl2_create_img(char const *fin, char const *fout)
{
	struct bl2 bl2;
	int fdin = -1, fdout = -1, ret;

	DBG("Create bl2 boot image from %s in %s\n", fin, fout);

	fdin = open(fin, O_RDONLY);
	if(fdin < 0) {
		PERR("Cannot open file %s", fin);
		ret = -errno;
		goto out;
	}

	fdout = open(fout, O_RDWR | O_CREAT, FOUT_MODE_DFT);
	if(fdout < 0) {
		PERR("Cannot open file %s", fout);
		ret = -errno;
		goto out;
	}

	ret = ftruncate(fdout, 0);
	if(ret < 0)
		goto out;

	ret = gi_bl2_init(&bl2, fdin);
	if(ret < 0)
		goto out;

	/* Fill the whole file with zeros */
	ret = ftruncate(fdout, bl2.totlen + BL2RAND_SZ);
	if(ret < 0)
		goto out;

	ret = gi_bl2_dump_hdr(&bl2, fdout);
	if(ret < 0)
		goto out;

	ret = gi_bl2_dump_key(&bl2, fdout);
	if(ret < 0)
		goto out;

	ret = gi_bl2_dump_binary(&bl2, fdout, fdin);
	if(ret < 0)
		goto out;

	ret = gi_bl2_sign(&bl2, fdout);
out:
	return ret;
}

int gi_bl2_extract(char const *fin, char const *fout)
{
	(void)fin;
	(void)fout;

	ERR("BL2 decoding is not implemented yet\n");

	return -1;
}
