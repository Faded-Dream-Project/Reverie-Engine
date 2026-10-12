/* ============================================================================
 * Reverie Engine — pack.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "pack.h"

#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "png.h"
#include "rev.h"
#include "util.h"

#define PACK_HEADER_SIZE 40
#define PACK_ENTRY_SIZE 28    /* name_off(4) name_len(4) data_off(8) data_len(8) crc(4) */

static u32 pk_get32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 pk_get64(const u8 *p)
{
	return (u64)pk_get32(p) | ((u64)pk_get32(p + 4) << 32);
}

static void pk_put32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static void pk_put64(u8 *p, u64 v)
{
	pk_put32(p, (u32)v);
	pk_put32(p + 4, (u32)(v >> 32));
}

bool rev_pack_probe(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	u8 magic[8];
	size_t n = fread(magic, 1, 8, f);
	fclose(f);
	if (n != 8)
		return false;
	return memcmp(magic, REV_PACK_MAGIC, 6) == 0;
}

bool rev_pack_load(const char *path, RevPack *pk, char *err, size_t errcap)
{
	memset(pk, 0, sizeof *pk);
	size_t len = 0;
	pk->file = rev_read_file(path, &len);
	pk->file_len = len;
	if (!pk->file || len < PACK_HEADER_SIZE + 8) {
		snprintf(err, errcap, "'%s' is not a .revpack (too small)", path);
		rev_pack_free(pk);
		return false;
	}
	const u8 *f = pk->file;

	u32 ver = (u32)f[6] | ((u32)f[7] << 8);
	if (memcmp(f, REV_PACK_MAGIC, 6) != 0 || ver != REV_PACK_VERSION) {
		snprintf(err, errcap, "'%s' is not a .revpack (bad magic/version)", path);
		rev_pack_free(pk);
		return false;
	}

	u32 scene_off = pk_get32(f + 8);
	u32 scene_len = pk_get32(f + 12);
	u32 n_assets = pk_get32(f + 16);
	u32 name_off = pk_get32(f + 20);
	u32 name_len = pk_get32(f + 24);
	u32 data_off = pk_get32(f + 28);
	u32 hdr_crc = pk_get32(f + 32);

	if (rev_crc32(f, 32) != hdr_crc) {
		snprintf(err, errcap, "pack header CRC mismatch");
		rev_pack_free(pk);
		return false;
	}

	if (n_assets > REV_PACK_MAX_ASSETS) {
		snprintf(err, errcap, "pack has too many assets (%u)", n_assets);
		rev_pack_free(pk);
		return false;
	}

	/* bounds checks */
	u64 need = (u64)PACK_HEADER_SIZE + (u64)n_assets * PACK_ENTRY_SIZE;
	if (need > len) {
		snprintf(err, errcap, "pack truncated (asset table)");
		rev_pack_free(pk);
		return false;
	}
	if ((u64)scene_off + scene_len + 1 > len ||
	    (u64)name_off + name_len > len || data_off > len) {
		snprintf(err, errcap, "pack truncated (regions)");
		rev_pack_free(pk);
		return false;
	}

	pk->scene_text = (const char *)f + scene_off;
	pk->scene_len = scene_len;
	pk->n_assets = (int)n_assets;

	const u8 *names = f + name_off;
	for (u32 i = 0; i < n_assets; i++) {
		const u8 *e = f + PACK_HEADER_SIZE + (size_t)i * PACK_ENTRY_SIZE;
		u32 no = pk_get32(e);
		u32 nl = pk_get32(e + 4);
		u64 doff = pk_get64(e + 8);
		u64 dlen = pk_get64(e + 16);
		u32 crc = pk_get32(e + 24);

		if ((u64)no + nl > name_len || (u64)doff + dlen > len) {
			snprintf(err, errcap, "pack truncated (asset %u)", i);
			rev_pack_free(pk);
			return false;
		}
		if (nl == 0 || nl > 512) {
			snprintf(err, errcap, "pack asset %u has a bad name", i);
			rev_pack_free(pk);
			return false;
		}

		pk->assets[i].name = (const char *)names + no;
		pk->assets[i].data = f + doff;
		pk->assets[i].len = (size_t)dlen;
		pk->assets[i].crc = crc;

		if (rev_crc32(pk->assets[i].data, (size_t)dlen) != crc) {
			snprintf(err, errcap, "pack asset '%s' CRC mismatch", pk->assets[i].name);
			rev_pack_free(pk);
			return false;
		}
	}
	return true;
}

void rev_pack_free(RevPack *pk)
{
	free(pk->file);
	memset(pk, 0, sizeof *pk);
}

