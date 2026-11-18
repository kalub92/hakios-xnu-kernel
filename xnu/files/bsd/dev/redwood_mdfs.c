/*
 * The modem's file server (r1_md=5, docs/cellular-stage2.md 2.1, 4/2c): the FS channel's requests (ccci_fsd's protocol, recovered
 * from ccci_mdinit) answered from a READ-ONLY ext4 reader over the r1's own NV partitions (nvdata, nvcfg, protect1, protect2),
 * with an in-RAM overlay for everything the modem writes, creates, deletes or renames.
 *
 * The rules this file is built on:
 *   - There is no write path to the eMMC here: the only calls out are redwood_emmc_nv_find (the GPT's entry) and
 *     redwood_emmc_nv_read (blocks in). redwood_emmc.c additionally refuses any write to those partitions.
 *   - Nothing read is ever logged: names, sizes, offsets, op codes and result codes only. No hex dumps of payloads.
 *   - The overlay is kalloc'd, copy-on-write per file, capped (OV_CAP) and gone when the run ends.
 * Single thread (the md thread), so the buffers below are static.
 */
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/redwood_emmc.h>
#include <sys/systm.h>

extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern uint32_t redwood_host_keep_len;

#include <libkern/crc.h>
#define SAY(...) redwood_host_keep_add(__VA_ARGS__)
/* the kept buffer drops what does not fit (64 KB, shared with everything else): leave room for the summary and the RESULT line */
#define KEEP_ROOM (0xff00u - 30000u)
#define FSAY(...) do { if (redwood_host_keep_len < KEEP_ROOM) redwood_host_keep_add(__VA_ARGS__); } while (0)

#define BS          4096u
#define OV_MAX      256u
#define OV_CAP      (16u << 20)
#define NFH         128
#define NOPS        0x26
#define FS_MAX_READ 0x4000u     /* a reply must stay under the stream limit 0x40c8; a READ/CMPT_READ is cut to this */
#define LOG_FULL    400u

static inline uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t le16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* ---------------------------------------------------------------- the ext4 reader (read only) */

struct m4 {
	const char *name;
	uint64_t lba;
	uint32_t bpg, ipg, isz, descsz, ngroups, first_data, nblocks;
	uint8_t *gdt;
	char jnl;               /* c clean, r needs_recovery set but the journal is empty, R needs_recovery and transactions in it, ? unknown, - not mounted */
	bool ok;
};
static struct m4 fs[4] = { { .name = "nvdata", .jnl = '-' }, { .name = "nvcfg", .jnl = '-' }, { .name = "protect1", .jnl = '-' },
	                   { .name = "protect2", .jnl = '-' } };

struct ino { uint16_t mode; uint32_t flags; uint64_t size; uint8_t blk[60]; };

#define BC_N 128
static uint8_t *bc_mem;
static uint32_t bc_tag[BC_N];
static uint8_t tmpb[BS], dbuf[BS], fbuf[BS], ext_node[5][BS];
static uint8_t *walkbuf;

static bool
rd_blk(int m, uint32_t blk, void *dst)
{
	uint32_t tag = ((uint32_t)(m + 1) << 28) | blk, slot = (blk ^ ((uint32_t)m * 37u)) % BC_N;
	uint8_t *c = bc_mem + slot * BS;

	if (blk >= fs[m].nblocks) {
		return false;
	}
	if (bc_tag[slot] != tag) {
		bc_tag[slot] = 0;
		if (!redwood_emmc_nv_read(fs[m].lba + (uint64_t)blk * 8, 8, c)) {
			return false;
		}
		bc_tag[slot] = tag;
	}
	memcpy(dst, c, BS);
	return true;
}

static bool
get_inode(int m, uint32_t n, struct ino *o)
{
	struct m4 *f = &fs[m];
	uint32_t g, idx, lo, hi = 0;
	uint64_t off;
	const uint8_t *d, *p;

	if (n == 0 || n > f->ipg * f->ngroups) {
		return false;
	}
	g = (n - 1) / f->ipg;
	idx = (n - 1) % f->ipg;
	d = f->gdt + (uint64_t)g * f->descsz;
	lo = le32(d + 8);
	if (f->descsz >= 64) {
		hi = le32(d + 0x28);
	}
	off = ((uint64_t)hi << 32 | lo) * BS + (uint64_t)idx * f->isz;
	if (off / BS >= f->nblocks || !rd_blk(m, (uint32_t)(off / BS), tmpb)) {
		return false;
	}
	p = tmpb + off % BS;
	o->mode = (uint16_t)le16(p);
	o->flags = le32(p + 0x20);
	o->size = le32(p + 4);
	if ((o->mode & 0xf000) == 0x8000) {
		o->size |= (uint64_t)le32(p + 0x6c) << 32;
	}
	memcpy(o->blk, p + 0x28, 60);
	return true;
}

/* the extent tree of an inode (any depth <= 4); 1 mapped, 0 a hole (or an unwritten extent: zeros), -1 error */
static int
bmap(int m, const struct ino *in, uint32_t lblk, uint64_t *pblk)
{
	const uint8_t *node = in->blk;

	if (!(in->flags & 0x80000)) {
		return -1;      /* block-mapped file: not supported */
	}
	for (int lvl = 0; lvl < 6; lvl++) {
		uint32_t ent = le16(node + 2), depth = le16(node + 6);
		int pick = -1;

		if (le16(node) != 0xf30a || lvl > 4) {
			return -1;
		}
		if (depth == 0) {
			for (uint32_t i = 0; i < ent; i++) {
				const uint8_t *e = node + 12 + 12 * i;
				uint32_t fb = le32(e), len = le16(e + 4);
				bool unwritten = len > 32768;

				if (unwritten) {
					len -= 32768;
				}
				if (lblk >= fb && lblk < fb + len) {
					if (unwritten) {
						return 0;
					}
					*pblk = ((uint64_t)le16(e + 6) << 32 | le32(e + 8)) + (lblk - fb);
					return 1;
				}
			}
			return 0;
		}
		for (uint32_t i = 0; i < ent; i++) {
			if (le32(node + 12 + 12 * i) <= lblk) {
				pick = (int)i;
			} else {
				break;
			}
		}
		if (pick < 0) {
			return 0;
		}
		{
			const uint8_t *e = node + 12 + 12 * pick;
			uint64_t leaf = le32(e + 4) | (uint64_t)le16(e + 8) << 32;

			if (leaf >= fs[m].nblocks || !rd_blk(m, (uint32_t)leaf, ext_node[lvl])) {
				return -1;
			}
			node = ext_node[lvl];
		}
	}
	return -1;
}

/* bytes of a file into dst: the length read, or -1 */
static int64_t
file_read(int m, const struct ino *in, uint64_t off, uint32_t len, uint8_t *dst)
{
	uint32_t done = 0;

	if (off >= in->size) {
		return 0;
	}
	if (len > in->size - off) {
		len = (uint32_t)(in->size - off);
	}
	while (done < len) {
		uint64_t pb = 0, at = off + done;
		uint32_t bo = (uint32_t)(at % BS), n = BS - bo;
		int r = bmap(m, in, (uint32_t)(at / BS), &pb);

		if (n > len - done) {
			n = len - done;
		}
		if (r < 0) {
			return -1;
		}
		if (r == 0) {
			memset(dst + done, 0, n);
		} else {
			if (pb >= fs[m].nblocks || !rd_blk(m, (uint32_t)pb, fbuf)) {
				return -1;
			}
			memcpy(dst + done, fbuf + bo, n);
		}
		done += n;
	}
	return done;
}

