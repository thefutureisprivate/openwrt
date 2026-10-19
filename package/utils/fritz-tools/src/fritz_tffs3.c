// SPDX-License-Identifier: GPL-2.0-only
/* Read the no-OOB TFFS 3.1 format used on AVM block2mtd devices. */

#define _FILE_OFFSET_BITS 64
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <mtd/mtd-user.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK_MAGIC UINT64_C(0x41564d5f54464653)
#define ENTRY_MAGIC UINT64_C(0x544646535f4e3331)
#define HEADER_SIZE 36
#define NAMES_ID 0x1ff
#define MAX_ERASE_SIZE (16U * 1024 * 1024)
#define MAX_DATA (64U * 1024 * 1024)
#define MAX_ENTRIES 65536

struct entry {
	uint32_t id, revision, segment, next, sequence, length;
	uint64_t offset;
	unsigned char *data;
};

struct index {
	struct entry *entries;
	size_t count, capacity, data_size;
	unsigned int blocks, corrupt;
};

struct value {
	size_t first, end, length;
	bool deleted;
};

static uint32_t get32(const void *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return be32toh(v);
}

static uint64_t get64(const void *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return be64toh(v);
}

/* TFFS uses the non-reflected polynomial, with no final complement. */
static uint32_t crc32_be(uint32_t crc, const unsigned char *p, size_t size)
{
	unsigned int bit;

	while (size--) {
		crc ^= (uint32_t)*p++ << 24;
		for (bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^ ((crc >> 31) ? 0x04c11db7 : 0);
	}
	return crc;
}

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
		p += n;
		offset += n;
		length -= n;
	}
	return 0;
}

static bool bad_page(uint32_t offset, const uint64_t *bad, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (offset == bad[i])
			return true;
	return false;
}

static int add_entry(struct index *idx, struct entry *entry)
{
	struct entry *new;
	size_t capacity;

	if (idx->count == MAX_ENTRIES || entry->length > MAX_DATA - idx->data_size)
		return -1;
	if (idx->count == idx->capacity) {
		capacity = idx->capacity ? idx->capacity * 2 : 128;
		new = realloc(idx->entries, capacity * sizeof(*new));
		if (!new)
			return -1;
		idx->entries = new;
		idx->capacity = capacity;
	}
	idx->entries[idx->count++] = *entry;
	idx->data_size += entry->length;
	return 0;
}

static int scan_block(struct index *idx, unsigned char *block, uint32_t erase,
		      uint32_t page, uint64_t offset)
{
	uint64_t bad[4];
	uint32_t pos, end, length, crc, expected, n, used, sequence;
	unsigned int count, i;
	unsigned char header[HEADER_SIZE];
	struct entry entry;

	if (get64(block) != BLOCK_MAGIC)
		return 0;
	if (get32(block + 8) != 0x00030001 || get32(block + 12) != 2 ||
	    get32(block + 24) != 1) {
		fprintf(stderr, "Unsupported TFFS block at 0x%" PRIx64 "\n", offset);
		return -1;
	}
	sequence = get32(block + 16);
	count = get32(block + 28);
	if (count > 4)
		goto bad_header;
	for (i = 0; i < count; i++) {
		bad[i] = get64(block + 32 + i * 8);
		if (bad[i] < page || bad[i] >= erase || bad[i] % page ||
		    (i && bad[i] <= bad[i - 1]))
			goto bad_header;
	}
	idx->blocks++;
	for (pos = page; pos < erase; pos += page) {
		if (bad_page(pos, bad, count) || get64(block + pos) != ENTRY_MAGIC)
			continue;
		memcpy(header, block + pos, sizeof(header));
		entry.id = get32(header + 8);
		if (!entry.id || entry.id == UINT32_MAX)
			continue;
		length = get32(header + 12);
		if (length > erase - pos - HEADER_SIZE) {
			idx->corrupt++;
			continue;
		}
		entry.data = malloc(length ? length : 1);
		if (!entry.data)
			return -1;
		expected = get32(header + 32);
		memset(header + 32, 0, 4);
		crc = crc32_be(UINT32_MAX, header, sizeof(header));
		end = pos + HEADER_SIZE;
		used = 0;
		while (used < length && end < erase) {
			if (!(end % page) && bad_page(end, bad, count)) {
				end += page;
				continue;
			}
			n = page - end % page;
			if (n > length - used)
				n = length - used;
			memcpy(entry.data + used, block + end, n);
			crc = crc32_be(crc, block + end, n);
			used += n;
			end += n;
		}
		if (used != length || crc != expected) {
			idx->corrupt++;
			free(entry.data);
			continue;
		}
		entry.length = length;
		entry.revision = get32(header + 20);
		entry.segment = get32(header + 24);
		entry.next = get32(header + 28);
		entry.sequence = sequence;
		entry.offset = offset + pos;
		if (add_entry(idx, &entry)) {
			free(entry.data);
			return -1;
		}
		pos = (end + page - 1) / page * page - page;
	}
	return 0;

bad_header:
	fprintf(stderr, "Invalid bad-page table at 0x%" PRIx64 "\n", offset);
	return -1;
}

