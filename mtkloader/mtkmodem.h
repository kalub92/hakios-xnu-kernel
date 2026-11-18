/*
 * Modem stage 0 (docs/cellular-plan.md), reading only: what LK hands over for MD1 in the device tree, printed as "md0:" lines.
 * /chosen/ccci,modem_info_v2 is MediaTek's struct _ccci_lk_info_v2 (little endian): u64 tag area, u32 size, s32 errno, u32 version,
 * u32 count, u32 load flag, s32 per-MD errno. The tags are in memory at that physical address (the MMU is off here, so it is read
 * as it is, byte by byte). Decoded as haretic's r1_md_probe.c r1md_decode_tag does. Uses dt_be32, dt_streq, dt_contains, dt_len
 * of mtkconnsys.h, which this file follows. Only called with " r1_md=" in the boot-args.
 */

static uint32_t
md_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
md_le64(const uint8_t *p)
{
	return (uint64_t)md_le32(p) | (uint64_t)md_le32(p + 4) << 32;
}

/* up to n printable characters of a string that may not end */
static void
md_str(const uint8_t *s, uint32_t n)
{
	for (uint32_t i = 0; i < n && s[i]; i++) {
		out_putc(s[i] >= 32 && s[i] < 127 ? (char)s[i] : '?');
	}
}

/* what the kernel is handed (boot-args, md_append_args): read only copies of the values decoded below */
static struct {
	uint32_t smem, smem_sz, ccb, ccb_sz, img, img_sz, dfd;
	int ok;
} md_hand;

static void
md_decode_tag(const uint8_t *name, const uint8_t *d, uint32_t size, uint64_t *md_base, uint32_t *md_size, uint64_t *nc, uint32_t *nc_size)
{
	if (dt_streq((const char *)name, "hdr_tbl_inf")) {
		for (uint32_t i = 0; i + 24 <= size; i += 24) {
			uint64_t base = md_le64(d + i);
			uint32_t sz = md_le32(d + i + 8);

			if (!base && !sz) {
				continue;
			}
			out("md0: hdr_tbl_inf md_id ");
			out_dec(d[i + 12]);
			out(" base ");
			out_hex(base);
			out(" size ");
			out_hex(sz);
			out(" errno ");
			out_dec((uint32_t)(int32_t)(int8_t)d[i + 13]);
			out("\n");
			if (d[i + 12] == 0 && d[i + 13] == 0) {
				*md_base = base;
				*md_size = sz;
				md_hand.img = (uint32_t)base;
				md_hand.img_sz = sz;
			}
		}
	} else if (dt_streq((const char *)name, "smem_layout") && size >= 36) {
		out("md0: smem_layout base ");
		out_hex(md_le64(d));
		out(" ap_md1 +");
		out_hex(md_le32(d + 8));
		out(" size ");
		out_hex(md_le32(d + 12));
		out("\n");
		*nc = md_le64(d) + md_le32(d + 8);
		*nc_size = md_le32(d + 12);
		md_hand.smem = (uint32_t)*nc;
		md_hand.smem_sz = *nc_size;
	} else if (dt_streq((const char *)name, "ccb_info") && size >= 12) {
		out("md0: ccb_info ");
		out_hex(md_le64(d));
		out(" size ");
		out_hex(md_le32(d + 8));
		out("\n");
		md_hand.ccb = (uint32_t)md_le64(d);
		md_hand.ccb_sz = md_le32(d + 8);
	} else if (dt_streq((const char *)name, "smem_dfd_size") && size == 4) {
		md_hand.dfd = md_le32(d);
	} else if (dt_streq((const char *)name, "md_mpu_inf")) {
		for (uint32_t i = 0; i + 24 <= size; i += 24) {
			if (md_le32(d + i + 8) == 0xffffffffu) {
				break;
			}
			out("md0: mpu region ");
			out_dec(md_le32(d + i + 8));
			out(" ");
			out_hex(md_le32(d + i));
			out("-");
			out_hex(md_le32(d + i + 4));
			out(" apc ");
			out_hex(md_le32(d + i + 12));
			out(" ");
			out_hex(md_le32(d + i + 16));
			out("\n");
		}
	} else if (dt_streq((const char *)name, "md1_chk") && size >= 192) {
		out("md0: md1_chk build ");
		md_str(d + 104, 64);
		out(" (");
		md_str(d + 40, 64);
		out(") platform ");
		md_str(d + 24, 16);
		out(" mem_size ");
		out_hex(md_le32(d + 172));
		out(" img_size ");
		out_hex(md_le32(d + 176));
		out("\n");
	}
}