/* a directory's entries, linear (an htree's index blocks are fake empty entries; its leaves are plain) */
struct dit { struct ino dir; int m; uint32_t lb, off, nlb; uint8_t *buf; bool have; };

static void
dit_open(struct dit *it, int m, const struct ino *dir, uint8_t *buf)
{
	it->dir = *dir;
	it->m = m;
	it->lb = it->off = 0;
	it->nlb = (uint32_t)((dir->size + BS - 1) / BS);
	it->buf = buf;
	it->have = false;
}

static bool
dit_next(struct dit *it, uint32_t *ino, char *name, uint32_t namesz)
{
	for (;;) {
		uint32_t in, rl, nl;
		const uint8_t *e;

		if (!it->have) {
			uint64_t pb = 0;
			int r;

			if (it->lb >= it->nlb) {
				return false;
			}
			r = bmap(it->m, &it->dir, it->lb, &pb);
			if (r <= 0 || !rd_blk(it->m, (uint32_t)pb, it->buf)) {
				it->lb++;
				continue;
			}
			it->have = true;
			it->off = 0;
		}
		if (it->off + 8 > BS) {
			it->have = false;
			it->lb++;
			continue;
		}
		e = it->buf + it->off;
		in = le32(e);
		rl = le16(e + 4);
		nl = e[6];
		if (rl < 8 || (rl & 3) || it->off + rl > BS || 8 + nl > rl) {
			it->have = false;
			it->lb++;
			continue;
		}
		it->off += rl;
		if (in == 0) {
			continue;
		}
		if (nl >= namesz) {
			nl = namesz - 1;
		}
		memcpy(name, e + 8, nl);
		name[nl] = 0;
		*ino = in;
		return true;
	}
}

/* relative path (no leading slash) from the root: 1 found, 0 the last component missing, -1 an earlier one missing / not a directory */
static int
path_lookup(int m, const char *path, struct ino *out, uint32_t *inum)
{
	struct ino cur;
	uint32_t ci = 2;

	if (!get_inode(m, 2, &cur)) {
		return -1;
	}
	while (*path) {
		char comp[128], name[128];
		uint32_t n = 0, found = 0, in;
		struct dit it;
		bool last, hit = false;

		while (*path == '/') {
			path++;
		}
		if (!*path) {
			break;
		}
		while (path[n] && path[n] != '/' && n < sizeof(comp) - 1) {
			comp[n] = path[n];
			n++;
		}
		comp[n] = 0;
		path += n;
		while (*path == '/') {
			path++;
		}
		last = !*path;
		if ((cur.mode & 0xf000) != 0x4000) {
			return -1;
		}
		dit_open(&it, m, &cur, dbuf);
		while (dit_next(&it, &in, name, sizeof(name))) {
			if (!strcmp(name, comp)) {
				found = in;
				hit = true;
				break;
			}
		}
		if (!hit) {
			return last ? 0 : -1;
		}
		ci = found;
		if (!get_inode(m, ci, &cur)) {
			return -1;
		}
	}
	*out = cur;
	if (inum) {
		*inum = ci;
	}
	return 1;
}

static bool
mount_one(int m)
{
	struct m4 *f = &fs[m];
	uint64_t blocks = 0, total;
	uint8_t *buf = kalloc_data(BS, Z_WAITOK | Z_ZERO);
	const uint8_t *sb;
	uint32_t incompat, gdt_blocks, features_ok = 0x2 | 0x4 | 0x40 | 0x80 | 0x200;
	bool ok = false;

	if (buf == NULL) {
		return false;
	}
	sb = buf + 1024;
	if (!redwood_emmc_nv_find(f->name, &f->lba, &blocks)) {
		SAY("r1md: ext4 %s: no such partition in the GPT", f->name);
		goto out;
	}
	if (!redwood_emmc_nv_read(f->lba, 8, buf) || le16(sb + 0x38) != 0xef53) {
		SAY("r1md: ext4 %s: at block %llu: no ext4 superblock", f->name, f->lba);
		goto out;
	}
	incompat = le32(sb + 0x60);
	if (le32(sb + 0x18) != 2 || (incompat & ~features_ok)) {
		SAY("r1md: ext4 %s: UNSUPPORTED (block size log %u, incompat %#x)", f->name, le32(sb + 0x18), incompat);
		goto out;
	}
	f->bpg = le32(sb + 0x20);
	f->ipg = le32(sb + 0x28);
	f->isz = le16(sb + 0x58);
	f->first_data = le32(sb + 0x14);
	f->descsz = (incompat & 0x80) ? le16(sb + 0xfe) : 32;
	total = le32(sb + 4) | ((incompat & 0x80) ? (uint64_t)le32(sb + 0x150) << 32 : 0);
	if (total > blocks / 8) {
		total = blocks / 8;
	}
	f->nblocks = (uint32_t)total;
	if (!f->bpg || !f->ipg || f->isz < 128 || f->isz > BS || f->descsz < 32 || f->descsz > 128) {
		SAY("r1md: ext4 %s: bad geometry", f->name);
		goto out;
	}
	f->ngroups = (uint32_t)((total - f->first_data + f->bpg - 1) / f->bpg);
	gdt_blocks = (f->ngroups * f->descsz + BS - 1) / BS;
	if (gdt_blocks > 64 || !(f->gdt = kalloc_data(gdt_blocks * BS, Z_WAITOK | Z_ZERO))) {
		SAY("r1md: ext4 %s: %u groups: too many", f->name, f->ngroups);
		goto out;
	}
	for (uint32_t i = 0; i < gdt_blocks; i++) {
		if (!rd_blk(m, f->first_data + 1 + i, f->gdt + i * BS)) {
			SAY("r1md: ext4 %s: group descriptors unreadable", f->name);
			goto out;
		}
	}
	f->ok = true;
	f->jnl = 'c';
	if (incompat & 0x4) {
		/* the journal flag is set while a filesystem is mounted rw (and after an unclean stop): read the journal's start field,
		 * nonzero = transactions that were never written home, i.e. what this reader cannot see (no replay, ever) */
		struct ino ji;
		uint64_t pb = 0;
		uint32_t jstart = 0;

		f->jnl = '?';
		if (get_inode(m, le32(sb + 0xe0), &ji) && bmap(m, &ji, 0, &pb) == 1 && rd_blk(m, (uint32_t)pb, tmpb) && __builtin_bswap32(le32(tmpb)) == 0xc03b3998) {
			memcpy(&jstart, tmpb + 0x1c, 4);
			jstart = __builtin_bswap32(jstart);
			f->jnl = jstart ? 'R' : 'r';
		}
		SAY("r1md: ext4 %s: NEEDS_RECOVERY is set (journal %s%s); mounted READ ONLY, the journal is NOT replayed", f->name,
		    f->jnl == 'r' ? "empty" : (f->jnl == 'R' ? "HAS TRANSACTIONS: stale data possible" : "not read"),
		    f->jnl == 'r' ? ", so the disk is current" : "");
	}
	SAY("r1md: ext4 %s: block %llu, %u blocks of 4096, %u groups, inode %u, incompat %#x ro_compat %#x: mounted read only", f->name, f->lba,
	    f->nblocks, f->ngroups, f->isz, incompat, le32(sb + 0x64));
	ok = true;
out:
	kfree_data(buf, BS);
	return ok;
}

