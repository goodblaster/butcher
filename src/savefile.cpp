/**
 * @file savefile.cpp
 *
 * MPQ access for Diablo save archives: strict validating reads, and writes
 * that rebuild the archive into a verified temporary file. See savefile.h.
 */
#include "savefile.h"

#include "../third_party/devilution/Source/encrypt.h"
#include "../third_party/devilution/3rdParty/PKWare/pkware.h"

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

/* The tables are consumed as raw little-endian images, exactly as the game
 * writes them. These three structs live outside any #pragma pack region and
 * happen to need no padding, so host and Win32 layouts agree. */
CT_ASSERT(sizeof(_FILEHEADER) == 104, filehdr_104);
CT_ASSERT(sizeof(_HASHENTRY) == 16, hashentry_16);
CT_ASSERT(sizeof(_BLOCKENTRY) == 16, blockentry_16);

struct MpqArchive {
	FILE *f;
	long filesize;
	_HASHENTRY *hash;
	_BLOCKENTRY *block;
};

static void seterr(char *err, const char *fmt, ...)
{
	if (err == NULL)
		return;
	va_list va;
	va_start(va, fmt);
	vsnprintf(err, MPQ_ERR_LEN, fmt, va);
	va_end(va);
}

static int host_is_little_endian(void)
{
	const DWORD probe = 1;
	return *(const BYTE *)&probe == 1;
}

/* ------------------------------------------------------------------ */
/* PKWare explode wrapper                                              */
/* ------------------------------------------------------------------ */

/*
 * Source/encrypt.cpp's PkwareDecompress() discards the output length, so a
 * short or over-long sector cannot be detected. This wrapper calls the same
 * shared explode() but keeps the length and bounds-checks every write, so a
 * corrupt archive is rejected rather than smashing the heap.
 */
typedef struct ExplodeCtx {
	const BYTE *src;
	DWORD src_len;
	DWORD src_pos;
	BYTE *dst;
	DWORD dst_cap;
	DWORD dst_pos;
	int overflow;
} ExplodeCtx;

static unsigned int explode_read(char *buf, unsigned int *size, void *param)
{
	ExplodeCtx *c = (ExplodeCtx *)param;
	DWORD avail = c->src_len - c->src_pos;
	DWORD take = *size < avail ? *size : avail;

	memcpy(buf, c->src + c->src_pos, take);
	c->src_pos += take;
	return take;
}

static void explode_write(char *buf, unsigned int *size, void *param)
{
	ExplodeCtx *c = (ExplodeCtx *)param;

	if (c->dst_pos + *size > c->dst_cap) {
		c->overflow = 1;
		return;
	}
	memcpy(c->dst + c->dst_pos, buf, *size);
	c->dst_pos += *size;
}

/** @return decompressed length, or -1 on failure. */
static long pk_explode(const BYTE *src, DWORD src_len, BYTE *dst, DWORD dst_cap)
{
	ExplodeCtx ctx;
	char *work = (char *)malloc(CMP_BUFFER_SIZE);

	if (work == NULL)
		return -1;

	ctx.src = src;
	ctx.src_len = src_len;
	ctx.src_pos = 0;
	ctx.dst = dst;
	ctx.dst_cap = dst_cap;
	ctx.dst_pos = 0;
	ctx.overflow = 0;

	explode(explode_read, explode_write, work, &ctx);
	free(work);

	if (ctx.overflow)
		return -1;
	return (long)ctx.dst_pos;
}

/* ------------------------------------------------------------------ */
/* Archive open                                                        */
/* ------------------------------------------------------------------ */

static int read_at(FILE *f, long off, void *buf, size_t len)
{
	if (fseek(f, off, SEEK_SET) != 0)
		return 0;
	return fread(buf, 1, len, f) == len;
}

