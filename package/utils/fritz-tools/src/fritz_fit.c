// SPDX-License-Identifier: GPL-2.0-only
/* Inspect AVM FIT images and expose their unaligned rootfs read-only. */

#define _FILE_OFFSET_BITS 64
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libfdt.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define AVM_HEADER_SIZE 72
#define AVM_MAGIC 0xfeed000d
#define MAX_FIT_SIZE (256U * 1024 * 1024)

struct fit_image {
	uint64_t offset, length;
	const char *name, *type, *compression;
	uint32_t crc;
};

static int read_at(int fd, void *buffer, size_t length, uint64_t offset)
{
	unsigned char *p = buffer;
	ssize_t n;

	while (length) {
		n = pread(fd, p, length, offset);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		length -= n;
		p += n;
		offset += n;
	}
	return 0;
}

static const char *string_prop(const void *fit, int node, const char *key)
{
	int length;
	const char *p = fdt_getprop(fit, node, key, &length);

	if (!p || length < 2 || p[length - 1] || memchr(p, 0, length - 1))
		return NULL;
	return p;
}

static bool safe_name(const char *name)
{
	static const char allowed[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_,.+-@";

	return name && *name &&
	       strspn(name, allowed) == strlen(name);
}

static int number_prop(const void *fit, int node, const char *key, uint32_t *value)
{
	int length;
	const fdt32_t *p = fdt_getprop(fit, node, key, &length);

	if (!p || length != sizeof(*p))
		return -1;
	*value = fdt32_to_cpu(*p);
	return 0;
}

static int image_info(const void *fit, int node, uint64_t base, uint64_t size,
		      struct fit_image *image)
{
	const unsigned char *data;
	const char *algo;
	uint32_t position, length, crc;
	int len, hash;
	bool have_crc = false;

	image->name = fdt_get_name(fit, node, NULL);
	image->type = string_prop(fit, node, "type");
	image->compression = string_prop(fit, node, "compression");
	if (!safe_name(image->name) || !safe_name(image->type) ||
	    !safe_name(image->compression))
		return -1;
	data = fdt_getprop(fit, node, "data", &len);
	if (data) {
		image->offset = base + (data - (const unsigned char *)fit);
		image->length = len;
		if (fdt_getprop(fit, node, "data-position", NULL) ||
		    fdt_getprop(fit, node, "data-offset", NULL) ||
		    fdt_getprop(fit, node, "data-size", NULL))
			return -1;
	} else {
		if (number_prop(fit, node, "data-size", &length))
			return -1;
		if (!number_prop(fit, node, "data-position", &position)) {
			if (fdt_getprop(fit, node, "data-offset", NULL))
				return -1;
			image->offset = base + position;
		} else if (!number_prop(fit, node, "data-offset", &position)) {
			image->offset = base + fdt_totalsize(fit) + position;
		} else {
			return -1;
		}
		image->length = length;
		if (image->offset < base + fdt_totalsize(fit))
			return -1;
	}
	if (!image->length || image->offset > size || image->length > size - image->offset)
		return -1;
	fdt_for_each_subnode(hash, fit, node) {
		algo = string_prop(fit, hash, "algo");
		if (!algo || strcmp(algo, "crc32"))
			continue;
		if (number_prop(fit, hash, "value", &crc) || (have_crc && crc != image->crc))
			return -1;
		image->crc = crc;
		have_crc = true;
	}
	/* CRC checks corruption only; this is not signature authentication. */
	return have_crc ? 0 : -1;
}

static int check_image(int fd, const struct fit_image *image)
{
	unsigned char buffer[65536];
	uint64_t offset = image->offset, remaining = image->length;
	uint32_t crc = crc32(0, NULL, 0);
	size_t n;

	while (remaining) {
		n = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		if (read_at(fd, buffer, n, offset))
			return -1;
		crc = crc32(crc, buffer, n);
		offset += n;
		remaining -= n;
	}
	if (crc != image->crc) {
		fprintf(stderr, "CRC mismatch for %s\n", image->name);
		return -1;
	}
	return 0;
}

static int copy_image(int fd, const struct fit_image *image)
{
	unsigned char buffer[65536];
	uint64_t offset = image->offset, remaining = image->length;
	size_t n;

	while (remaining) {
		n = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		if (read_at(fd, buffer, n, offset) || fwrite(buffer, 1, n, stdout) != n)
			return -1;
		offset += n;
		remaining -= n;
	}
	return fflush(stdout) ? -1 : 0;
}

static int rootfs_node(const void *fit, int images, int config)
{
	static const char * const keys[] = { "squashFS", "filesystem", "loadables" };
	const char *name, *type;
	int n, count, node, selected = -1;
	size_t i;

	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		count = fdt_stringlist_count(fit, config, keys[i]);
		if (count == -FDT_ERR_NOTFOUND)
			continue;
		if (count < 0)
			return -1;
		for (n = 0; n < count; n++) {
			name = fdt_stringlist_get(fit, config, keys[i], n, NULL);
			if (!safe_name(name))
				return -1;
			node = fdt_subnode_offset(fit, images, name);
			if (node < 0)
				return -1;
			type = string_prop(fit, node, "type");
			if (!type || strcmp(type, "filesystem"))
				continue;
			if (selected >= 0 && selected != node)
				return -1;
			selected = node;
		}
	}
	return selected;
}