struct wk { uint32_t files, dirs, unreadable; uint64_t bytes; };

/* counts and sizes only, per directory that has files: the Mac side compares them with the Android list */
static void
walk(int m, uint32_t depth, const char *path, const struct ino *dir, struct wk *tot)
{
	struct dit it;
	char name[128], child[96];
	uint32_t in, nfiles = 0;
	uint64_t nbytes = 0;

	dit_open(&it, m, dir, walkbuf + depth * BS);
	while (dit_next(&it, &in, name, sizeof(name))) {
		struct ino ci;

		if (!strcmp(name, ".") || !strcmp(name, "..")) {
			continue;
		}
		if (!get_inode(m, in, &ci)) {
			tot->unreadable++;
			continue;
		}
		if ((ci.mode & 0xf000) == 0x8000) {
			nfiles++;
			nbytes += ci.size;
		} else if ((ci.mode & 0xf000) == 0x4000 && depth < 6) {
			tot->dirs++;
			snprintf(child, sizeof(child), "%s/%s", path, name);
			walk(m, depth + 1, child, &ci, tot);
		}
	}
	tot->files += nfiles;
	tot->bytes += nbytes;
	if (nfiles) {
		SAY("r1md: nv %s %s: %u files %llu bytes", fs[m].name, path[0] ? path : "/", nfiles, nbytes);
	}
}

/* ---------------------------------------------------------------- the overlay */

enum { OV_FREE = 0, OV_FILE, OV_DIR, OV_WHITE };
struct ovn { char path[96]; uint8_t state; bool sealed; uint8_t *data; uint32_t size, cap, crc; };
static struct ovn *ovs;
static uint32_t ov_total, ov_nodes, ov_capped;

static int
ovfind(const char *vp)
{
	for (uint32_t i = 0; i < OV_MAX; i++) {
		if (ovs[i].state && !strcmp(ovs[i].path, vp)) {
			return (int)i;
		}
	}
	return -1;
}

static void
ov_clear(int i)
{
	if (ovs[i].data) {
		ov_total -= ovs[i].cap;
		kfree_data(ovs[i].data, ovs[i].cap);
	}
	if (ovs[i].state) {
		ov_nodes--;
	}
	memset(&ovs[i], 0, sizeof(ovs[i]));
}

static int
ov_new(const char *vp, uint8_t state)
{
	int i = ovfind(vp);

	if (i >= 0) {
		ov_clear(i);
	} else {
		for (i = 0; i < (int)OV_MAX && ovs[i].state; i++) {
		}
		if (i == (int)OV_MAX) {
			if (ov_capped++ < 5) {
				SAY("r1md: fs OVERLAY FULL: %u nodes", OV_MAX);
			}
			return -1;
		}
	}
	strlcpy(ovs[i].path, vp, sizeof(ovs[i].path));
	ovs[i].state = state;
	ov_nodes++;
	return i;
}

static bool
ov_grow(int i, uint32_t need)
{
	struct ovn *n = &ovs[i];
	uint32_t cap, old = n->cap;
	uint8_t *d;

	if (need <= n->cap) {
		return true;
	}
	cap = (need + 4095) & ~4095u;
	if (cap < n->cap * 2 && n->cap * 2 <= OV_CAP) {
		cap = n->cap * 2;
	}
	if (ov_total - old + cap > OV_CAP) {
		cap = (need + 4095) & ~4095u;
	}
	if (ov_total - old + cap > OV_CAP || !(d = kalloc_data(cap, Z_WAITOK | Z_ZERO))) {
		if (ov_capped++ < 5) {
			SAY("r1md: fs OVERLAY FULL: cap %u B reached (in use %u, wanted %u more)", OV_CAP, ov_total, cap - old);
		}
		return false;
	}
	if (n->data) {
		memcpy(d, n->data, n->size);
		kfree_data(n->data, old);
	}
	n->data = d;
	n->cap = cap;
	ov_total += cap - old;
	return true;
}

/* r1_md=7 gate 4: the node's checksum as of its last change; every read from the node and the final sweep check it */
static void
ov_seal(int i)
{
	ovs[i].crc = crc32(0, ovs[i].data, ovs[i].size);
	ovs[i].sealed = true;
}

/* ---------------------------------------------------------------- names, drives */

#define E_PARAM   (-2)
#define E_NOFILE  (-9)
#define E_BADH    (-10)
#define E_ACCESS  (-16)
#define E_GENERAL (-18)
#define E_NOPATH  (-19)
#define E_TOOMANY (-5)
#define E_OVERLEN (-49)

/* drive letter -> partition and directory; others have nothing behind them (the overlay only) */
static int
drive_map(char d, const char **base)
{
	switch (d) {
	case 'Z': *base = "md"; return 0;
	case 'V': *base = "md_cmn"; return 0;
	case 'S': *base = ""; return 1;
	case 'X': *base = "md"; return 2;
	case 'Y': *base = "md"; return 3;
	}
	return -1;
}

/* UTF-16LE wire name -> "Z:/a/b" (low bytes, \ to /, slashes collapsed, no trailing slash, no ..) or an FS error */
static int
fs_name(const uint8_t *a, uint32_t alen, char *vp, uint32_t vpsz)
{
	char tmp[128];
	uint32_t n = 0, o = 2;

	for (uint32_t i = 0; i + 1 < alen; i += 2) {
		uint8_t c = a[i];

		if (c == 0) {
			break;
		}
		if (n >= sizeof(tmp) - 1) {
			return E_OVERLEN;
		}
		tmp[n++] = c == '\\' ? '/' : (char)c;
	}
	tmp[n] = 0;
	if (n < 2 || tmp[1] != ':') {
		return E_NOPATH;
	}
	if (strnstr(tmp, "..", n)) {
		return E_PARAM;
	}
	if (vpsz < 4) {
		return E_OVERLEN;
	}
	vp[0] = (tmp[0] >= 'a' && tmp[0] <= 'z') ? (char)(tmp[0] - 32) : tmp[0];
	vp[1] = ':';
	for (uint32_t i = 2; i < n; i++) {
		char c = tmp[i];

		if (c == '/' && (o == 2 || vp[o - 1] == '/')) {
			continue;
		}
		if (c == '.' && (i + 1 == n || tmp[i + 1] == '/') && (o == 2 || vp[o - 1] == '/')) {
			continue;
		}
		if (o + 2 >= vpsz) {
			return E_OVERLEN;
		}
		if (o == 2) {
			vp[o++] = '/';
		}
		vp[o++] = c;
	}
	if (o > 3 && vp[o - 1] == '/') {
		o--;
	}
	vp[o] = 0;
	return 0;
}