MpqArchive *mpq_open(const char *path, char *err)
{
	_FILEHEADER hdr;
	MpqArchive *a;
	long size;

	if (!host_is_little_endian()) {
		seterr(err, "big-endian hosts are not supported");
		return NULL;
	}

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		seterr(err, "cannot open %s: %s", path, strerror(errno));
		return NULL;
	}

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
		seterr(err, "cannot determine size of %s", path);
		fclose(f);
		return NULL;
	}

	if (size < MPQ_DATA_OFFSET) {
		seterr(err, "too small to be a save archive (%ld bytes, need at least %d)",
		    size, MPQ_DATA_OFFSET);
		fclose(f);
		return NULL;
	}

	if (!read_at(f, 0, &hdr, sizeof(hdr))) {
		seterr(err, "cannot read MPQ header");
		fclose(f);
		return NULL;
	}

	/*
	 * Validate exactly what ParseMPQHeader validates -- but refuse instead
	 * of truncating. Each mismatch is reported specifically; "not an MPQ"
	 * and "an MPQ this tool cannot handle" are different problems for the
	 * person holding the save.
	 */
	if ((DWORD)hdr.signature != MPQ_SIGNATURE) {
		seterr(err, "not an MPQ archive (signature 0x%08X, expected 0x%08X)",
		    (unsigned)hdr.signature, MPQ_SIGNATURE);
		fclose(f);
		return NULL;
	}
	if (hdr.headersize != 32 || hdr.version != 0 || hdr.sectorsizeid != 3) {
		seterr(err, "unsupported MPQ variant (headersize=%d version=%u sectorsizeid=%d; "
		            "expected 32/0/3)",
		    hdr.headersize, (unsigned)hdr.version, hdr.sectorsizeid);
		fclose(f);
		return NULL;
	}
	if (hdr.blockoffset != MPQ_BLOCK_OFFSET || hdr.hashoffset != MPQ_HASH_OFFSET
	    || hdr.blockcount != MPQ_INDEX_ENTRIES || hdr.hashcount != MPQ_INDEX_ENTRIES) {
		/*
		 * A well-formed MPQ that is not a save. By far the likeliest cause is
		 * someone pointing the tool at the game's asset archive, so say that
		 * rather than reciting table offsets.
		 */
		if (size > 4 * 1024 * 1024) {
			seterr(err, "this is a game asset archive, not a character save "
			            "(%ld MB, %d files inside). Saves are named single_0.sv "
			            "or single_0.hsv and live beside Diablo.exe, or under "
			            "Application Support for DevilutionX.",
			    size / (1024 * 1024), hdr.blockcount);
		} else {
			seterr(err, "valid MPQ, but not a Diablo save: tables are at "
			            "block=%d/%d hash=%d/%d, where a save has 104/2048 and "
			            "32872/2048",
			    hdr.blockoffset, hdr.blockcount, hdr.hashoffset, hdr.hashcount);
		}
		fclose(f);
		return NULL;
	}
	if (hdr.filesize != (int)size) {
		seterr(err, "header filesize %d does not match actual size %ld -- refusing "
		            "(the game would silently truncate this file)",
		    hdr.filesize, size);
		fclose(f);
		return NULL;
	}

	a = (MpqArchive *)calloc(1, sizeof(*a));
	if (a == NULL) {
		fclose(f);
		seterr(err, "out of memory");
		return NULL;
	}
	a->f = f;
	a->filesize = size;
	a->block = (_BLOCKENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY));
	a->hash = (_HASHENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	if (a->block == NULL || a->hash == NULL) {
		seterr(err, "out of memory");
		mpq_close(a);
		return NULL;
	}

	InitHash();

	if (!read_at(f, MPQ_BLOCK_OFFSET, a->block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY))) {
		seterr(err, "cannot read block table");
		mpq_close(a);
		return NULL;
	}
	Decrypt((DWORD *)a->block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY),
	    Hash("(block table)", 3));

	if (!read_at(f, MPQ_HASH_OFFSET, a->hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY))) {
		seterr(err, "cannot read hash table");
		mpq_close(a);
		return NULL;
	}
	Decrypt((DWORD *)a->hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY),
	    Hash("(hash table)", 3));

	return a;
}