static void
modem_dt_report(uint64_t dtb)
{
	const uint8_t *fdt = (const uint8_t *)(uintptr_t)dtb;
	const uint8_t *structs, *p, *end, *info = NULL;
	const char *strings;
	const char *names[8];
	uint32_t info_len = 0, rm_ac = 2, rm_sc = 2;
	int depth = -1, in_rm = 0, in_chosen = 0;
	uint64_t md_base = 0, nc = 0;
	uint32_t md_size = 0, nc_size = 0;
	int usable = 0, have = 0;

	if (dtb == 0 || dt_be32(fdt) != FDT_MAGIC) {
		out("md0: no flattened device tree from LK\n");
		return;
	}
	structs = fdt + dt_be32(fdt + 8);
	strings = (const char *)(fdt + dt_be32(fdt + 12));
	end = structs + dt_be32(fdt + 36);
	/* pass 1: the handoff property, and reserved-memory's cell sizes (they come before its children) */
	for (p = structs; p < end;) {
		uint32_t token = dt_be32(p);

		p += 4;
		if (token == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;

			p += (dt_len(name) + 1 + 3) & ~3u;
			depth++;
			if (depth < 8) {
				names[depth] = name;
			}
			in_chosen = depth == 1 && dt_streq(name, "chosen");
			in_rm = depth == 1 && dt_streq(name, "reserved-memory");
		} else if (token == FDT_END_NODE) {
			depth--;
			in_chosen = in_rm = 0;
		} else if (token == FDT_PROP) {
			uint32_t len = dt_be32(p), nameoff = dt_be32(p + 4);
			const uint8_t *value = p + 8;
			const char *pname = strings + nameoff;

			p += 8 + ((len + 3) & ~3u);
			if (in_chosen && dt_streq(pname, "ccci,modem_info_v2")) {
				info = value;
				info_len = len;
			} else if (in_rm && len == 4 && dt_streq(pname, "#address-cells")) {
				rm_ac = dt_be32(value);
			} else if (in_rm && len == 4 && dt_streq(pname, "#size-cells")) {
				rm_sc = dt_be32(value);
			}
		} else if (token == FDT_END) {
			break;
		}
	}
	if (info == NULL || info_len < 28) {
		out("md0: /chosen/ccci,modem_info_v2 missing or short\n");
	} else {
		uint64_t base = md_le64(info);
		uint32_t size = md_le32(info + 8), version = md_le32(info + 16), count = md_le32(info + 20);
		uint32_t name_len = version >= 2 ? 64 : 16, off = 0;

		have = 1;
		out("md0: modem_info_v2 tags ");
		out_hex(base);
		out(" size ");
		out_hex(size);
		out(" ver ");
		out_dec(version);
		out(" count ");
		out_dec(count);
		out(" lk_errno ");
		out_dec(md_le32(info + 12));
		out(" flag ");
		out_hex(md_le32(info + 24));
		out(" md1 errno ");
		out_dec(info_len >= 32 ? md_le32(info + 28) : 0xffffffffu);
		out("\n");
		if (base != 0 && size != 0 && size <= 0x10000 && count <= 64) {
			const uint8_t *area = (const uint8_t *)(uintptr_t)base;

			for (uint32_t i = 0; i < count; i++) {
				uint32_t data_off, data_size, next;

				if (off + name_len + 12 > size) {
					break;
				}
				data_off = md_le32(area + off + name_len);
				data_size = md_le32(area + off + name_len + 4);
				next = md_le32(area + off + name_len + 8);
				if (data_off > size || data_size > size - data_off) {
					break;
				}
				md_decode_tag(area + off, area + data_off, data_size, &md_base, &md_size, &nc, &nc_size);
				if (next <= off && i + 1 < count) {
					break;
				}
				off = next;
			}
		}
		usable = md_base && md_size && nc && nc_size;
	}
	/* pass 2: reserved-memory's children whose name has ccci or md */
	depth = -1;
	for (p = structs; p < end;) {
		uint32_t token = dt_be32(p);
		static const char *skip_name;

		p += 4;
		if (token == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;

			p += (dt_len(name) + 1 + 3) & ~3u;
			depth++;
			if (depth < 8) {
				names[depth] = name;
			}
			skip_name = name;
		} else if (token == FDT_END_NODE) {
			depth--;
		} else if (token == FDT_PROP) {
			uint32_t len = dt_be32(p);
			const uint8_t *value = p + 8;
			const char *pname = strings + dt_be32(p + 4);

			p += 8 + ((len + 3) & ~3u);
			if (depth == 2 && depth < 8 && dt_streq(names[1], "reserved-memory") && dt_streq(pname, "reg") &&
			    (dt_contains(skip_name, dt_len(skip_name), "ccci") || dt_contains(skip_name, dt_len(skip_name), "md"))) {
				out("md0: reserved-memory ");
				out(skip_name);
				out(" reg");
				for (uint32_t i = 0; i + 4 * (rm_ac + rm_sc) <= len && i < 4 * (rm_ac + rm_sc) * 4; i += 4 * (rm_ac + rm_sc)) {
					uint64_t a = 0, s = 0;

					for (uint32_t c = 0; c < rm_ac; c++) {
						a = a << 32 | dt_be32(value + i + 4 * c);
					}
					for (uint32_t c = 0; c < rm_sc; c++) {
						s = s << 32 | dt_be32(value + i + 4 * (rm_ac + c));
					}
					out(" ");
					out_hex(a);
					out("+");
					out_hex(s);
				}
				out("\n");
			}
		} else if (token == FDT_END) {
			break;
		}
	}
	md_hand.ok = usable;
	out(usable ? "md0: handoff usable\n" : have ? "md0: handoff INCOMPLETE\n" : "md0: handoff NONE\n");
}