enum { R_NONE = 0, R_EFILE, R_EDIR, R_OFILE, R_ODIR };
struct ref { int kind, m, ov; struct ino in; };

/* 0 found, else the FS error (-9 missing, -19 the parent is missing); unmapped drives and whiteouts are missing */
static int
resolve(const char *vp, struct ref *r)
{
	const char *base = "";
	char ep[110];
	int m, e;

	memset(r, 0, sizeof(*r));
	r->ov = ovfind(vp);
	r->m = -1;
	if (r->ov >= 0) {
		if (ovs[r->ov].state == OV_WHITE) {
			return E_NOFILE;
		}
		r->kind = ovs[r->ov].state == OV_DIR ? R_ODIR : R_OFILE;
		return 0;
	}
	m = drive_map(vp[0], &base);
	if (m < 0 || !fs[m].ok) {
		return E_NOFILE;
	}
	if (vp[2]) {
		snprintf(ep, sizeof(ep), "%s%s%s", base, base[0] ? "/" : "", vp + 3);
	} else {
		strlcpy(ep, base, sizeof(ep));
	}
	e = path_lookup(m, ep, &r->in, NULL);
	if (e <= 0) {
		return e == 0 ? E_NOFILE : E_NOPATH;
	}
	r->m = m;
	r->kind = (r->in.mode & 0xf000) == 0x4000 ? R_EDIR : ((r->in.mode & 0xf000) == 0x8000 ? R_EFILE : R_NONE);
	return r->kind == R_NONE ? E_ACCESS : 0;
}

/* a drive root counts as a directory when something is behind it */
static bool
parent_ok(const char *vp)
{
	char p[96];
	struct ref r;
	char *s, *n;

	strlcpy(p, vp, sizeof(p));
	for (s = NULL, n = p; *n; n++) {
		if (*n == '/') {
			s = n;
		}
	}
	if (s == NULL || s == p + 2) {
		const char *b;

		return drive_map(p[0], &b) >= 0;
	}
	*s = 0;
	return resolve(p, &r) == 0 && (r.kind == R_EDIR || r.kind == R_ODIR);
}

/* ---------------------------------------------------------------- handles and the counters */

struct fh { bool used; char vp[96]; int m; struct ino in; int ov; uint64_t pos; uint32_t flags; };
static struct fh *fhs;     /* NFH + 1, index 0 unused */

static struct {
	uint32_t ops, logged, bytes_read, bytes_written, creates, deletes, renames, unimpl, errors, missing, cap_ops, bad_req;
	uint32_t per_op[NOPS + 1], opened_distinct;
	bool logfull_said;
} S;
/* r1_md=7 gates 2-4 (the same struct is in redwood_md.c) */
struct mdfs_gate_info {
	uint32_t unimpl, bad_req, cap_ops, cap_hits, errors, errors_other, prot_reads, prot_short, prot_mut, crc_checked, crc_bad, sweep_nodes, sweep_bad, prot_same, max_req, oth_mut;
	uint64_t prot_bytes;
	char oth_files[96];
};
static struct mdfs_gate_info G;
static uint32_t opened_hash[1024];
static uint32_t missing_hash[64];
static uint32_t unimpl_seen[NOPS + 1];

static uint32_t
hash(const char *s)
{
	uint32_t h = 2166136261u;

	while (*s) {
		h = (h ^ (uint8_t)*s++) * 16777619u;
	}
	return h ? h : 1;
}

static void
note_opened(const char *vp)
{
	uint32_t h = hash(vp);

	for (uint32_t i = 0; i < S.opened_distinct; i++) {
		if (opened_hash[i] == h) {
			return;
		}
	}
	if (S.opened_distinct < 1024) {
		opened_hash[S.opened_distinct++] = h;
	}
}

static void
note_missing(const char *vp)
{
	uint32_t h = hash(vp);

	for (uint32_t i = 0; i < S.missing && i < 64; i++) {
		if (missing_hash[i] == h) {
			return;
		}
	}
	if (S.missing < 64) {
		missing_hash[S.missing] = h;
		FSAY("r1md: fs MISSING \"%s\"", vp);
	}
	S.missing++;
}

static int64_t
fh_size(const struct fh *f)
{
	return f->ov >= 0 ? (int64_t)ovs[f->ov].size : (int64_t)f->in.size;
}

/* the first write to a file that lives on the partition: a copy of it into the overlay, every handle on it moved over */
static int
promote(struct fh *f)
{
	int i;
	uint32_t size = (uint32_t)f->in.size;

	if (f->ov >= 0) {
		return 0;
	}
	if (f->m < 0 || (f->in.mode & 0xf000) != 0x8000) {
		return E_ACCESS;
	}
	if (f->in.size > OV_CAP || (i = ov_new(f->vp, OV_FILE)) < 0) {
		return E_GENERAL;
	}
	if (!ov_grow(i, size ? size : 1) || (size && file_read(f->m, &f->in, 0, size, ovs[i].data) != (int64_t)size)) {
		ov_clear(i);
		return E_GENERAL;
	}
	ovs[i].size = size;
	ov_seal(i);
	for (int h = 1; h <= NFH; h++) {
		if (fhs[h].used && !strcmp(fhs[h].vp, f->vp)) {
			fhs[h].ov = i;
		}
	}
	return 0;
}

/* calibration, IMEI and the protect partitions (X:, Y:): read only, in full, never changed */
static bool
prot_path(const char *p)
{
	return strnstr(p, "/CALIBRAT", 96) || strnstr(p, "/NVD_IMEI", 96);
}

/* the modem's own files on the protect drives (X:, Y:, e.g. NA77_005): changes are counted and named, they do not fail gate 3 */
static bool
xy_path(const char *p)
{
	return (p[0] == 'X' || p[0] == 'Y') && !prot_path(p);
}

/* a read on such a path: short only counts when it is not the end of the file (the request length is the modem's, cut or not) */
static void
prot_read(const struct fh *f, uint32_t want, int64_t got)
{
	if (!f || !prot_path(f->vp)) {
		return;
	}
	G.prot_reads++;
	if (got > 0) {
		G.prot_bytes += (uint64_t)got;
	}
	if (got < 0 || ((uint64_t)got < want && f->pos < (uint64_t)fh_size(f))) {
		G.prot_short++;
		SAY("r1md: fs PROTECTED READ SHORT \"%s\": %d of %u", f->vp, (int)got, want);
	}
}

static uint8_t rdbuf[FS_MAX_READ + 4];