const u8 *rev_pack_asset(const RevPack *pk, const char *name, size_t *len)
{
	if (!pk || !name)
		return NULL;
	for (int i = 0; i < pk->n_assets; i++)
		if (strcmp(pk->assets[i].name, name) == 0) {
			if (len)
				*len = pk->assets[i].len;
			return pk->assets[i].data;
		}
	return NULL;
}

/* ---- writing ---------------------------------------------------------------- */

/* Collects referenced image paths from the scene IR. Returns a malloc'd
 * array of malloc'd strings; count in *n_out. */
static char **collect_assets(const RSceneIR *ir, int *n_out)
{
	char **list = NULL;
	int n = 0, cap = 0;

	for (int li = 0; li < ir->nlayers; li++) {
		const RLayerIR *L = ir->layers[li];
		for (int pi = 0; pi < L->nprops; pi++) {
			const RProp *p = &L->props[pi];
			if (strcmp(p->key, "image") != 0 && strcmp(p->key, "src") != 0)
				continue;
			if (!p->val || p->val->kind != RV_STR || !p->val->str)
				continue;
			const char *src = p->val->str;
			/* dedupe */
			bool dup = false;
			for (int i = 0; i < n; i++)
				if (strcmp(list[i], src) == 0) {
					dup = true;
					break;
				}
			if (dup)
				continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 8;
				char **nl = (char **)realloc(list, (size_t)cap * sizeof *nl);
				if (!nl)
					continue;
				list = nl;
			}
			list[n] = strdup(src);
			if (list[n])
				n++;
		}
	}
	*n_out = n;
	return list;
}

bool rev_pack_write(const char *out_path, const char *scene_path,
                    char *err, size_t errcap)
{
	size_t scene_len = 0;
	u8 *scene = rev_read_file(scene_path, &scene_len);
	if (!scene) {
		snprintf(err, errcap, "cannot read '%s'", scene_path);
		return false;
	}

	RArena *ar = NULL;
	char perr[256] = { 0 };
	RSceneIR *ir = rev_parse_scene((const char *)scene, scene_len, &ar, perr, sizeof perr);
	if (!ir) {
		snprintf(err, errcap, "scene parse failed: %s", perr);
		free(scene);
		return false;
	}

	char *base = rev_path_dirname(scene_path);
	int n_assets = 0;
	char **names = collect_assets(ir, &n_assets);

	/* read every asset */
	u8 *adata[REV_PACK_MAX_ASSETS];
	size_t alen[REV_PACK_MAX_ASSETS];
	char *akeys[REV_PACK_MAX_ASSETS];
	int ok_assets = 0;
	for (int i = 0; i < n_assets && ok_assets < REV_PACK_MAX_ASSETS; i++) {
		char *full = rev_path_join(base, names[i]);
		size_t l = 0;
		u8 *d = rev_read_file(full, &l);
		if (!d) {
			LOGW("pack: asset '%s' not found — scene will need it on disk", names[i]);
			free(full);
			continue;
		}
		adata[ok_assets] = d;
		alen[ok_assets] = l;
		akeys[ok_assets] = strdup(names[i]);
		ok_assets++;
		free(full);
	}

	/* layout */
	u32 name_blob_off = PACK_HEADER_SIZE + (u32)ok_assets * PACK_ENTRY_SIZE;
	u32 name_blob_len = 0;
	for (int i = 0; i < ok_assets; i++)
		name_blob_len += (u32)strlen(akeys[i]) + 1;
	u32 scene_off = name_blob_off + name_blob_len;
	u32 scene_len32 = (u32)scene_len;
	u32 data_off = scene_off + scene_len32 + 1;

	u64 total = data_off;
	for (int i = 0; i < ok_assets; i++)
		total += alen[i];
	if (total > 0x7fffffffull) {
		snprintf(err, errcap, "pack too large");
		goto fail;
	}

	u8 *out = (u8 *)calloc(1, (size_t)total + 16);
	if (!out) {
		snprintf(err, errcap, "out of memory");
		goto fail;
	}

	memcpy(out, REV_PACK_MAGIC, 6);
	out[6] = (u8)REV_PACK_VERSION;        /* u16 format version */
	out[7] = (u8)(REV_PACK_VERSION >> 8);
	pk_put32(out + 8, scene_off);
	pk_put32(out + 12, scene_len32);
	pk_put32(out + 16, (u32)ok_assets);
	pk_put32(out + 20, name_blob_off);
	pk_put32(out + 24, name_blob_len);
	pk_put32(out + 28, data_off);
	pk_put32(out + 32, 0);    /* header CRC, fixed below */
	out[36] = 0;
	out[37] = 1;
	out[38] = 0;
	out[39] = 0;              /* flags */

	/* name blob + entries */
	u32 no = 0;
	u64 doff = data_off;
	for (int i = 0; i < ok_assets; i++) {
		u8 *e = out + PACK_HEADER_SIZE + (size_t)i * PACK_ENTRY_SIZE;
		u32 nl = (u32)strlen(akeys[i]) + 1;
		memcpy(out + name_blob_off + no, akeys[i], nl);
		pk_put32(e, no);
		pk_put32(e + 4, nl);
		pk_put64(e + 8, doff);
		pk_put64(e + 16, alen[i]);
		pk_put32(e + 24, rev_crc32(adata[i], alen[i]));
		no += nl;
		doff += alen[i];
	}

	memcpy(out + scene_off, scene, scene_len);
	out[scene_off + scene_len] = 0;
	doff = data_off;
	for (int i = 0; i < ok_assets; i++) {
		memcpy(out + doff, adata[i], alen[i]);
		doff += alen[i];
	}

	pk_put32(out + 32, rev_crc32(out, 32));

	if (!rev_write_file(out_path, out, (size_t)total)) {
		snprintf(err, errcap, "cannot write '%s'", out_path);
		free(out);
		goto fail;
	}
	free(out);
	LOGI("pack: %d asset(s), %llu bytes -> %s", ok_assets,
	     (unsigned long long)total, out_path);

fail:
	for (int i = 0; i < ok_assets; i++) {
		free(adata[i]);
		free(akeys[i]);
	}
	for (int i = 0; i < n_assets; i++)
		free(names[i]);
	free(names);
	arena_destroy(ar);
	free(base);
	free(scene);
	return true;    /* ok_assets may be 0; missing assets were warned */
}

