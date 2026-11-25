// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * The panel console in its text_color= from the first kernel line.
 *
 * S07textcolor repaints the console in the configured preset, but only
 * once init runs it: the kernel's boot log before that is drawn in the
 * stock VT palette (grey on black).  The kernel takes its VT palette
 * defaults from vt.default_red/_grn/_blu= on the command line before the
 * first console is set up, so the shim writes the preset there: the DTB
 * bootargs carry those three parameters with the kernel's own stock
 * values in a fixed width ("0xNN" x 16), and they are overwritten in
 * place like the other DTB placeholders.  The preset name comes from
 * config.txt in the boot FAT slot the shim has just loaded into PSRAM,
 * read with S07textcolor's line rules.
 *
 * No ESP-IDF calls: linux/tests/tab5-textcolor-harness.c builds this
 * file on a host, and linux/tests/test-tab5-textcolor-boot.py checks the
 * preset table against textcolor.sh.
 */

#include "tab5_textcolor.h"

#include <string.h>

static uint32_t le16(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return le16(p) | (le16(p + 2) << 16);
}

/*
 * The first text_color= line, byte by byte across cluster boundaries.
 * S07textcolor reads each line with 'IFS= read -r', drops one trailing
 * CR, skips leading blanks (space, tab), takes the first line that then
 * starts with "text_color=" and trims blanks off both ends of the value.
 * Here a CR is held back until the next byte shows whether it ends the
 * line, and the value is stored without its leading blanks; trailing
 * blanks are stored and cut off at the end (len_trim).
 */
enum tc_state { TC_LINE_START, TC_KEY, TC_VALUE, TC_SKIP };

struct tc_scan {
	enum tc_state st;
	size_t key_pos;
	char *name;
	size_t cap;		/* name bytes, NUL excluded */
	size_t len;		/* stored bytes */
	size_t len_trim;	/* stored bytes up to the last non-blank */
	bool cr;		/* a CR waits for the next byte */
	bool over;		/* the trimmed value does not fit */
};

static const char tc_key[] = "text_color=";

static bool tc_blank(uint8_t c)
{
	return c == ' ' || c == '\t';
}

static void tc_store(struct tc_scan *s, uint8_t c)
{
	if (!s->len && tc_blank(c))
		return;		/* leading blank */
	if (s->len < s->cap) {
		s->name[s->len++] = (char)c;
		if (!tc_blank(c))
			s->len_trim = s->len;
	} else if (!tc_blank(c)) {
		s->over = true;
	}
}

static void tc_finish(struct tc_scan *s)
{
	if (s->over)
		s->len_trim = 0;	/* too long to be a preset name */
	s->name[s->len_trim] = '\0';
}

/* true when the text_color= line has ended: the name is final. */
static bool tc_feed(struct tc_scan *s, uint8_t c)
{
	switch (s->st) {
	case TC_LINE_START:
		if (c == '\n' || tc_blank(c))
			return false;
		s->st = TC_KEY;
		s->key_pos = 0;
		/* fall through */
	case TC_KEY:
		if (c == (uint8_t)tc_key[s->key_pos]) {
			if (++s->key_pos == sizeof(tc_key) - 1) {
				s->st = TC_VALUE;
				s->len = s->len_trim = 0;
				s->cr = s->over = false;
			}
		} else {
			s->st = c == '\n' ? TC_LINE_START : TC_SKIP;
		}
		return false;
	case TC_VALUE:
		if (s->cr) {
			s->cr = false;
			if (c == '\n')
				return true;	/* CRLF: the CR is dropped */
			tc_store(s, '\r');	/* a CR inside the line is text */
		}
		if (c == '\n')
			return true;
		if (c == '\r')
			s->cr = true;
		else
			tc_store(s, c);
		return false;
	case TC_SKIP:
		if (c == '\n')
			s->st = TC_LINE_START;
		return false;
	}
	return false;
}

/* FAT12 entry of cluster c; 0xfff (end of chain) outside the table. */
static uint32_t fat12_next(const uint8_t *fat, size_t fat_bytes, uint32_t c)
{
	size_t off = (size_t)c + c / 2;
	uint32_t v;

	if (off + 1 >= fat_bytes)
		return 0xfff;
	v = le16(fat + off);
	return (c & 1) ? v >> 4 : v & 0xfff;
}