/* up to len bytes at the handle's position into dst; the count read, -1 on error; the position moves */
static int64_t
read_h(struct fh *f, uint32_t len, uint8_t *dst)
{
	int64_t got;

	if (f->ov < 0 && (f->m < 0 || (f->in.mode & 0xf000) != 0x8000)) {
		return 0;       /* a directory handle */
	}
	if (f->ov >= 0) {
		struct ovn *v = &ovs[f->ov];

		got = f->pos >= v->size ? 0 : (v->size - f->pos < len ? (int64_t)(v->size - f->pos) : (int64_t)len);
		if (got) {
			memcpy(dst, v->data + f->pos, (size_t)got);
		}
		if (v->sealed) {
			G.crc_checked++;
			if (crc32(0, v->data, v->size) != v->crc) {
				G.crc_bad++;
				SAY("r1md: fs OVERLAY CHECKSUM MISMATCH on a read of \"%s\"", v->path);
			}
		}
	} else {
		got = file_read(f->m, &f->in, f->pos, len, dst);
	}
	if (got > 0) {
		f->pos += (uint64_t)got;
	}
	return got;
}

static int
fh_alloc(const char *vp, const struct ref *r, uint32_t flags)
{
	for (int h = 1; h <= NFH; h++) {
		if (!fhs[h].used) {
			memset(&fhs[h], 0, sizeof(fhs[h]));
			fhs[h].used = true;
			strlcpy(fhs[h].vp, vp, sizeof(fhs[h].vp));
			fhs[h].m = r->m;
			fhs[h].in = r->in;
			fhs[h].ov = r->kind == R_OFILE ? r->ov : -1;
			fhs[h].flags = flags;
			return h;
		}
	}
	return E_TOOMANY;
}

static struct fh *
fh_get(uint32_t h)
{
	return (h >= 1 && h <= NFH && fhs[h].used) ? &fhs[h] : NULL;
}

/* OPEN: a handle or an FS error */
static int
do_open(const char *vp, uint32_t flags)
{
	struct ref r;
	int e = resolve(vp, &r), h;
	bool create = flags & 0x30000, trunc = flags & 0x20000;

	if (e == 0) {
		/* a directory opens too (the modem opens Z:\\NVRAM with flag 0x800 first, run md2c1): a handle that reads nothing */
		if (trunc && (r.kind == R_EFILE || r.kind == R_OFILE)) {
			int i = ov_new(vp, OV_FILE);

			if (i < 0) {
				return E_GENERAL;
			}
			for (int k = 1; k <= NFH; k++) {
				if (fhs[k].used && !strcmp(fhs[k].vp, vp)) {
					fhs[k].ov = i;
				}
			}
			r.kind = R_OFILE;
			r.ov = i;
		}
	} else {
		int i;

		if (!create) {
			note_missing(vp);
			return e;
		}
		if (!parent_ok(vp)) {
			note_missing(vp);
			return E_NOPATH;
		}
		if ((i = ov_new(vp, OV_FILE)) < 0) {
			return E_GENERAL;
		}
		memset(&r, 0, sizeof(r));
		r.kind = R_OFILE;
		r.ov = i;
		r.m = -1;
		S.creates++;
	}
	h = fh_alloc(vp, &r, flags);
	if (h > 0) {
		note_opened(vp);
	}
	return h;
}

/* a file's whole content as a new overlay file at newvp (rename, move) */
static int
do_rename(const char *from, const char *to)
{
	struct ref r;
	int e = resolve(from, &r), i;
	struct fh tmp;

	if (e != 0) {
		return e;
	}
	if (r.kind != R_EFILE && r.kind != R_OFILE) {
		return E_PARAM;     /* directories: not supported (logged by the caller) */
	}
	if (!parent_ok(to)) {
		return E_NOPATH;
	}
	if (r.kind == R_OFILE) {
		int j = ovfind(to);

		if (j >= 0 && j != r.ov) {
			ov_clear(j);
		}
		strlcpy(ovs[r.ov].path, to, sizeof(ovs[r.ov].path));
		if ((i = ov_new(from, OV_WHITE)) < 0) {
			return E_GENERAL;
		}
		/* an overlay-only file (created by the modem) leaves nothing to hide on the partition, but a copy of one that
		 * replaced a partition file does: the whiteout stays, harmlessly, in both cases */
	} else {
		memset(&tmp, 0, sizeof(tmp));
		strlcpy(tmp.vp, from, sizeof(tmp.vp));
		tmp.m = r.m;
		tmp.in = r.in;
		tmp.ov = -1;
		if ((e = promote(&tmp)) != 0) {
			return e;
		}
		i = ovfind(from);
		{
			int j = ovfind(to);

			if (j >= 0 && j != i) {
				ov_clear(j);
			}
		}
		strlcpy(ovs[i].path, to, sizeof(ovs[i].path));
		if (ov_new(from, OV_WHITE) < 0) {
			return E_GENERAL;
		}
	}
	S.renames++;
	return 0;
}

static int
do_delete(const char *vp)
{
	struct ref r;
	int e = resolve(vp, &r);

	if (e != 0) {
		return e;
	}
	if (r.kind != R_EFILE && r.kind != R_OFILE && r.kind != R_EDIR && r.kind != R_ODIR) {
		return E_ACCESS;
	}
	if (ov_new(vp, OV_WHITE) < 0) {
		return E_GENERAL;
	}
	S.deletes++;
	return 0;
}

/* ---------------------------------------------------------------- the protocol */

static const char *opname[NOPS + 1] = {
	[0x01] = "OPEN", "SEEK", "READ", "WRITE", "CLOSE", "CLOSEALL", "CREATEDIR", "REMOVEDIR", "GETFILESIZE", "GETFOLDERSIZE", "RENAME", "MOVE",
	"COUNT", "GETDISKINFO", "DELETE", "GETATTRIBUTES", "OPENHINT", "FINDFIRST", "FINDNEXT", "FINDCLOSE", "LOCKFAT", "UNLOCKFAT", "SHUTDOWN",
	"XDELETE", "CLEARDISKFLAG", "GETDRIVE", "GETCLUSTERSIZE", "SETDISKFLAG", "OTP_WRITE", "OTP_READ", "OTP_QUERYLENGTH", "OTP_LOCK", "RESTORE",
	"CMPT_READ", "BIN_REGION", "CMPT_WRITE", "GETFILEDETAIL",
};

static uint32_t *ob;
static uint32_t on, onp;
static char last_req[160];
static int phase;
static const uint8_t *pa[8];
static uint32_t pl[8];

static void
ow_word(uint32_t v)
{
	ob[on++] = 4;
	ob[on++] = v;
	onp++;
}

/* an argument of n bytes (src may be NULL = zeros) */
static void
ow_data(const void *src, uint32_t n)
{
	uint32_t w = (n + 3) / 4;

	ob[on++] = n;
	if (w) {
		ob[on + w - 1] = 0;
		if (src) {
			memcpy(&ob[on], src, n);
		} else {
			memset(&ob[on], 0, w * 4);
		}
	}
	on += w;
	onp++;
}

