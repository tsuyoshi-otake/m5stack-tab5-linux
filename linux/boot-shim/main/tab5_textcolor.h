/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
#ifndef TAB5_TEXTCOLOR_H
#define TAB5_TEXTCOLOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The panel console's text_color from the first kernel line.  Pure C (no
 * ESP-IDF calls) so that linux/tests/tab5-textcolor-harness.c runs it on a
 * host. */

#define TAB5_TC_NAME_MAX 16

/* text_color= from config.txt in a FAT12 boot image: 1 with the value in
 * name (NUL-terminated; a value longer than name_len - 1 comes back empty,
 * no preset name is that long), 0 when config.txt or the key is missing,
 * -1 when the image is not a FAT12 volume this reader follows.  The line
 * rules are S07textcolor's: leading blanks skipped, one trailing CR
 * dropped, the value trimmed, the first text_color= line wins. */
int tab5_textcolor_from_fat(const uint8_t *img, size_t img_len, char *name,
			    size_t name_len);

/* The 16 palette slots of a preset, the same table as textcolor.sh: slot 0
 * the background, 1-7 the normal colour, 8 the dim shade, 9-15 bright.
 * false for "white" and unknown names: the kernel's stock palette stays. */
bool tab5_textcolor_palette(const char *name, uint8_t rgb[16][3]);

/* Overwrite the values of the vt.default_red/_grn/_blu= placeholder in the
 * DTB's bootargs (16 fixed-width "0xNN" values each) in place.  false when
 * any of the three is missing or malformed; nothing is written then. */
bool tab5_textcolor_patch_bootargs(uint8_t *dtb, size_t dtb_len,
				   uint8_t rgb[16][3]);

#endif