static int compare_entry(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (x->id != y->id)
		return x->id < y->id ? -1 : 1;
	if (x->revision != y->revision)
		return x->revision < y->revision ? -1 : 1;
	if (x->segment != y->segment)
		return x->segment < y->segment ? -1 : 1;
	/* Garbage collection can leave duplicate segments after a power loss. */
	if (x->sequence != y->sequence)
		return x->sequence > y->sequence ? -1 : 1;
	if (x->offset != y->offset)
		return x->offset > y->offset ? -1 : 1;
	return 0;
}

static size_t next_segment(const struct index *idx, size_t pos, size_t end)
{
	uint32_t segment = idx->entries[pos++].segment;

	while (pos < end && idx->entries[pos].segment == segment)
		pos++;
	return pos;
}

static bool complete_chain(const struct index *idx, size_t first, size_t end,
			   struct value *value)
{
	const struct entry *entry = &idx->entries[first];
	uint32_t expected = 0;
	size_t pos, length = 0;

	value->deleted = entry->segment == UINT32_MAX;
	if (value->deleted) {
		if (entry->next || next_segment(idx, first, end) != end)
			return false;
	} else {
		for (pos = first; pos < end; pos = next_segment(idx, pos, end)) {
			entry = &idx->entries[pos];
			if (entry->segment != expected || entry->length > MAX_DATA - length)
				return false;
			expected = entry->next;
			length += entry->length;
		}
		if (expected)
			return false;
	}
	value->first = first;
	value->end = end;
	value->length = length;
	return true;
}

static bool find_value(const struct index *idx, uint32_t id, struct value *value)
{
	struct value candidate;
	size_t first = 0, end = idx->count, middle;
	bool found = false;

	while (first < end) {
		middle = first + (end - first) / 2;
		if (idx->entries[middle].id < id)
			first = middle + 1;
		else
			end = middle;
	}
	for (; first < idx->count && idx->entries[first].id == id; first = end) {
		end = first + 1;
		while (end < idx->count && idx->entries[end].id == idx->entries[first].id &&
		       idx->entries[end].revision == idx->entries[first].revision)
			end++;
		if (!idx->entries[first].revision)
			continue;
		if (complete_chain(idx, first, end, &candidate)) {
			*value = candidate;
			found = true;
		}
	}
	return found;
}

static unsigned char *get_value(const struct index *idx, const struct value *value)
{
	unsigned char *data;
	size_t pos, used = 0;

	data = malloc(value->length + 1);
	if (!data)
		return NULL;
	for (pos = value->first; pos < value->end; pos = next_segment(idx, pos, value->end)) {
		memcpy(data + used, idx->entries[pos].data, idx->entries[pos].length);
		used += idx->entries[pos].length;
	}
	data[used] = 0;
	return data;
}

static int lookup_name(const struct index *idx, const char *name, uint32_t *id)
{
	struct value value;
	unsigned char *data, *end;
	size_t pos = 0, length;
	int ret = -1;

	if (!find_value(idx, NAMES_ID, &value) || value.deleted)
		return -1;
	data = get_value(idx, &value);
	if (!data)
		return -1;
	while (pos + 4 < value.length) {
		end = memchr(data + pos + 4, 0, value.length - pos - 4);
		if (!end)
			break;
		length = end - (data + pos + 4) + 1;
		if (!strcmp((char *)data + pos + 4, name)) {
			*id = get32(data + pos);
			ret = 0;
			break;
		}
		pos += 4 + ((length + 3) & ~(size_t)3);
	}
	free(data);
	return ret;
}

static int number(const char *text, uint32_t *value)
{
	char *end;
	unsigned long long n;

	errno = 0;
	if (!text[0] || text[0] == '-')
		return -1;
	n = strtoull(text, &end, 0);
	if (errno || *end || n > UINT32_MAX)
		return -1;
	*value = n;
	return 0;
}