/* one request. req = the payload from the op word; out = the reply from the op word on (>= 0x4100 bytes). Returns the reply's length. */
uint32_t
mdfs_handle(const uint8_t *req, uint32_t len, uint32_t *out)
{
	uint32_t op, nargs, off = 8, res_log = 0, flags = 0;
	char vp[96], path_log[96], extra[40];
	int32_t st = 0;
	bool bad = false, loud = false;
	struct fh *f = NULL;
	uint32_t creates0 = S.creates;
	bool wr_same = false;
	uint32_t wr_pos = 0, wr_len = 0, wr_diff = 0;

	ob = out;
	on = 2;
	onp = 0;
	path_log[0] = 0;
	extra[0] = 0;
	if (len < 8) {
		op = 0;
		nargs = 0;
		bad = true;
	} else {
		op = le32(req);
		nargs = le32(req + 4);
	}
	ob[0] = 0xffff0000u | op;
	if (!bad && (nargs < 1 || nargs > 6) && op != 0x1017 && op != 0x1006 && op != 0x1015 && op != 0x1016 && op != 0x1019 && op != 0x101c &&
	    op != 0x100e) {
		bad = true;
	}
	for (uint32_t i = 0; !bad && i < nargs && i < 6; i++) {
		if (off + 4 > len) {
			bad = true;
			break;
		}
		pl[i] = le32(req + off);
		pa[i] = req + off + 4;
		if (pl[i] == 0 || off + 4 + ((pl[i] + 3) & ~3u) > len) {
			bad = true;
			break;
		}
		off += 4 + ((pl[i] + 3) & ~3u);
	}
	S.ops++;
	if (op >= 0x1001 && op <= 0x1000 + NOPS) {
		S.per_op[op - 0x1000]++;
	}
	if (bad) {
		S.bad_req++;
		FSAY("r1md: fs #%u BAD REQUEST op %#x nargs %u len %u -> -2", S.ops, op, nargs, len);
		ow_word((uint32_t)E_PARAM);
		ob[1] = onp;
		S.errors++;
		return on * 4;
	}
#define ARGW(i) ((i) < nargs && pl[i] >= 4 ? le32(pa[i]) : 0)
#define NAMEARG(i) ((i) < nargs ? fs_name(pa[i], pl[i], vp, sizeof(vp)) : E_PARAM)
	switch (op) {
	case 0x1001:
	case 0x1011: {  /* OPEN, OPENHINT (name, flags[, hint 8 bytes]) */
		int e = NAMEARG(0);

		flags = ARGW(1);
		if (e == 0) {
			strlcpy(path_log, vp, sizeof(path_log));
			e = do_open(vp, flags);
		}
		st = e;
		snprintf(extra, sizeof(extra), "flags %#x", flags);
		ow_word((uint32_t)st);
		if (op == 0x1011) {
			ow_data(nargs > 2 && pl[2] >= 8 ? pa[2] : NULL, 8);
		}
		break;
	}
	case 0x1002: {  /* SEEK (h, off, whence) */
		int32_t o = (int32_t)ARGW(1);
		uint32_t wh = ARGW(2);
		int64_t np;

		f = fh_get(ARGW(0));
		if (!f) {
			st = E_BADH;
		} else if (wh > 2) {
			st = E_PARAM;
		} else {
			np = wh == 0 ? o : (wh == 1 ? (int64_t)f->pos + o : fh_size(f) + o);
			if (np < 0) {
				st = E_PARAM;
			} else {
				f->pos = (uint64_t)np;
				st = (int32_t)np;
			}
		}
		snprintf(extra, sizeof(extra), "off %d wh %u", o, wh);
		ow_word((uint32_t)st);
		break;
	}
	case 0x1003: {  /* READ (h, len) -> {4,status}{4,nread}{nread,data} */
		uint32_t want = ARGW(1), n = 0;

		f = fh_get(ARGW(0));
		G.max_req = want > G.max_req ? want : G.max_req;
		if (want > FS_MAX_READ) {
			SAY("r1md: fs READ CUT: asked %u, given %u", want, FS_MAX_READ);
			S.cap_ops++;
			want = FS_MAX_READ;
		}
		if (!f) {
			st = E_BADH;
		} else {
			int64_t got = read_h(f, want, rdbuf);

			prot_read(f, want, got);
			if (got < 0) {
				st = E_GENERAL;
			} else {
				n = (uint32_t)got;
				S.bytes_read += n;
			}
		}
		ow_word((uint32_t)st);
		ow_word(n);
		ow_data(rdbuf, n);
		snprintf(extra, sizeof(extra), "len %u", ARGW(1));
		res_log = n;
		goto logit;
	}
	case 0x1004: {  /* WRITE (h, data, len) */
		uint32_t n = ARGW(2), written = 0;

		f = fh_get(ARGW(0));
		if (nargs < 3 || n > pl[1]) {
			n = nargs >= 2 ? pl[1] : 0;
		}
		if (!f) {
			st = E_BADH;
		} else if ((st = promote(f)) == 0) {
			struct ovn *v = &ovs[f->ov];

			if (f->pos + n > OV_CAP || !ov_grow(f->ov, (uint32_t)f->pos + n)) {
				st = E_GENERAL;
			} else {
				if (f->pos > v->size) {
					memset(v->data + v->size, 0, (size_t)(f->pos - v->size));
				}
				wr_same = f->pos + n <= v->size && !memcmp(v->data + f->pos, pa[1], n);
				wr_pos = (uint32_t)f->pos;
				wr_len = n;
				for (uint32_t i = 0; f->pos + i < v->size && i < n; i++) {
					wr_diff += v->data[f->pos + i] != ((const uint8_t *)pa[1])[i];
				}
				memcpy(v->data + f->pos, pa[1], n);
				if (crc32(0, pa[1], n) != crc32(0, v->data + f->pos, n)) {      /* read back what was just stored */
					G.crc_bad++;
					SAY("r1md: fs OVERLAY WRITE READ-BACK MISMATCH \"%s\"", v->path);
				}
				f->pos += n;
				if (f->pos > v->size) {
					v->size = (uint32_t)f->pos;
				}
				written = n;
				S.bytes_written += n;
				ov_seal(f->ov);
			}
		}
		snprintf(extra, sizeof(extra), "len %u", n);
		ow_word((uint32_t)st);
		ow_word(written);
		res_log = written;
		goto logit;
	}
	case 0x1005:
		f = fh_get(ARGW(0));
		if (f) {
			strlcpy(path_log, f->vp, sizeof(path_log));
			memset(f, 0, sizeof(*f));
			f = NULL;
		} else {
			st = E_BADH;
		}
		ow_word((uint32_t)st);
		goto logit_nopath;
	case 0x1006:
	case 0x1017:
		for (int h = 1; h <= NFH; h++) {
			memset(&fhs[h], 0, sizeof(fhs[h]));
		}
		if (op == 0x1006) {
			ow_word(0);
		}
		goto logit_nopath;
	case 0x1007: {  /* CREATEDIR: an existing directory is not an error (GUESS: the stock mapping would give -18 for EEXIST) */
		int e = NAMEARG(0);
		struct ref r;

		if (e == 0) {
			strlcpy(path_log, vp, sizeof(path_log));
			e = resolve(vp, &r);
			if (e == 0) {
				e = (r.kind == R_EDIR || r.kind == R_ODIR) ? 0 : E_ACCESS;
			} else if (e == E_NOFILE && parent_ok(vp)) {
				e = ov_new(vp, OV_DIR) >= 0 ? 0 : E_GENERAL;
				S.creates++;
			}
		}
		st = e;
		ow_word((uint32_t)st);
		break;
	}
	case 0x1008:
	case 0x100f: {  /* REMOVEDIR, DELETE */
		int e = NAMEARG(0);

		if (e == 0) {
			strlcpy(path_log, vp, sizeof(path_log));
			e = do_delete(vp);
		}
		st = e;
		ow_word((uint32_t)st);
		break;
	}
	case 0x1009:
		f = fh_get(ARGW(0));
		st = f ? 0 : E_BADH;
		ow_word((uint32_t)st);
		ow_word(f ? (uint32_t)fh_size(f) : 0);
		res_log = f ? (uint32_t)fh_size(f) : 0;
		goto logit;
	case 0x100b:
	case 0x100c: {  /* RENAME, MOVE (old, new) */
		char to[96];
		int e = NAMEARG(0), e2 = E_PARAM;

		if (e == 0) {
			strlcpy(path_log, vp, sizeof(path_log));
			e2 = nargs > 1 ? fs_name(pa[1], pl[1], to, sizeof(to)) : E_PARAM;
			e = e2 == 0 ? do_rename(path_log, to) : e2;
			if (e == E_PARAM) {
				loud = true;
			}
			snprintf(extra, sizeof(extra), "to %.30s", e2 == 0 ? to : "?");
			if (e2 == 0 && prot_path(to)) {
				G.prot_mut++;
			}
		}
		st = e;
		ow_word((uint32_t)st);
		break;
	}
	case 0x100e: {  /* GETDISKINFO: the drive argument is ignored by the stock server (it statfs()es one fixed path); struct 0x54 */
		uint32_t di[0x54 / 4];

		memset(di, 0, sizeof(di));
		di[0x30 / 4] = 512;
		di[0x34 / 4] = 4096 / 512;
		di[0x38 / 4] = fs[0].nblocks;
		di[0x40 / 4] = 512;
		if (nargs >= 1) {
			NAMEARG(0);
			strlcpy(path_log, vp, sizeof(path_log));
		}
		ow_word(0);
		ow_data(di, 0x54);
		break;
	}
	case 0x1010: {  /* GETATTRIBUTES: 0x20 file, 0x10 directory (GUESS: MAUI's FS_ATTR bits) */
		int e = NAMEARG(0);
		struct ref r;

		if (e == 0) {
			strlcpy(path_log, vp, sizeof(path_log));
			e = resolve(vp, &r);
			if (e == 0) {
				e = (r.kind == R_EDIR || r.kind == R_ODIR) ? 0x10 : 0x20;
			}
		}
		st = e;
		ow_word((uint32_t)st);
		break;
	}
	case 0x1015:
		ow_word(0);
		break;
	case 0x1016:
		ow_word(1);
		break;
	case 0x1019:
	case 0x101c:
		ow_word(0);
		break;
	case 0x101a:
		ow_word('Z');
		break;
	case 0x1021: {  /* RESTORE: never (the raw nvram partition is never read for this and never written) */
		int e = NAMEARG(0);

		if (e == 0) {
			strlcpy(path_log, vp, sizeof(path_log));
		}
		st = E_GENERAL;
		loud = true;
		ow_word((uint32_t)st);
		break;
	}
	case 0x1022: {  /* CMPT_READ: name + struct; the reply is the struct's fields {8: done mask, error}{4: size}{4: nread}{len: data} */
		const uint8_t *c = nargs > 1 ? pa[1] : NULL;
		uint32_t mask = c && pl[1] >= 4 ? le32(c) : 0, cflags = c && pl[1] >= 0x10 ? le32(c + 0xc) : 0, done = 0, nread = 0, size = 0;
		uint32_t whence = c && pl[1] >= 0x1c ? le32(c + 0x18) : 0, rlen = c && pl[1] >= 0x24 ? le32(c + 0x20) : 0, d8[2];
		int32_t err = 0, offset = c && pl[1] >= 0x18 ? (int32_t)le32(c + 0x14) : 0;
		int e = NAMEARG(0);

		G.max_req = rlen > G.max_req ? rlen : G.max_req;
		if (rlen > FS_MAX_READ) {
			SAY("r1md: fs READ CUT: asked %u, given %u (%s)", rlen, FS_MAX_READ, vp);
			S.cap_ops++;
			rlen = FS_MAX_READ;
		}
		memset(rdbuf, 0, rlen);         /* the data argument is rlen bytes whatever was read, as in the stock code */
		if (e == 0) {
			strlcpy(path_log, vp, sizeof(path_log));
			flags = cflags;
e = (mask & 1) ? do_open(vp, cflags) : E_PARAM;
		}
		if (e < 0) {
			err = e;
		} else {
			uint32_t h = (uint32_t)e;

			f = fh_get(h);
			done = 1;
			if (mask & 2) {
				size = (uint32_t)fh_size(f);
				done |= 2;
			}
			if (mask & 4) {
				int64_t np = whence == 0 ? offset : (whence == 1 ? (int64_t)f->pos + offset : fh_size(f) + offset);

				if (whence > 2 || np < 0) {
					err = E_PARAM;
				} else {
					f->pos = (uint64_t)np;
					done |= 4;
				}
			}
			if (err == 0 && (mask & 8)) {
				int64_t got = read_h(f, rlen, rdbuf);

				prot_read(f, rlen, got);
				if (got < 0) {
					err = E_GENERAL;
				} else {
					nread = (uint32_t)got;
					S.bytes_read += nread;
					done |= 8;
				}
			}
			if (err == 0 && (mask & 0x10)) {
				done |= 0x10;
			}
			memset(&fhs[h], 0, sizeof(fhs[h]));     /* closed at the end of the one request (the stock code leaks it without the close bit) */
			f = NULL;
		}
		d8[0] = done;
		d8[1] = (uint32_t)err;
		ow_data(d8, 8);
		ow_word(size);
		ow_word(nread);
		ow_data(rdbuf, rlen);
		st = err;
		snprintf(extra, sizeof(extra), "off %d len %u map %#x", offset, rlen, mask);
		res_log = nread;
		goto logit;
	}
	case 0x1023:
		ow_word((uint32_t)E_PARAM);
		st = E_PARAM;
		loud = true;
		break;
	default:
		/* not implemented: the spec's "not supported" status, loudly (once per op in full, every one counted) */
		st = E_PARAM;
		loud = true;
		S.unimpl++;
		if (op < 0x1001 || op > 0x1000 + NOPS || !unimpl_seen[op - 0x1000]++) {
			SAY("r1md: fs UNIMPLEMENTED op %#x %s nargs %u arg0 len %u -> -2", op, op >= 0x1001 && op <= 0x1000 + NOPS && opname[op - 0x1000] ?
			    opname[op - 0x1000] : "?", nargs, nargs ? pl[0] : 0);
		}
		ow_word((uint32_t)st);
		break;
	}
	if (f) {
		strlcpy(path_log, f->vp, sizeof(path_log));
	}
logit_nopath:
logit:
	if (f && !path_log[0]) {
		strlcpy(path_log, f->vp, sizeof(path_log));
	}
	ob[1] = onp;
	if (res_log == 0 && op != 0x1003 && op != 0x1004 && op != 0x1009 && op != 0x1022) {
		res_log = (uint32_t)st;
	}
	{
		int32_t shown = (op == 0x1003 || op == 0x1004 || op == 0x1009 || op == 0x1022) ? (st < 0 ? st : (int32_t)res_log) : (int32_t)res_log;

		if (shown < 0) {
			S.errors++;
			/* the three the stock boot gets too: GETATTRIBUTES FAT6E63FA35.log, CMPT_READ and DELETE of SP81_049 (all -9) */
			if (!((op == 0x1010 && strnstr(path_log, "FAT6E63FA35.log", 96)) ||
			    ((op == 0x1022 || op == 0x100f) && strnstr(path_log, "SP81_049", 96)))) {
				G.errors_other++;
			}
		}
		bool mut_op = op == 0x1004 || op == 0x1007 || op == 0x1008 || op == 0x100b || op == 0x100c || op == 0x100f ||
		    S.creates != creates0 || ((op == 0x1001 || op == 0x1011 || op == 0x1022) && (flags & 0x20000));

		if (mut_op && xy_path(path_log)) {
			const char *b = strrchr(path_log, '/');

			b = b ? b + 1 : path_log;
			G.oth_mut++;
			if (!strnstr(G.oth_files, b, sizeof(G.oth_files))) {
				size_t l = strlen(G.oth_files);

				snprintf(G.oth_files + l, sizeof(G.oth_files) - l, "%s%.12s", l ? " " : "", b);
			}
		}
		if (mut_op && prot_path(path_log)) {
			G.prot_mut++;
			G.prot_same += op == 0x1004 && wr_same;
			SAY("r1md: fs CHANGE ON A PROTECTED PATH: op %#x \"%s\" at %u len %u, %u bytes differ from what was there%s", op, path_log, wr_pos, wr_len, wr_diff, op == 0x1004 && wr_same ? " (the bytes are the ones already there)" : "");
		}
		snprintf(last_req, sizeof(last_req), "#%u %s \"%s\" %s -> %d", S.ops, op >= 0x1001 && op <= 0x1000 + NOPS && opname[op - 0x1000] ?
		    opname[op - 0x1000] : "?", path_log, extra, shown);
		if ((S.logged < LOG_FULL && (S.ops <= LOG_FULL)) || (phase && op != 0x1022 && S.logged < 4 * LOG_FULL)) {
			S.logged++;
			FSAY("r1md: fs #%u %s \"%s\" %s -> %d%s", S.ops, op >= 0x1001 && op <= 0x1000 + NOPS && opname[op - 0x1000] ? opname[op - 0x1000] : "?",
			    path_log, extra, shown, loud ? " (NOT SUPPORTED)" : "");
		} else if (!S.logfull_said) {
			S.logfull_said = true;
			SAY("r1md: fs: the first %u requests are logged; from here only counted", LOG_FULL);
		}
	}
	return on * 4;
}