void mpq_close(MpqArchive *a)
{
	if (a == NULL)
		return;
	if (a->f != NULL)
		fclose(a->f);
	free(a->block);
	free(a->hash);
	free(a);
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

/* Mirrors mpqapi_get_hash_index (Source/mpqapi.cpp:257): linear probe from
 * Hash(name,0) & 0x7FF, stopping at an empty slot (block == -1), skipping
 * deleted ones (block == -2). */
static int find_hash_index(MpqArchive *a, const char *name)
{
	DWORD hash_a = Hash(name, 1);
	DWORD hash_b = Hash(name, 2);
	DWORD idx = Hash(name, 0) & 0x7FF;
	int budget = MPQ_INDEX_ENTRIES;

	for (; a->hash[idx].block != -1; idx = (idx + 1) & 0x7FF) {
		if (budget-- == 0)
			break;
		if ((DWORD)a->hash[idx].hashcheck[0] != hash_a)
			continue;
		if ((DWORD)a->hash[idx].hashcheck[1] != hash_b)
			continue;
		if (a->hash[idx].lcid != 0)
			continue;
		if (a->hash[idx].block == -2)
			continue;
		return (int)idx;
	}
	return -1;
}

int mpq_has_file(MpqArchive *a, const char *name)
{
	return find_hash_index(a, name) != -1;
}

/* ------------------------------------------------------------------ */
/* Read                                                                */
/* ------------------------------------------------------------------ */

BYTE *mpq_read_file(MpqArchive *a, const char *name, DWORD *out_len, char *err)
{
	int hidx = find_hash_index(a, name);
	if (hidx < 0) {
		seterr(err, "no file named \"%s\" in archive", name);
		return NULL;
	}

	int bidx = a->hash[hidx].block;
	if (bidx < 0 || bidx >= MPQ_INDEX_ENTRIES) {
		seterr(err, "\"%s\": hash entry points at block %d, out of range", name, bidx);
		return NULL;
	}

	_BLOCKENTRY *blk = &a->block[bidx];
	DWORD flags = (DWORD)blk->flags;

	if (!(flags & MPQ_FLAG_EXISTS)) {
		seterr(err, "\"%s\": block %d is not marked present (flags 0x%08X)",
		    name, bidx, (unsigned)flags);
		return NULL;
	}
	if (flags & MPQ_FLAG_ENCRYPTED) {
		seterr(err, "\"%s\": per-file encryption is not supported (flags 0x%08X)",
		    name, (unsigned)flags);
		return NULL;
	}
	if (flags & MPQ_FLAG_SINGLEUNIT) {
		seterr(err, "\"%s\": single-unit files are not supported (flags 0x%08X)",
		    name, (unsigned)flags);
		return NULL;
	}
	if (!(flags & MPQ_FLAG_IMPLODE)) {
		seterr(err, "\"%s\": expected PKWare-imploded storage (flags 0x%08X)",
		    name, (unsigned)flags);
		return NULL;
	}

	if (blk->sizefile <= 0) {
		seterr(err, "\"%s\": empty or negative file size (%d)", name, blk->sizefile);
		return NULL;
	}
	if (blk->offset < MPQ_DATA_OFFSET || blk->sizealloc < 0
	    || (long)blk->offset + blk->sizealloc > a->filesize) {
		seterr(err, "\"%s\": block region [%d,%d) lies outside the file (size %ld)",
		    name, blk->offset, blk->offset + blk->sizealloc, a->filesize);
		return NULL;
	}

	DWORD file_len = (DWORD)blk->sizefile;
	DWORD nsectors = (file_len + MPQ_SECTOR_SIZE - 1) / MPQ_SECTOR_SIZE;
	DWORD table_bytes = (nsectors + 1) * sizeof(DWORD);

	if (table_bytes > (DWORD)blk->sizealloc) {
		seterr(err, "\"%s\": sector table does not fit in the allocated block", name);
		return NULL;
	}

	DWORD *offs = (DWORD *)malloc(table_bytes);
	if (offs == NULL) {
		seterr(err, "out of memory");
		return NULL;
	}
	if (!read_at(a->f, blk->offset, offs, table_bytes)) {
		seterr(err, "\"%s\": cannot read sector offset table", name);
		free(offs);
		return NULL;
	}

	/* The sector table is not encrypted -- mpqapi never sets MPQ_FLAG_ENCRYPTED. */
	if (offs[0] != table_bytes) {
		seterr(err, "\"%s\": first sector offset is %u, expected %u",
		    name, (unsigned)offs[0], (unsigned)table_bytes);
		free(offs);
		return NULL;
	}
	for (DWORD i = 0; i < nsectors; i++) {
		if (offs[i + 1] < offs[i]) {
			seterr(err, "\"%s\": sector offsets are not monotonic at %u", name, (unsigned)i);
			free(offs);
			return NULL;
		}
	}
	if (offs[nsectors] > (DWORD)blk->sizealloc) {
		seterr(err, "\"%s\": sector data (%u bytes) overruns the allocated block (%d)",
		    name, (unsigned)offs[nsectors], blk->sizealloc);
		free(offs);
		return NULL;
	}

	BYTE *out = (BYTE *)malloc(file_len);
	BYTE *sector = (BYTE *)malloc(MPQ_SECTOR_SIZE * 2);
	if (out == NULL || sector == NULL) {
		seterr(err, "out of memory");
		free(offs);
		free(out);
		free(sector);
		return NULL;
	}

	DWORD written = 0;
	for (DWORD i = 0; i < nsectors; i++) {
		DWORD stored = offs[i + 1] - offs[i];
		DWORD raw = file_len - written;
		if (raw > MPQ_SECTOR_SIZE)
			raw = MPQ_SECTOR_SIZE;

		if (stored == 0 || stored > MPQ_SECTOR_SIZE * 2) {
			seterr(err, "\"%s\": sector %u has implausible stored size %u",
			    name, (unsigned)i, (unsigned)stored);
			goto fail;
		}
		if (!read_at(a->f, blk->offset + offs[i], sector, stored)) {
			seterr(err, "\"%s\": cannot read sector %u", name, (unsigned)i);
			goto fail;
		}

		if (stored == raw) {
			/*
			 * PkwareCompress (Source/encrypt.cpp:103) keeps the imploded
			 * output only when it is smaller, so a sector stored at full
			 * size is literal. This is the standard MPQ IMPLODE rule.
			 */
			memcpy(out + written, sector, raw);
		} else {
			long got = pk_explode(sector, stored, out + written, raw);
			if (got < 0) {
				seterr(err, "\"%s\": sector %u failed to decompress", name, (unsigned)i);
				goto fail;
			}
			if ((DWORD)got != raw) {
				seterr(err, "\"%s\": sector %u decompressed to %ld bytes, expected %u",
				    name, (unsigned)i, got, (unsigned)raw);
				goto fail;
			}
		}
		written += raw;
	}

	free(offs);
	free(sector);
	*out_len = file_len;
	return out;

fail:
	free(offs);
	free(out);
	free(sector);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Write                                                               */
/* ------------------------------------------------------------------ */

/**
 * Build the on-disk body of a file: a sector offset table followed by
 * 4096-byte sectors, each imploded unless implosion made it bigger.
 *
 * Mirrors mpqapi_write_file_contents (Source/mpqapi.cpp:444). PkwareCompress
 * is the game's own routine, so a sector recompressed here is byte-identical
 * to what the game would have written for the same input.
 *
 * @return malloc'd body, or NULL. *body_len receives its length.
 */
static BYTE *build_file_body(const BYTE *data, DWORD len, DWORD *body_len, char *err)
{
	DWORD nsectors = (len + MPQ_SECTOR_SIZE - 1) / MPQ_SECTOR_SIZE;
	DWORD table_bytes = (nsectors + 1) * sizeof(DWORD);

	/* Worst case: every sector stays literal. */
	BYTE *body = (BYTE *)malloc(table_bytes + nsectors * MPQ_SECTOR_SIZE);
	if (body == NULL) {
		seterr(err, "out of memory");
		return NULL;
	}

	DWORD *offs = (DWORD *)body;
	DWORD pos = table_bytes;
	BYTE sector[MPQ_SECTOR_SIZE];

	for (DWORD s = 0; s < nsectors; s++) {
		DWORD raw = len - s * MPQ_SECTOR_SIZE;
		if (raw > MPQ_SECTOR_SIZE)
			raw = MPQ_SECTOR_SIZE;

		memcpy(sector, data + s * MPQ_SECTOR_SIZE, raw);
		int stored = PkwareCompress(sector, (int)raw);
		if (stored <= 0 || (DWORD)stored > raw) {
			seterr(err, "sector %u compressed to an invalid size %d", (unsigned)s, stored);
			free(body);
			return NULL;
		}

		offs[s] = pos;
		memcpy(body + pos, sector, (size_t)stored);
		pos += (DWORD)stored;
	}
	offs[nsectors] = pos;

	*body_len = pos;
	return body;
}

typedef struct LiveBlock {
	int old_index;
	_BLOCKENTRY blk;
} LiveBlock;

static int cmp_live(const void *a, const void *b)
{
	int oa = ((const LiveBlock *)a)->blk.offset;
	int ob = ((const LiveBlock *)b)->blk.offset;
	return (oa > ob) - (oa < ob);
}

static int write_all(FILE *f, const void *buf, size_t len)
{
	return fwrite(buf, 1, len, f) == len;
}

int mpq_replace_file(const char *path, const char *name, const BYTE *data,
    DWORD len, char *err)
{
	MpqArchive *a = NULL;
	_BLOCKENTRY *newblk = NULL;
	_HASHENTRY *newhash = NULL;
	LiveBlock *live = NULL;
	BYTE *body = NULL;
	BYTE *copybuf = NULL;
	char *tmppath = NULL;
	FILE *out = NULL;
	int ok = 0;

	/* Declared up front: every failure path jumps to `out`, and C++ forbids
	 * a goto that bypasses an initialization still in scope at the label. */
	int target_hash, target_block, nlive, i, h;
	DWORD body_len, cursor;
	int remap[MPQ_INDEX_ENTRIES];

	a = mpq_open(path, err);
	if (a == NULL)
		return 0;

	target_hash = find_hash_index(a, name);
	if (target_hash < 0) {
		seterr(err, "\"%s\" does not exist in the archive (this tool replaces "
		            "files, it does not add them)",
		    name);
		goto out;
	}
	target_block = a->hash[target_hash].block;

	body_len = 0;
	body = build_file_body(data, len, &body_len, err);
	if (body == NULL)
		goto out;

	/* Collect live blocks, in their original on-disk order so an unchanged
	 * archive rewrites to the same layout. */
	live = (LiveBlock *)calloc(MPQ_INDEX_ENTRIES, sizeof(LiveBlock));
	newblk = (_BLOCKENTRY *)calloc(MPQ_INDEX_ENTRIES, sizeof(_BLOCKENTRY));
	newhash = (_HASHENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	if (live == NULL || newblk == NULL || newhash == NULL) {
		seterr(err, "out of memory");
		goto out;
	}

	nlive = 0;
	{
		char seen[MPQ_INDEX_ENTRIES];
		memset(seen, 0, sizeof(seen));
		for (h = 0; h < MPQ_INDEX_ENTRIES; h++) {
			int b = a->hash[h].block;
			if (b < 0 || b >= MPQ_INDEX_ENTRIES)
				continue;
			if (!((DWORD)a->block[b].flags & MPQ_FLAG_EXISTS))
				continue;
			if (seen[b])
				continue;
			seen[b] = 1;

			_BLOCKENTRY *src = &a->block[b];
			if (src->offset < MPQ_DATA_OFFSET || src->sizealloc < 0
			    || (long)src->offset + src->sizealloc > a->filesize) {
				seterr(err, "block %d lies outside the file; refusing to rewrite "
				            "a malformed archive",
				    b);
				goto out;
			}
			live[nlive].old_index = b;
			live[nlive].blk = *src;
			nlive++;
		}
	}
	qsort(live, nlive, sizeof(LiveBlock), cmp_live);

	/* Lay the data region out sequentially. */
	for (i = 0; i < MPQ_INDEX_ENTRIES; i++)
		remap[i] = -1;

	cursor = MPQ_DATA_OFFSET;
	for (i = 0; i < nlive; i++) {
		DWORD size = (live[i].old_index == target_block)
		    ? body_len
		    : (DWORD)live[i].blk.sizealloc;

		newblk[i].offset = (int)cursor;
		newblk[i].sizealloc = (int)size;
		if (live[i].old_index == target_block) {
			newblk[i].sizefile = (int)len;
			newblk[i].flags = (int)(MPQ_FLAG_EXISTS | MPQ_FLAG_IMPLODE);
		} else {
			newblk[i].sizefile = live[i].blk.sizefile;
			newblk[i].flags = live[i].blk.flags;
		}
		remap[live[i].old_index] = i;
		cursor += size;
	}

	/*
	 * Preserve the hash table slot for slot. Deleted entries (block == -2)
	 * must survive: dropping one would shorten a probe chain and make an
	 * unrelated file unreachable.
	 */
	memcpy(newhash, a->hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	for (h = 0; h < MPQ_INDEX_ENTRIES; h++) {
		int b = newhash[h].block;
		if (b < 0)
			continue; /* -1 empty, -2 deleted */
		if (b >= MPQ_INDEX_ENTRIES || remap[b] < 0) {
			/* Referenced a block that is not live. Mark deleted rather
			 * than empty, so the probe chain keeps its length. */
			newhash[h].block = -2;
			continue;
		}
		newhash[h].block = remap[b];
	}

	/* Write to a temporary file beside the target. */
	{
		size_t n = strlen(path) + 16;
		tmppath = (char *)malloc(n);
		if (tmppath == NULL) {
			seterr(err, "out of memory");
			goto out;
		}
		snprintf(tmppath, n, "%s.tmpXXXXXX", path);
		int fd = mkstemp(tmppath);
		if (fd < 0) {
			seterr(err, "cannot create temporary file beside %s: %s", path,
			    strerror(errno));
			free(tmppath);
			tmppath = NULL;
			goto out;
		}
		/*
		 * mkstemp creates at 0600 and rename() carries that mode onto the
		 * target, so without this the save silently loses its permissions.
		 */
		struct stat st;
		if (stat(path, &st) == 0)
			(void)fchmod(fd, st.st_mode & 07777);

		out = fdopen(fd, "wb");
		if (out == NULL) {
			seterr(err, "cannot open temporary file: %s", strerror(errno));
			close(fd);
			goto out;
		}
	}

	{
		_FILEHEADER hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.signature = (int)MPQ_SIGNATURE;
		hdr.headersize = 32;
		hdr.filesize = (int)cursor;
		hdr.version = 0;
		hdr.sectorsizeid = 3;
		hdr.hashoffset = MPQ_HASH_OFFSET;
		hdr.blockoffset = MPQ_BLOCK_OFFSET;
		hdr.hashcount = MPQ_INDEX_ENTRIES;
		hdr.blockcount = MPQ_INDEX_ENTRIES;

		Encrypt((DWORD *)newblk, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY),
		    Hash("(block table)", 3));
		Encrypt((DWORD *)newhash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY),
		    Hash("(hash table)", 3));

		int wrote = write_all(out, &hdr, sizeof(hdr))
		    && write_all(out, newblk, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY))
		    && write_all(out, newhash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));

		/* Decrypt back so the in-memory copies stay usable if needed. */
		Decrypt((DWORD *)newblk, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY),
		    Hash("(block table)", 3));
		Decrypt((DWORD *)newhash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY),
		    Hash("(hash table)", 3));

		if (!wrote) {
			seterr(err, "short write to temporary file");
			goto out;
		}
	}

	copybuf = (BYTE *)malloc(MPQ_SECTOR_SIZE);
	if (copybuf == NULL) {
		seterr(err, "out of memory");
		goto out;
	}
	for (i = 0; i < nlive; i++) {
		if (live[i].old_index == target_block) {
			if (!write_all(out, body, body_len)) {
				seterr(err, "short write writing \"%s\"", name);
				goto out;
			}
			continue;
		}
		/* Copy the untouched file's compressed bytes verbatim. */
		DWORD remaining = (DWORD)live[i].blk.sizealloc;
		long src = live[i].blk.offset;
		while (remaining > 0) {
			DWORD chunk = remaining < MPQ_SECTOR_SIZE ? remaining : MPQ_SECTOR_SIZE;
			if (!read_at(a->f, src, copybuf, chunk)) {
				seterr(err, "cannot re-read block at %ld while copying", src);
				goto out;
			}
			if (!write_all(out, copybuf, chunk)) {
				seterr(err, "short write copying an untouched file");
				goto out;
			}
			src += chunk;
			remaining -= chunk;
		}
	}

	if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
		seterr(err, "cannot flush temporary file: %s", strerror(errno));
		goto out;
	}
	if (fclose(out) != 0) {
		out = NULL;
		seterr(err, "cannot close temporary file: %s", strerror(errno));
		goto out;
	}
	out = NULL;

	/*
	 * Verify before replacing anything. Re-open the temporary archive and
	 * read the file back; if it does not match what we meant to write, the
	 * original is left alone and the temp is discarded.
	 */
	{
		char verr[MPQ_ERR_LEN] = { 0 };
		MpqArchive *v = mpq_open(tmppath, verr);
		if (v == NULL) {
			seterr(err, "wrote a file that will not re-open (%s); original left "
			            "untouched",
			    verr);
			goto out;
		}
		DWORD got_len = 0;
		BYTE *got = mpq_read_file(v, name, &got_len, verr);
		mpq_close(v);
		if (got == NULL) {
			seterr(err, "wrote a file whose \"%s\" will not read back (%s); "
			            "original left untouched",
			    name, verr);
			goto out;
		}
		int same = (got_len == len) && memcmp(got, data, len) == 0;
		free(got);
		if (!same) {
			seterr(err, "verification failed: \"%s\" read back differently; "
			            "original left untouched",
			    name);
			goto out;
		}
	}

	if (rename(tmppath, path) != 0) {
		seterr(err, "cannot replace %s: %s", path, strerror(errno));
		goto out;
	}
	free(tmppath);
	tmppath = NULL;
	ok = 1;

