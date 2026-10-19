// SPDX-License-Identifier: GPL-2.0-only
/* Extract AVM PROM v1-v3 records and ACAL v1 board/calibration payloads. */
#define _FILE_OFFSET_BITS 64
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define MAX_DATA (2 * 1024 * 1024)
#define ACAL_HEADER 44

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)p[0] << 8 | p[1];
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)be16(p) << 16 | be16(p + 2);
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
	uint16_t crc = 0;
	unsigned int i;

	while (n--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xa001 : 0);
	}
	return crc;
}

static int read_at(int fd, void *buf, size_t n, uint64_t off)
{
	ssize_t r;
	uint8_t *p = buf;

	while (n) {
		r = pread(fd, p, n, off);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		p += r;
		n -= r;
		off += r;
	}
	return 0;
}

static uint8_t *read_file(const char *path, size_t *size, size_t limit)
{
	struct stat st;
	uint8_t *data = NULL;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
	    st.st_size <= 0 || (uint64_t)st.st_size > limit)
		goto out;
	data = malloc(st.st_size);
	if (!data)
		goto out;
	if (read_at(fd, data, st.st_size, 0)) {
		free(data);
		data = NULL;
	} else {
		*size = st.st_size;
	}
out:
	close(fd);
	return data;
}

static unsigned int record_type(uint8_t type, int *compressed)
{
	switch (type) {
	case 7:
		*compressed = 1;
		return 0;
	case 8:
		*compressed = 1;
		return 2;
	case 18:
		*compressed = 1;
		return 19;
	case 26:
		*compressed = 1;
		return 25;
	default: return type;
	}
}

static uint8_t *read_record(int fd, uint64_t off, uint64_t size,
			   unsigned int type, size_t *outlen)
{
	uint8_t header[8], *raw = NULL, *out = NULL;
	size_t len, hdrlen = 4;
	int compressed = 0;
	z_stream z = { 0 };

	if (off > size || size - off < 8 || read_at(fd, header, 8, off))
		return NULL;
	if (header[0] == 1 || header[0] == 2) {
		len = be16(header + 2);
	} else if (header[0] == 3) {
		if (be16(header + 2) & ~1)
			return NULL;
		compressed = be16(header + 2) & 1;
		len = be32(header + 4);
		hdrlen = 8;
	} else {
		return NULL;
	}
	if (record_type(header[1], &compressed) != type ||
	    !len || len > MAX_DATA || len > size - off - hdrlen)
		return NULL;
	raw = malloc(len);
	if (!raw || read_at(fd, raw, len, off + hdrlen))
		goto out;
	if (!compressed) {
		*outlen = len;
		return raw;
	}
	out = malloc(MAX_DATA);
	if (!out)
		goto out;
	z.next_in = raw;
	z.avail_in = len;
	z.next_out = out;
	z.avail_out = MAX_DATA;
	if (inflateInit(&z) != Z_OK)
		goto bad;
	if (inflate(&z, Z_FINISH) != Z_STREAM_END || z.avail_in) {
		inflateEnd(&z);
		goto bad;
	}
	*outlen = z.total_out;
	inflateEnd(&z);
	goto out;
bad:
	free(out);
	out = NULL;
out:
	free(raw);
	return out;
}

static uint16_t xor16(const uint8_t *p, size_t len)
{
	uint16_t sum = 0;

	while (len >= 2) {
		sum ^= p[0] | (uint16_t)p[1] << 8;
		p += 2;
		len -= 2;
	}
	return sum;
}

struct region {
	uint32_t start, len;
};

struct block {
	uint32_t size, mask;
	struct region regions[2];
};

/* Factory-specific regions retained when updating the manufacturer's BDF. */
static const struct block alder[] = {
	{ 131072, 255, { { 25688, 4024 }, { 87538, 256 } } },
};
static const struct block waikiki[] = {
	{ 40960, 255, { { 0, 0 }, { 0, 0 } } },
	{ 22528, 1, { { 6340, 520 }, { 7896, 76 } } },
	{ 59392, 2, { { 13484, 3764 }, { 22412, 216 } } },
	{ 61440, 4, { { 13476, 3764 }, { 22404, 216 } } },
};

