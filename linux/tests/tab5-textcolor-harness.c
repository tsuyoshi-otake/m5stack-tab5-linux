// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * Host harness for linux/boot-shim/main/tab5_textcolor.c, driven by
 * test-tab5-textcolor-boot.py: a boot FAT image file and a bootargs
 * string as the arguments.  It runs what the shim's patch_text_color()
 * runs and prints the outcome:
 *
 *   rc=<tab5_textcolor_from_fat result>
 *   name=<the name, hex>
 *   palette=<1 when the name is a preset>
 *   patched=<1 when the bootargs were rewritten>
 *   args=<the bootargs afterwards>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tab5_textcolor.h"

static uint8_t img[1u << 20];

int main(int argc, char **argv)
{
	char name[TAB5_TC_NAME_MAX];
	uint8_t rgb[16][3];
	size_t n, alen;
	uint8_t *args;
	int rc, pal = 0, patched = 0;
	FILE *f;

	if (argc != 3)
		return 2;
	f = fopen(argv[1], "rb");
	if (!f)
		return 2;
	n = fread(img, 1, sizeof(img), f);
	fclose(f);
	alen = strlen(argv[2]) + 1;	/* the DTB string's NUL */
	args = malloc(alen);
	if (!args)
		return 2;
	memcpy(args, argv[2], alen);

	rc = tab5_textcolor_from_fat(img, n, name, sizeof(name));
	printf("rc=%d\nname=", rc);
	for (const char *p = name; *p; p++)
		printf("%02x", (unsigned char)*p);
	if (rc == 1)
		pal = tab5_textcolor_palette(name, rgb);
	if (pal)
		patched = tab5_textcolor_patch_bootargs(args, alen, rgb);
	printf("\npalette=%d\npatched=%d\nargs=%s\n", pal, patched,
	       (const char *)args);
	free(args);
	return 0;
}