bool rev_pack_unpack(const char *pack_path, const char *out_dir,
                     char *err, size_t errcap)
{
	RevPack pk;
	if (!rev_pack_load(pack_path, &pk, err, errcap))
		return false;

	/* create the output directory (single level) */
	mkdir(out_dir, 0755);

	/* scene.rev */
	{
		char path[512];
		snprintf(path, sizeof path, "%s/scene.rev", out_dir);
		if (!rev_write_file(path, (const u8 *)pk.scene_text, pk.scene_len)) {
			snprintf(err, errcap, "cannot write '%s'", path);
			rev_pack_free(&pk);
			return false;
		}
	}

	for (int i = 0; i < pk.n_assets; i++) {
		char path[600];
		/* keep a flat filename: replace directory separators */
		char flat[512];
		snprintf(flat, sizeof flat, "%s", pk.assets[i].name);
		for (char *c = flat; *c; c++)
			if (*c == '/')
				*c = '_';
		snprintf(path, sizeof path, "%s/%s", out_dir, flat);
		if (!rev_write_file(path, pk.assets[i].data, pk.assets[i].len)) {
			snprintf(err, errcap, "cannot write '%s'", path);
			rev_pack_free(&pk);
			return false;
		}
		/* rewrite references in the scene text */
		LOGI("unpack: %s -> %s", pk.assets[i].name, flat);
	}

	/* rewrite the scene's image paths to flat names */
	{
		char path[512];
		snprintf(path, sizeof path, "%s/scene.rev", out_dir);
		size_t len = 0;
		u8 *text = rev_read_file(path, &len);
		if (text) {
			for (int i = 0; i < pk.n_assets; i++) {
				char flat[512];
				snprintf(flat, sizeof flat, "%s", pk.assets[i].name);
				for (char *c = flat; *c; c++)
					if (*c == '/')
						*c = '_';
				/* replace "assets/foo.svg" with "assets_foo.svg" */
				char needle[600], repl[600];
				snprintf(needle, sizeof needle, "\"%s\"", pk.assets[i].name);
				snprintf(repl, sizeof repl, "\"%s\"", flat);
				char *at = (char *)text;
				size_t nlen = strlen(needle), rlen = strlen(repl);
				while ((at = strstr(at, needle)) != NULL) {
					/* in-place replace (repl never longer than needle + _) */
					if (rlen <= nlen) {
						memmove(at, repl, rlen);
						memmove(at + rlen, at + nlen, strlen(at + nlen) + 1);
					}
					at += rlen;
				}
			}
			rev_write_file(path, text, strlen((char *)text));
			free(text);
		}
	}

	rev_pack_free(&pk);
	return true;
}