/* ---------------------------------------------------------------- start, summary */

bool
mdfs_init(void)
{
	struct wk all[4];
	uint32_t total_files = 0;

	bc_mem = kalloc_data(BC_N * BS, Z_WAITOK | Z_ZERO);
	walkbuf = kalloc_data(8 * BS, Z_WAITOK | Z_ZERO);
	ovs = kalloc_data(OV_MAX * sizeof(*ovs), Z_WAITOK | Z_ZERO);
	fhs = kalloc_data((NFH + 1) * sizeof(*fhs), Z_WAITOK | Z_ZERO);
	if (!bc_mem || !walkbuf || !ovs || !fhs) {
		SAY("r1md: fs: no memory");
		return false;
	}
	for (int m = 0; m < 4; m++) {
		struct ino root;

		memset(&all[m], 0, sizeof(all[m]));
		if (!mount_one(m) || !get_inode(m, 2, &root)) {
			SAY("r1md: fs: %s did not mount", fs[m].name);
			if (m == 0) {
				return false;   /* nvdata is the one the modem cannot do without */
			}
			continue;
		}
		walk(m, 0, "", &root, &all[m]);
		SAY("r1md: nv %s total: %u files %llu bytes, %u directories with files below, %u unreadable", fs[m].name, all[m].files, all[m].bytes,
		    all[m].dirs, all[m].unreadable);
		total_files += all[m].files;
	}
	SAY("r1md: nv reader: %u files in all (Android's list: 390)", total_files);
	return fs[0].ok;
}