int tab5_textcolor_from_fat(const uint8_t *img, size_t img_len, char *name,
			    size_t name_len)
{
	uint32_t bps, spc, rsv, nfats, root_ents, total, fat_secs;
	uint32_t root_secs, data_sec, clusters, cluster, left, hops = 0;
	uint64_t meta_bytes;
	const uint8_t *fat, *root, *ent = NULL;
	struct tc_scan s;

	if (!img || !name || name_len < 2)
		return -1;
	name[0] = '\0';
	if (img_len < 512 || img[510] != 0x55 || img[511] != 0xaa)
		return -1;
	bps = le16(img + 11);
	spc = img[13];
	rsv = le16(img + 14);
	nfats = img[16];
	root_ents = le16(img + 17);
	total = le16(img + 19);
	if (!total)
		total = le32(img + 32);
	fat_secs = le16(img + 22);
	if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc ||
	    (spc & (spc - 1)) || !rsv || !nfats || !root_ents || !fat_secs)
		return -1;
	root_secs = (root_ents * 32 + bps - 1) / bps;
	data_sec = rsv + nfats * fat_secs + root_secs;
	if (data_sec >= total)
		return -1;
	clusters = (total - data_sec) / spc;
	/* Fewer than 4085 clusters is what makes a volume FAT12. */
	if (!clusters || clusters >= 4085)
		return -1;
	meta_bytes = (uint64_t)data_sec * bps;
	if (meta_bytes > img_len)
		return -1;

	fat = img + (size_t)rsv * bps;
	root = img + (size_t)(rsv + nfats * fat_secs) * bps;
	for (uint32_t i = 0; i < root_ents; i++) {
		const uint8_t *e = root + (size_t)i * 32;

		if (e[0] == 0x00)
			break;			/* end of the directory */
		if (e[0] == 0xe5)
			continue;		/* deleted */
		if ((e[11] & 0x3f) == 0x0f)
			continue;		/* long-name piece */
		if (e[11] & 0x18)
			continue;		/* directory or volume label */
		if (!memcmp(e, "CONFIG  TXT", 11)) {
			ent = e;
			break;
		}
	}
	if (!ent)
		return 0;

	memset(&s, 0, sizeof(s));
	s.st = TC_LINE_START;
	s.name = name;
	s.cap = name_len - 1;
	left = le32(ent + 28);
	cluster = le16(ent + 26);
	while (left) {
		uint32_t cbytes = spc * bps;
		uint32_t take = left < cbytes ? left : cbytes;
		uint64_t off;

		/* A chain longer than the volume is a loop. */
		if (cluster < 2 || cluster >= clusters + 2 || ++hops > clusters)
			return -1;
		off = ((uint64_t)data_sec + (uint64_t)(cluster - 2) * spc) * bps;
		if (off + take > img_len)
			return -1;
		for (uint32_t i = 0; i < take; i++) {
			if (tc_feed(&s, img[off + i])) {
				tc_finish(&s);
				return 1;
			}
		}
		left -= take;
		if (!left)
			break;
		cluster = fat12_next(fat, (size_t)fat_secs * bps, cluster);
		if (cluster >= 0xff7)
			return -1;		/* bad cluster or early end */
	}
	/* The last line may have no newline. */
	if (s.st == TC_VALUE) {
		tc_finish(&s);
		return 1;
	}
	return 0;
}

/*
 * The same presets as textcolor.sh's textcolor_seq (normal, bright,
 * dim, background).  test-tab5-textcolor-boot.py fails when the two
 * tables differ.
 */
struct tc_preset {
	const char *name;
	uint32_t normal, bright, dim, bg;
};

static const struct tc_preset tc_presets[] = {
	{ "green",  0x2bd62b, 0x47ff47, 0x177317, 0x000000 },
	{ "amber",  0xd67200, 0xff941a, 0x733d00, 0x000000 },
	{ "orange", 0xd65400, 0xff741a, 0x732d00, 0x000000 },
	{ "cyan",   0x2bbcd6, 0x47e3ff, 0x176573, 0x000000 },
	{ "blue",   0x598bd6, 0x79aeff, 0x304a73, 0x000000 },
	{ "pink",   0xd65ca7, 0xff7ccd, 0x73325a, 0x000000 },
	{ "red",    0xd63636, 0xff5353, 0x731d1d, 0x000000 },
	{ "lcd",    0x11100d, 0x090907, 0x2d271b, 0x4e422c },
};

static void tc_slot(uint8_t rgb[3], uint32_t v)
{
	rgb[0] = (uint8_t)(v >> 16);
	rgb[1] = (uint8_t)(v >> 8);
	rgb[2] = (uint8_t)v;
}

bool tab5_textcolor_palette(const char *name, uint8_t rgb[16][3])
{
	for (size_t i = 0; i < sizeof(tc_presets) / sizeof(tc_presets[0]); i++) {
		const struct tc_preset *p = &tc_presets[i];

		if (strcmp(name, p->name))
			continue;
		tc_slot(rgb[0], p->bg);
		for (int k = 1; k < 8; k++)
			tc_slot(rgb[k], p->normal);
		tc_slot(rgb[8], p->dim);
		for (int k = 9; k < 16; k++)
			tc_slot(rgb[k], p->bright);
		return true;
	}
	return false;
}

/* "0xNN,0xNN,...,0xNN": 16 values, 15 commas. */
#define TC_LIST_LEN (16 * 5 - 1)

static bool tc_hex(uint8_t c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	       (c >= 'A' && c <= 'F');
}

/* The value list after the first "vt.default_xxx=", or NULL when it is
 * missing or not 16 fixed-width values ending the parameter. */
static uint8_t *tc_list(uint8_t *dtb, size_t len, const char *key)
{
	size_t klen = strlen(key);

	for (size_t i = 0; i + klen + TC_LIST_LEN < len; i++) {
		uint8_t *p = dtb + i + klen;

		if (memcmp(dtb + i, key, klen))
			continue;
		for (int k = 0; k < 16; k++) {
			const uint8_t *v = p + k * 5;

			if (v[0] != '0' || v[1] != 'x' || !tc_hex(v[2]) ||
			    !tc_hex(v[3]) || (k < 15 && v[4] != ','))
				return NULL;
		}
		if (p[TC_LIST_LEN] != ' ' && p[TC_LIST_LEN] != '\0')
			return NULL;
		return p;
	}
	return NULL;
}

bool tab5_textcolor_patch_bootargs(uint8_t *dtb, size_t dtb_len,
				   uint8_t rgb[16][3])
{
	static const char *const keys[3] = {
		"vt.default_red=", "vt.default_grn=", "vt.default_blu=",
	};
	static const char hex[] = "0123456789abcdef";
	uint8_t *list[3];

	for (int c = 0; c < 3; c++) {
		list[c] = tc_list(dtb, dtb_len, keys[c]);
		if (!list[c])
			return false;
	}
	for (int c = 0; c < 3; c++) {
		for (int k = 0; k < 16; k++) {
			list[c][k * 5 + 2] = (uint8_t)hex[rgb[k][c] >> 4];
			list[c][k * 5 + 3] = (uint8_t)hex[rgb[k][c] & 15];
		}
	}
	return true;
}