static int map_rootfs(int fd, const char *device, const struct fit_image *image)
{
	struct loop_config config = { 0 };
	unsigned char super[96];
	uint32_t magic;
	uint64_t used;
	int loop, ret;

	if (strcmp(image->type, "filesystem") || strcmp(image->compression, "none") ||
	    image->length < sizeof(super) || image->length % 512 ||
	    read_at(fd, super, sizeof(super), image->offset)) {
		fprintf(stderr, "Rootfs must be an uncompressed, sector-sized SquashFS image\n");
		return -1;
	}
	memcpy(&magic, super, sizeof(magic));
	memcpy(&used, super + 40, sizeof(used));
	if (le32toh(magic) != 0x73717368 || le64toh(used) < sizeof(super) ||
	    le64toh(used) > image->length) {
		fprintf(stderr, "Invalid SquashFS superblock\n");
		return -1;
	}
	loop = open(device, O_RDWR | O_CLOEXEC);
	if (loop < 0) {
		perror(device);
		return -1;
	}
	config.fd = fd;
	config.block_size = 512;
	config.info.lo_offset = image->offset;
	config.info.lo_sizelimit = image->length;
	config.info.lo_flags = LO_FLAGS_READ_ONLY;
	ret = ioctl(loop, LOOP_CONFIGURE, &config);
	if (ret)
		perror("LOOP_CONFIGURE");
	close(loop);
	return ret;
}

static void usage(const char *name)
{
	fprintf(stderr, "Usage: %s [-c configuration] [-x image|-r|-L loop-device] input\n"
		"  Verify CRC32 and list FIT images (AVM headers are detected automatically).\n"
		"  -x extracts an image; -r extracts the selected rootfs, both to stdout.\n"
		"  -L maps the selected SquashFS read-only on an unused loop device.\n"
		"  Detach the loop device with losetup -d after unmounting.\n", name);
}

int main(int argc, char **argv)
{
	unsigned char header[AVM_HEADER_SIZE + sizeof(struct fdt_header)]
		__attribute__((aligned(8)));
	uint32_t magic, outer_size, fit_size;
	uint64_t size, base = 0;
	struct fit_image image = { 0 }, selected = { 0 };
	struct stat st;
	void *fit = NULL;
	const char *config_name = NULL, *extract = NULL, *loop = NULL;
	int fd = -1, config, configs, images, node, select_node = -1, opt;
	int action = 0, ret = EXIT_FAILURE;

	while ((opt = getopt(argc, argv, "c:x:rL:")) != -1) {
		switch (opt) {
		case 'c':
			config_name = optarg;
			break;
		case 'x':
			extract = optarg;
			/* fall through */
		case 'r':
		case 'L':
			if (action)
				goto invalid;
			action = opt;
			if (opt == 'L')
				loop = optarg;
			break;
		default:
			goto invalid;
		}
	}
	if (argc - optind != 1 || (extract && !safe_name(extract)))
		goto invalid;
	fd = open(argv[optind], O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st)) {
		perror(argv[optind]);
		goto out;
	}
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &size))
			goto out;
	} else if (S_ISREG(st.st_mode)) {
		size = st.st_size;
	} else {
		fprintf(stderr, "Input must be a regular file or block device\n");
		goto out;
	}
	if (size > INT64_MAX || size < sizeof(header) || read_at(fd, header, sizeof(header), 0))
		goto bad_fit;
	memcpy(&magic, header, sizeof(magic));
	if (le32toh(magic) == AVM_MAGIC)
		base = AVM_HEADER_SIZE;
	if (fdt_check_header(header + base))
		goto bad_fit;
	fit_size = fdt_totalsize(header + base);
	if (fit_size > MAX_FIT_SIZE || fit_size > size - base)
		goto bad_fit;
	if (base) {
		memcpy(&outer_size, header + 4, sizeof(outer_size));
		if (le32toh(outer_size) != fit_size)
			goto bad_fit;
	}
	fit = malloc(fit_size);
	if (!fit || read_at(fd, fit, fit_size, base))
		goto out;
	if (fdt_check_full(fit, fit_size))
		goto bad_fit;
	configs = fdt_path_offset(fit, "/configurations");
	images = fdt_path_offset(fit, "/images");
	if (configs < 0 || images < 0)
		goto bad_fit;
	if (!config_name)
		config_name = string_prop(fit, configs, "default");
	if (!safe_name(config_name))
		goto bad_fit;
	config = fdt_subnode_offset(fit, configs, config_name);
	if (config < 0)
		goto bad_fit;
	if (extract)
		select_node = fdt_subnode_offset(fit, images, extract);
	else if (action)
		select_node = rootfs_node(fit, images, config);
	if (action && select_node < 0) {
		fprintf(stderr, "Requested image or unique rootfs not found\n");
		goto out;
	}
	if (!action)
		printf("configuration\t%s\n", config_name);
	fdt_for_each_subnode(node, fit, images) {
		if (image_info(fit, node, base, size, &image))
			goto bad_fit;
		if (check_image(fd, &image))
			goto out;
		if (node == select_node)
			selected = image;
		if (!action)
			printf("%s\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\tcrc32:%08" PRIx32 "\n",
			       image.name, image.type, image.compression,
			       image.offset, image.length, image.crc);
	}
	if (action) {
		if (!selected.length)
			goto bad_fit;
		if (loop ? map_rootfs(fd, loop, &selected) : copy_image(fd, &selected))
			goto out;
	} else if (fflush(stdout)) {
		goto out;
	}
	ret = EXIT_SUCCESS;
	goto out;

bad_fit:
	fprintf(stderr, "Invalid, truncated or unsupported FIT image\n");
	goto out;
invalid:
	usage(argv[0]);
out:
	free(fit);
	if (fd >= 0)
		close(fd);
	return ret;
}