static int patch_board(uint8_t *bdf, const uint8_t *factory, size_t len)
{
	const struct block *blocks;
	size_t nblocks, off = 0, i, j, checksum;
	uint8_t bands = 255;
	uint16_t sum;

	if (len & 1 || xor16(bdf, len) != 0xffff ||
	    xor16(factory, len) != 0xffff)
		return -1;
	if (len == 131072) {
		blocks = alder;
		nblocks = sizeof(alder) / sizeof(*alder);
		checksum = 10;
	} else {
		if (len <= 557 || bdf[557] != factory[557] ||
		    !bdf[557] || (bdf[557] & ~7))
			return -1;
		bands = bdf[557];
		blocks = waikiki;
		nblocks = sizeof(waikiki) / sizeof(*waikiki);
		checksum = 12;
	}
	for (i = 0; i < nblocks; i++) {
		if (!(bands & blocks[i].mask))
			continue;
		if (blocks[i].size > len - off)
			return -1;
		for (j = 0; j < 2; j++) {
			const struct region *r = &blocks[i].regions[j];

			if (r->start > blocks[i].size ||
			    r->len > blocks[i].size - r->start)
				return -1;
			memcpy(bdf + off + r->start, factory + off + r->start, r->len);
		}
		off += blocks[i].size;
	}
	if (off != len)
		return -1;
	sum = xor16(bdf, len) ^ 0xffff;
	bdf[checksum] ^= sum;
	bdf[checksum + 1] ^= sum >> 8;
	return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
	char *tmp;
	FILE *f;
	int fd, ret = -1;

	tmp = malloc(strlen(path) + sizeof(".XXXXXX"));
	if (!tmp)
		return -1;
	sprintf(tmp, "%s.XXXXXX", path);
	fd = mkstemp(tmp);
	if (fd < 0)
		goto out;
	f = fdopen(fd, "wb");
	if (!f) {
		close(fd);
		goto unlink;
	}
	if (fwrite(data, 1, len, f) != len) {
		fclose(f);
		goto unlink;
	}
	if (fclose(f))
		goto unlink;
	ret = rename(tmp, path);
unlink:
	unlink(tmp);
out:
	free(tmp);
	return ret;
}

int main(int argc, char **argv)
{
	uint8_t *offsets = NULL, *acal = NULL, *bdf = NULL, *candidate;
	size_t offsets_len = 0, acal_len = 0, bdf_len = 0, candidate_len = 0;
	uint32_t board_off, board_len, cal_off, cal_len;
	unsigned long type, hw;
	uint64_t size;
	unsigned int stride = 1;
	struct stat st;
	char *end;
	size_t i;
	int fd = -1, ret = EXIT_FAILURE;

	if (argc != 8) {
		fprintf(stderr, "Usage: %s device offsets-file type hwrev "
			"template board-out cal-out\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	errno = 0;
	type = strtoul(argv[3], &end, 0);
	if (errno || end == argv[3] || *end || (type != 0 && type != 2 && type != 19 && type != 25))
		goto out;
	hw = strtoul(argv[4], &end, 0);
	if (errno || *end || !hw || hw > UINT32_MAX)
		goto out;
	offsets = read_file(argv[2], &offsets_len, 27 * 4);
	bdf = read_file(argv[5], &bdf_len, MAX_DATA);
	fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (!offsets || offsets_len % 4 || !bdf || fd < 0 || fstat(fd, &st))
		goto out;
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &size) || ioctl(fd, BLKPBSZGET, &stride) ||
		    stride < 512 || stride > 65536 || (stride & (stride - 1)))
			goto out;
	} else if (S_ISREG(st.st_mode) && st.st_size > 0) {
		size = st.st_size;
	} else {
		goto out;
	}
	for (i = 0; i < offsets_len; i += 4) {
		candidate = read_record(fd, (uint64_t)be32(offsets + i) * stride,
					size, type, &candidate_len);
		if (!candidate)
			continue;
		if (acal) {
			free(candidate);
			fprintf(stderr, "Duplicate calibration record\n");
			goto out;
		}
		acal = candidate;
		acal_len = candidate_len;
	}
	if (!acal || acal_len < ACAL_HEADER || memcmp(acal, "ACAL", 4) ||
	    be16(acal + 4) != 1 || be32(acal + 8) != hw)
		goto out;
	board_off = be32(acal + 24);
	board_len = be32(acal + 28);
	cal_off = be32(acal + 36);
	cal_len = be32(acal + 40);
	/* The packer stores two consecutive payloads with offsets relative to 44. */
	if (board_off || board_len != bdf_len || cal_off != board_len || !cal_len ||
	    (uint64_t)board_len + cal_len + ACAL_HEADER != acal_len ||
	    crc16(acal + 8, acal_len - 8) != be16(acal + 6) ||
	    patch_board(bdf, acal + ACAL_HEADER, bdf_len))
		goto out;
	/* Publish board data last: its firmware request starts radio calibration. */
	if (write_file(argv[7], acal + ACAL_HEADER + cal_off, cal_len) ||
	    write_file(argv[6], bdf, bdf_len))
		goto out;
	ret = EXIT_SUCCESS;
out:
	if (ret)
		fprintf(stderr, "Unable to extract valid factory calibration\n");
	if (fd >= 0)
		close(fd);
	free(offsets);
	free(acal);
	free(bdf);
	return ret;
}