out:
	if (out != NULL)
		fclose(out);
	if (tmppath != NULL) {
		remove(tmppath);
		free(tmppath);
	}
	mpq_close(a);
	free(body);
	free(copybuf);
	free(live);
	free(newblk);
	free(newhash);
	return ok;
}

/* ------------------------------------------------------------------ */
/* hero                                                                */
/* ------------------------------------------------------------------ */

int save_write_hero(const char *path, const PkPlayerStruct *hero, char *err)
{
	DWORD plain = (DWORD)sizeof(*hero);
	DWORD enc = codec_get_encoded_len(plain);

	BYTE *buf = (BYTE *)calloc(1, enc);
	if (buf == NULL) {
		seterr(err, "out of memory");
		return 0;
	}
	memcpy(buf, hero, plain);
	codec_encode(buf, plain, enc, SAVE_PASSWORD_SINGLE);

	int r = mpq_replace_file(path, "hero", buf, enc, err);
	free(buf);
	return r;
}

int save_create(const char *path, const PkPlayerStruct *h, char *err)
{
	/* "x" so an existing character is never silently replaced. */
	FILE *f = fopen(path, "wbx");
	if (f == NULL) {
		seterr(err, "cannot create %s: %s", path, strerror(errno));
		return 0;
	}

	BYTE blob[1288];
	memset(blob, 0, sizeof(blob));
	memcpy(blob, h, sizeof(*h));
	codec_encode(blob, sizeof(*h), sizeof(blob), SAVE_PASSWORD_SINGLE);

	_BLOCKENTRY *block = (_BLOCKENTRY *)calloc(MPQ_INDEX_ENTRIES, sizeof(_BLOCKENTRY));
	_HASHENTRY *hash = (_HASHENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	if (block == NULL || hash == NULL) {
		free(block);
		free(hash);
		fclose(f);
		remove(path);
		seterr(err, "out of memory");
		return 0;
	}
	memset(hash, 0xFF, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	InitHash();

	/* One sector: 1288 bytes is well under MPQ_SECTOR_SIZE. */
	BYTE body[MPQ_SECTOR_SIZE * 2];
	DWORD *offs = (DWORD *)body;
	BYTE sector[MPQ_SECTOR_SIZE];
	memcpy(sector, blob, sizeof(blob));
	int stored = PkwareCompress(sector, (int)sizeof(blob));
	offs[0] = 8;
	offs[1] = 8 + (DWORD)stored;
	memcpy(body + 8, sector, (size_t)stored);

	block[0].offset = MPQ_DATA_OFFSET;
	block[0].sizealloc = (int)offs[1];
	block[0].sizefile = (int)sizeof(blob);
	block[0].flags = (int)(MPQ_FLAG_EXISTS | MPQ_FLAG_IMPLODE);

	DWORD idx = Hash("hero", 0) & (MPQ_INDEX_ENTRIES - 1);
	hash[idx].hashcheck[0] = (int)Hash("hero", 1);
	hash[idx].hashcheck[1] = (int)Hash("hero", 2);
	hash[idx].lcid = 0;
	hash[idx].block = 0;

	_FILEHEADER hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.signature = (int)MPQ_SIGNATURE;
	hdr.headersize = 32;
	hdr.filesize = (int)(MPQ_DATA_OFFSET + offs[1]);
	hdr.sectorsizeid = 3;
	hdr.hashoffset = MPQ_HASH_OFFSET;
	hdr.blockoffset = MPQ_BLOCK_OFFSET;
	hdr.hashcount = MPQ_INDEX_ENTRIES;
	hdr.blockcount = MPQ_INDEX_ENTRIES;

	Encrypt((DWORD *)block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY),
	    Hash("(block table)", 3));
	Encrypt((DWORD *)hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY),
	    Hash("(hash table)", 3));

	int ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1
	    && fwrite(block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), 1, f) == 1
	    && fwrite(hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), 1, f) == 1
	    && fwrite(body, offs[1], 1, f) == 1;
	free(block);
	free(hash);
	if (fclose(f) != 0)
		ok = 0;

	if (!ok) {
		remove(path);
		seterr(err, "short write creating %s", path);
		return 0;
	}

	/* Never leave behind something that does not read back. */
	PkPlayerStruct back;
	if (!save_read_hero(path, &back, NULL, err)) {
		remove(path);
		return 0;
	}
	if (memcmp(&back, h, sizeof(back)) != 0) {
		remove(path);
		seterr(err, "the new save did not read back as written");
		return 0;
	}
	return 1;
}

