/* ============================================================================
 * Reverie Engine — pack.h
 * The .revpack container: a scene's text plus every referenced asset
 * (PNG/SVG) in one checksummed binary file.
 *
 * Layout (all integers little-endian):
 *   0   magic "REVPAK\x01\x00"      (6B magic + u16 format version)
 *   8   u32 scene_off                offset of scene text
 *   12  u32 scene_len                scene text length (excl. NUL)
 *   16  u32 asset_count
 *   20  u32 name_blob_off            name/strings region
 *   24  u32 name_blob_len
 *   28  u32 data_off                 asset data region
 *   32  u32 header_crc32             CRC-32 of bytes 0..31
 *   36  u8  engine_version[3]        writer version (e.g. 0.1.0)
 *   39  u8  flags                    0
 *   40  asset entries, 28 bytes each:
 *         u32 name_off, u32 name_len (into the name blob)
 *         u64 data_off, u64 data_len
 *         u32 data_crc32
 *   ..  name blob, scene text (NUL-terminated), asset data blobs
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_PACK_H
#define REV_PACK_H

#include "rev_types.h"

#define REV_PACK_MAGIC "REVPAK"
#define REV_PACK_VERSION 1
#define REV_PACK_MAX_ASSETS 128

typedef struct {
	u8 *file;                    /* whole file in memory (owned)        */
	size_t file_len;
	const char *scene_text;      /* NUL-terminated view into file       */
	size_t scene_len;
	int n_assets;
	struct {
		const char *name;    /* view into file                      */
		const u8 *data;      /* view into file                      */
		size_t len;
		u32 crc;
	} assets[REV_PACK_MAX_ASSETS];
} RevPack;

/* Quick check: does the file start with the REVPAK magic? */
bool rev_pack_probe(const char *path);

/* Loads and validates (magic, bounds, header CRC, per-asset CRC). */
bool rev_pack_load(const char *path, RevPack *pk, char *err, size_t errcap);
void rev_pack_free(RevPack *pk);

/* Asset lookup by the path recorded in the scene; NULL when absent. */
const u8 *rev_pack_asset(const RevPack *pk, const char *name, size_t *len);

/* Builds a .revpack from a scene file, embedding every referenced asset
 * found next to it. Returns false with a message on failure. */
bool rev_pack_write(const char *out_path, const char *scene_path,
                    char *err, size_t errcap);

/* Extracts a .revpack back into a directory (revtool unpack). */
bool rev_pack_unpack(const char *pack_path, const char *out_dir,
                     char *err, size_t errcap);

#endif /* REV_PACK_H */