/* " r1_md_smem=0x... " etc. appended to the kernel's boot-args when the handoff was usable (the kernel's r1_md=2 reads them) */
static void
md_append_args(char *cmd, uint32_t room)
{
	static const char hex[] = "0123456789abcdef";
	const struct { const char *key; uint32_t v; } a[] = {
		{ " r1_md_smem=0x", md_hand.smem }, { " r1_md_smem_sz=0x", md_hand.smem_sz }, { " r1_md_ccb=0x", md_hand.ccb },
		{ " r1_md_ccb_sz=0x", md_hand.ccb_sz }, { " r1_md_img=0x", md_hand.img }, { " r1_md_img_sz=0x", md_hand.img_sz },
		{ " r1_md_dfd=0x", md_hand.dfd },
	};
	uint32_t n = 0;

	if (!md_hand.ok) {
		return;
	}
	while (cmd[n]) {
		n++;
	}
	for (unsigned i = 0; i < sizeof(a) / sizeof(a[0]); i++) {
		uint32_t k = 0;

		if (n + 14 + 8 + 1 >= room) {
			return;
		}
		while (a[i].key[k]) {
			cmd[n++] = a[i].key[k++];
		}
		for (int sh = 28; sh >= 0; sh -= 4) {
			cmd[n++] = hex[(a[i].v >> sh) & 15];
		}
		cmd[n] = 0;
	}
}