int save_has_game(const char *path)
{
	char err[MPQ_ERR_LEN];
	MpqArchive *a = mpq_open(path, err);
	if (a == NULL)
		return 0;
	int has = mpq_has_file(a, "game");
	mpq_close(a);
	return has;
}

int save_read_hero(const char *path, PkPlayerStruct *out, int *used_multi, char *err)
{
	if (used_multi != NULL)
		*used_multi = 0;

	MpqArchive *a = mpq_open(path, err);
	if (a == NULL)
		return 0;

	DWORD len = 0;
	BYTE *buf = mpq_read_file(a, "hero", &len, err);
	mpq_close(a);
	if (buf == NULL)
		return 0;

	DWORD want = codec_get_encoded_len(sizeof(PkPlayerStruct));
	if (len != want) {
		seterr(err, "\"hero\" is %u bytes, expected %u", (unsigned)len, (unsigned)want);
		free(buf);
		return 0;
	}

	/* Single-player first; fall back to the multiplayer password so a
	 * misidentified save reports the useful error rather than "corrupt". */
	BYTE *copy = (BYTE *)malloc(len);
	if (copy == NULL) {
		seterr(err, "out of memory");
		free(buf);
		return 0;
	}
	memcpy(copy, buf, len);

	int decoded = codec_decode(buf, len, SAVE_PASSWORD_SINGLE);
	if (decoded == 0) {
		memcpy(buf, copy, len);
		decoded = codec_decode(buf, len, SAVE_PASSWORD_MULTI);
		if (decoded != 0 && used_multi != NULL)
			*used_multi = 1;
	}
	free(copy);

	if (decoded == 0) {
		seterr(err, "\"hero\" failed to decode with either password -- the save is "
		            "corrupt, or this is a spawn/Hellfire archive");
		free(buf);
		return 0;
	}
	if (decoded != (int)sizeof(PkPlayerStruct)) {
		seterr(err, "\"hero\" decoded to %d bytes, expected %zu",
		    decoded, sizeof(PkPlayerStruct));
		free(buf);
		return 0;
	}

	memcpy(out, buf, sizeof(*out));
	free(buf);
	return 1;
}