/* the journal flags of the four partitions, in the order nvdata nvcfg protect1 protect2 */
const char *
mdfs_jnl(void)
{
	static char s[8];

	for (int m = 0; m < 4; m++) {
		s[m] = fs[m].jnl;
	}
	s[4] = 0;
	return s;
}

/* 1 = after HS2: every request but CMPT_READ is logged from here on (the 400-line cap is lifted for them) */
void
mdfs_phase(int p)
{
	phase = p;
}

const char *
mdfs_last(void)
{
	return last_req[0] ? last_req : "none";
}

void
mdfs_counts(uint32_t *ops, uint32_t *missing, uint32_t *ovl)
{
	*ops = S.ops;
	*missing = S.missing;
	*ovl = ov_total;
}

void
mdfs_summary(void)
{
	char line[200];
	size_t n = 0;

	for (uint32_t i = 1; i <= NOPS; i++) {
		if (S.per_op[i] && opname[i]) {
			n += snprintf(line + n, sizeof(line) - n, "%s=%u ", opname[i], S.per_op[i]);
			if (n > 150) {
				SAY("r1md: fs ops: %s", line);
				n = 0;
			}
		}
	}
	if (n) {
		SAY("r1md: fs ops: %s", line);
	}
	SAY("r1md: fs summary: %u requests, %u distinct files opened, read %u B, written to the overlay %u B, creates %u, deletes %u, renames %u",
	    S.ops, S.opened_distinct, S.bytes_read, S.bytes_written, S.creates, S.deletes, S.renames);
	SAY("r1md: fs summary: unimplemented %u, errors returned %u, missing paths %u, bad requests %u, reads cut to %#x %u, overlay %u B in %u nodes, cap hits %u",
	    S.unimpl, S.errors, S.missing, S.bad_req, FS_MAX_READ, S.cap_ops, ov_total, ov_nodes, ov_capped);
}

/* r1_md=7: the counters for gates 2-4, after a sweep of every overlay node's checksum */
void
mdfs_gate(struct mdfs_gate_info *g)
{
	G.sweep_nodes = G.sweep_bad = 0;
	for (uint32_t i = 0; ovs && i < OV_MAX; i++) {
		if (ovs[i].state == OV_FILE && ovs[i].sealed) {
			G.sweep_nodes++;
			if (crc32(0, ovs[i].data, ovs[i].size) != ovs[i].crc) {
				G.sweep_bad++;
			}
		}
	}
	G.unimpl = S.unimpl;
	G.bad_req = S.bad_req;
	G.cap_ops = S.cap_ops;
	G.cap_hits = ov_capped;
	G.errors = S.errors;
	*g = G;
}

/* the overlay goes with the run */
void
mdfs_done(void)
{
	if (ovs) {
		for (int i = 0; i < (int)OV_MAX; i++) {
			if (ovs[i].data) {
				ov_clear(i);
			}
		}
	}
}