static void usage(const char *name)
{
	fprintf(stderr, "Usage: %s [-e erase-size] [-p page-size] {-l|-n name|-r id} input\n"
		"  Read TFFS 3.1 without OOB; input is opened read-only.\n"
		"  Geometry is required for files/block devices; page size defaults to 4096.\n"
		"  -l lists record IDs, revisions and lengths without values.\n"
		"  -n prints a named value; -r extracts a record as raw bytes.\n", name);
}

int main(int argc, char **argv)
{
	struct index idx = { 0 };
	struct mtd_info_user mtd;
	struct stat st;
	struct value value;
	unsigned char *block = NULL, *data = NULL;
	uint32_t erase = 0, page = 0, id = 0;
	uint64_t size, offset;
	const char *name = NULL;
	int opt, action = 0, fd = -1, ret = EXIT_FAILURE;
	size_t i, length;

	while ((opt = getopt(argc, argv, "e:p:ln:r:")) != -1) {
		switch (opt) {
		case 'e':
			if (number(optarg, &erase) || !erase)
				goto invalid;
			break;
		case 'p':
			if (number(optarg, &page) || !page)
				goto invalid;
			break;
		case 'n':
			name = optarg;
			/* fall through */
		case 'r':
		case 'l':
			if (action || (opt == 'r' && number(optarg, &id)))
				goto invalid;
			action = opt;
			break;
		default:
			goto invalid;
		}
	}
	if (!action || argc - optind != 1)
		goto invalid;
	fd = open(argv[optind], O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st)) {
		perror(argv[optind]);
		goto out;
	}
	if (!ioctl(fd, MEMGETINFO, &mtd)) {
		size = mtd.size;
		if (!erase)
			erase = mtd.erasesize;
		if (!page)
			page = mtd.writesize > 4096 ? mtd.writesize : 4096;
	} else if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &size)) {
			perror("BLKGETSIZE64");
			goto out;
		}
	} else if (S_ISREG(st.st_mode)) {
		size = st.st_size;
	} else {
		fprintf(stderr, "Input must be a regular file, block device or MTD device\n");
		goto out;
	}
	if (!page)
		page = 4096;
	if (page < 4096 || page > MAX_ERASE_SIZE / 2 || (page & (page - 1)) ||
	    erase > MAX_ERASE_SIZE || erase < page * 2 || erase % page ||
	    !size || size > INT64_MAX || size % erase) {
		fprintf(stderr, "Invalid or missing geometry, or truncated input\n");
		goto out;
	}
	block = malloc(erase);
	if (!block)
		goto out;
	for (offset = 0; offset < size; offset += erase) {
		if (read_at(fd, block, erase, offset)) {
			fprintf(stderr, "Read failed at 0x%" PRIx64 "\n", offset);
			goto out;
		}
		if (scan_block(&idx, block, erase, page, offset))
			goto out;
	}
	if (!idx.blocks) {
		fprintf(stderr, "No supported TFFS blocks found\n");
		goto out;
	}
	if (idx.corrupt)
		fprintf(stderr, "Ignored %u corrupt records\n", idx.corrupt);
	if (idx.count)
		qsort(idx.entries, idx.count, sizeof(*idx.entries), compare_entry);
	if (action == 'l') {
		for (i = 0; i < idx.count; i++) {
			id = idx.entries[i].id;
			if (i && id == idx.entries[i - 1].id)
				continue;
			if (find_value(&idx, id, &value))
				printf("0x%08" PRIx32 "\t%" PRIu32 "\t%zu\t%s\n", id,
				       idx.entries[value.first].revision, value.length,
				       value.deleted ? "deleted" : "present");
		}
	} else {
		if ((name && lookup_name(&idx, name, &id)) ||
		    !find_value(&idx, id, &value) || value.deleted) {
			fprintf(stderr, "Record absent, deleted or incomplete\n");
			goto out;
		}
		data = get_value(&idx, &value);
		if (!data)
			goto out;
		length = value.length;
		if (name && length && !data[length - 1])
			length--;
		if (fwrite(data, 1, length, stdout) != length || (name && putchar('\n') == EOF))
			goto out;
	}
	if (fflush(stdout))
		goto out;
	ret = EXIT_SUCCESS;
	goto out;

invalid:
	usage(argv[0]);
out:
	for (i = 0; i < idx.count; i++)
		free(idx.entries[i].data);
	free(idx.entries);
	free(block);
	free(data);
	if (fd >= 0)
		close(fd);
	return ret;
}
