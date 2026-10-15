/* ============================================================================
 * Reverie Engine — composite.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "composite.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SSE2__
#include <emmintrin.h>
#endif

const char *rev_blend_name(int mode)
{
	switch (mode) {
	case REV_BLEND_NORMAL:   return "normal";
	case REV_BLEND_ADD:      return "add";
	case REV_BLEND_SCREEN:   return "screen";
	case REV_BLEND_MULTIPLY: return "multiply";
	case REV_BLEND_OVERLAY:  return "overlay";
	default:                 return "?";
	}
}

/* --------------------------------------------------------------------------
 * blend core
 *
 * W3C compositing model per channel:
 *   Co = as*(1-ad)*Cs + as*ad*B(Cd, Cs) + (1-as)*ad*Cd
 *   Ao = as + ad*(1-as)
 * ------------------------------------------------------------------------ */

#define CLAMP01(x) ((x) < 0 ? 0 : ((x) > 1 ? 1 : (x)))

static inline f64 blend_fn(int mode, f64 cb, f64 cs)
{
	switch (mode) {
	case REV_BLEND_NORMAL:   return cs;
	case REV_BLEND_ADD:      return CLAMP01(cb + cs);
	case REV_BLEND_SCREEN:   return cb + cs - cb * cs;
	case REV_BLEND_MULTIPLY: return cb * cs;
	case REV_BLEND_OVERLAY:
		return cb <= 0.5 ? 2.0 * cb * cs : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
	default:                 return cs;
	}
}

static inline u32 blend_pixel(u32 d, u32 s, int mode, u32 amul)
{
	u32 sr = s & 0xff, sg = (s >> 8) & 0xff, sb = (s >> 16) & 0xff, sa = (s >> 24) & 0xff;
	u32 dr = d & 0xff, dg = (d >> 8) & 0xff, db = (d >> 16) & 0xff, da = (d >> 24) & 0xff;

	if (amul != 255)
		sa = (u32)(((u64)sa * amul) >> 8);
	if (sa == 0)
		return d;

	if (mode == REV_BLEND_NORMAL && sa == 255)
		return s;

	f64 as = sa / 255.0, ad = da / 255.0;
	f64 cs_r = sr / 255.0, cs_g = sg / 255.0, cs_b = sb / 255.0;
	f64 cd_r = dr / 255.0, cd_g = dg / 255.0, cd_b = db / 255.0;

	f64 ao = as + ad * (1.0 - as);
	f64 k1 = as * (1.0 - ad);
	f64 k3 = (1.0 - as) * ad;

	f64 or_ = (k1 * cs_r + k3 * cd_r + as * ad * blend_fn(mode, cd_r, cs_r)) / ao;
	f64 og  = (k1 * cs_g + k3 * cd_g + as * ad * blend_fn(mode, cd_g, cs_g)) / ao;
	f64 ob  = (k1 * cs_b + k3 * cd_b + as * ad * blend_fn(mode, cd_b, cs_b)) / ao;

	u32 ra = (u32)(ao * 255.0 + 0.5);
	u32 rr = (u32)(CLAMP01(or_) * 255.0 + 0.5);
	u32 rg = (u32)(CLAMP01(og) * 255.0 + 0.5);
	u32 rb = (u32)(CLAMP01(ob) * 255.0 + 0.5);
	return (ra > 255 ? 255 : ra) << 24 | (rb > 255 ? 255 : rb) << 16 |
	       (rg > 255 ? 255 : rg) << 8 | (rr > 255 ? 255 : rr);
}

void rev_comp_blend(RFb *dst, const RFb *src, int mode, f64 opacity)
{
	u32 amul = (u32)(rev_clampf(opacity, 0.0, 1.0) * 255.0 + 0.5);
	size_t n = (size_t)dst->w * dst->h;
	u32 *d = dst->px;
	const u32 *s = src->px;

	if (mode == REV_BLEND_NORMAL && amul == 255) {
		/* fast path: opaque copy where source alpha = 255, blend otherwise */
		for (size_t i = 0; i < n; i++) {
			u32 sa = s[i] >> 24;
			d[i] = (sa == 255) ? s[i] : blend_pixel(d[i], s[i], mode, amul);
		}
		return;
	}

	for (size_t i = 0; i < n; i++)
		d[i] = blend_pixel(d[i], s[i], mode, amul);
}

void rev_comp_blend_at(RFb *dst, const RFb *src, int dx, int dy,
                       int mode, f64 opacity)
{
	u32 amul = (u32)(rev_clampf(opacity, 0.0, 1.0) * 255.0 + 0.5);

	int x0 = dx < 0 ? -dx : 0;
	int y0 = dy < 0 ? -dy : 0;
	int x1 = (int)src->w - ((dx + (int)src->w > (int)dst->w) ? dx + (int)src->w - (int)dst->w : 0);
	int y1 = (int)src->h - ((dy + (int)src->h > (int)dst->h) ? dy + (int)src->h - (int)dst->h : 0);
	if (x1 <= x0 || y1 <= y0)
		return;

	for (int y = y0; y < y1; y++) {
		u32 *d = dst->px + (size_t)(y + dy) * dst->w + (size_t)(x0 + dx);
		const u32 *s = src->px + (size_t)y * src->w + x0;
		for (int x = x0; x < x1; x++)
			d[x - x0] = blend_pixel(d[x - x0], s[x - x0], mode, amul);
	}
}

/* --------------------------------------------------------------------------
 * blur (3-pass box, premultiplied)
 * ------------------------------------------------------------------------ */

static void blur_hpass(u32 *px, u32 w, u32 h, int r)
{
	if (r < 1 || w < 2)
		return;
	u32 *tmp = (u32 *)malloc((size_t)w * sizeof(u32));
	if (!tmp)
		return;

	u32 div = (u32)(2 * r + 1);
	for (u32 y = 0; y < h; y++) {
		u32 *row = px + (size_t)y * w;
		for (u32 x = 0; x < w; x++)
			tmp[x] = row[x];
		/* box smoothing with straight-alpha premultiplied accumulators */
		u32 ar = 0, ag = 0, ab = 0, aa = 0;
		for (int i = -r; i <= r; i++) {
			u32 x = (u32)rev_clamp_i32(i, 0, (int)w - 1);
			u32 p = tmp[x];
			u32 a = p >> 24;
			ar += (p & 0xff) * a;
			ag += ((p >> 8) & 0xff) * a;
			ab += ((p >> 16) & 0xff) * a;
			aa += a;
		}
		for (u32 x = 0; x < w; x++) {
			u32 a = (aa + div / 2) / div;
			u32 rr = a ? (ar + div / 2) / div / a : 0;
			u32 gg = a ? (ag + div / 2) / div / a : 0;
			u32 bb = a ? (ab + div / 2) / div / a : 0;
			row[x] = (a > 255 ? 255 : a) << 24 |
			         (bb > 255 ? 255 : bb) << 16 |
			         (gg > 255 ? 255 : gg) << 8 |
			         (rr > 255 ? 255 : rr);
			/* slide window */
			u32 xo = (u32)rev_clamp_i32((int)x - r, 0, (int)w - 1);
			u32 xi = (u32)rev_clamp_i32((int)x + r + 1, 0, (int)w - 1);
			u32 po = tmp[xo], pi = tmp[xi];
			u32 ao = po >> 24, ai = pi >> 24;
			ar += (pi & 0xff) * ai - (po & 0xff) * ao;
			ag += ((pi >> 8) & 0xff) * ai - ((po >> 8) & 0xff) * ao;
			ab += ((pi >> 16) & 0xff) * ai - ((po >> 16) & 0xff) * ao;
			aa += ai - ao;
		}
	}
	free(tmp);
}

static void blur_vpass(u32 *px, u32 w, u32 h, int r)
{
	if (r < 1 || h < 2)
		return;
	u32 *tmp = (u32 *)malloc((size_t)h * sizeof(u32));
	if (!tmp)
		return;
	u32 div = (u32)(2 * r + 1);

	for (u32 x = 0; x < w; x++) {
		u32 *col = px + x;
		for (u32 y = 0; y < h; y++)
			tmp[y] = col[(size_t)y * w];
		u32 ar = 0, ag = 0, ab = 0, aa = 0;
		for (int i = -r; i <= r; i++) {
			u32 y = (u32)rev_clamp_i32(i, 0, (int)h - 1);
			u32 p = tmp[y];
			u32 a = p >> 24;
			ar += (p & 0xff) * a;
			ag += ((p >> 8) & 0xff) * a;
			ab += ((p >> 16) & 0xff) * a;
			aa += a;
		}
		for (u32 y = 0; y < h; y++) {
			u32 a = (aa + div / 2) / div;
			u32 rr = a ? (ar + div / 2) / div / a : 0;
			u32 gg = a ? (ag + div / 2) / div / a : 0;
			u32 bb = a ? (ab + div / 2) / div / a : 0;
			col[(size_t)y * w] = (a > 255 ? 255 : a) << 24 |
			                     (bb > 255 ? 255 : bb) << 16 |
			                     (gg > 255 ? 255 : gg) << 8 |
			                     (rr > 255 ? 255 : rr);
			u32 yo = (u32)rev_clamp_i32((int)y - r, 0, (int)h - 1);
			u32 yi = (u32)rev_clamp_i32((int)y + r + 1, 0, (int)h - 1);
			u32 po = tmp[yo], pi = tmp[yi];
			u32 ao = po >> 24, ai = pi >> 24;
			ar += (pi & 0xff) * ai - (po & 0xff) * ao;
			ag += ((pi >> 8) & 0xff) * ai - ((po >> 8) & 0xff) * ao;
			ab += ((pi >> 16) & 0xff) * ai - ((po >> 16) & 0xff) * ao;
			aa += ai - ao;
		}
	}
	free(tmp);
}

void rev_blur(RFb *fb, f64 radius)
{
	if (radius < 0.5 || fb->w < 2 || fb->h < 2)
		return;
	int r = (int)(radius / 3.0 + 0.5);
	if (r < 1) r = 1;
	int rmax = (int)(fb->w > fb->h ? fb->h : fb->w) / 2 - 1;
	if (rmax < 1) return;
	if (r > rmax) r = rmax;

	for (int pass = 0; pass < 3; pass++) {
		blur_hpass(fb->px, fb->w, fb->h, r);
		blur_vpass(fb->px, fb->w, fb->h, r);
	}
}

/* --------------------------------------------------------------------------
 * theme-switch fade
 * ------------------------------------------------------------------------ */

void rev_comp_fade(RFb *dst, f64 black)
{
	if (!dst || !dst->px || black <= 0.0)
		return;
	u32 k = (u32)(rev_clampf(black, 0.0, 1.0) * 256.0 + 0.5);
	if (k > 256)
		k = 256;
	u32 keep = 256u - k;	/* 256 = untouched, 0 = fully black */
	size_t n = (size_t)dst->w * dst->h;

#ifdef __SSE2__
	/* 4 pixels per iteration, exact same rounding as the scalar path:
	 * each channel byte is isolated into the low byte of a u16 lane, scaled
	 * with (c*keep+128)>>8 (the value lands back in the lane's low byte),
	 * then shifted into its final position with a 32-bit lane shift. Alpha
	 * is ORed through untouched. c*keep+128 <= 255*256+128 fits u16. */
	if (n >= 4) {
		const __m128i keepv = _mm_set1_epi16((short)keep);
		const __m128i rnd = _mm_set1_epi16(128);
		const __m128i bytemask = _mm_set1_epi32(0x000000FFu);
		size_t n4 = n & ~(size_t)3;
		for (size_t i = 0; i < n4; i += 4) {
			__m128i v = _mm_loadu_si128((const __m128i *)(dst->px + i));
			__m128i a = _mm_and_si128(v, _mm_set1_epi32((int)0xFF000000u));
			/* isolate each channel into the low byte of its lanes */
			__m128i r = _mm_and_si128(v, bytemask);
			__m128i g = _mm_and_si128(_mm_srli_epi32(v, 8), bytemask);
			__m128i b = _mm_and_si128(_mm_srli_epi32(v, 16), bytemask);
			/* (c*keep+128)>>8: the scaled channel value (0..255) ends up in
			 * the LOW byte of each u16 lane = bits 0-7 of the 32-bit lane */
			r = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(r, keepv), rnd), 8);
			g = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(g, keepv), rnd), 8);
			b = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(b, keepv), rnd), 8);
			/* final positions: r stays at bits 0-7, g -> 8-15, b -> 16-23 */
			g = _mm_slli_epi32(g, 8);
			b = _mm_slli_epi32(b, 16);
			__m128i out = _mm_or_si128(a, _mm_or_si128(r, _mm_or_si128(g, b)));
			_mm_storeu_si128((__m128i *)(dst->px + i), out);
		}
		for (size_t i = n4; i < n; i++) {
			u32 p = dst->px[i];
			dst->px[i] = (p & 0xFF000000u) |
				     ((((p & 0xFF0000u) >> 16) * keep + 128u) >> 8) << 16 |
				     ((((p & 0x00FF00u) >> 8) * keep + 128u) >> 8) << 8 |
				     (((p & 0xFFu) * keep + 128u) >> 8);
		}
		return;
	}
#endif

	for (size_t i = 0; i < n; i++) {
		u32 p = dst->px[i];
		dst->px[i] = (p & 0xFF000000u) |
		             ((((p & 0xFF0000u) >> 16) * keep + 128u) >> 8) << 16 |
		             ((((p & 0x00FF00u) >> 8) * keep + 128u) >> 8) << 8 |
		             (((p & 0xFFu) * keep + 128u) >> 8);
	}
}

/* --------------------------------------------------------------------------
 * resampling
 * ------------------------------------------------------------------------ */

void rev_resample_bilinear(const u32 *src, u32 sw, u32 sh, u32 *dst, u32 dw, u32 dh)
{
	if (!src || !dst || sw == 0 || sh == 0 || dw == 0 || dh == 0)
		return;
	f64 xr = (f64)sw / dw;
	f64 yr = (f64)sh / dh;

	for (u32 y = 0; y < dh; y++) {
		f64 sy = ((f64)y + 0.5) * yr - 0.5;
		f64 y0f = floor(sy);
		int y0 = (int)y0f;
		f64 fy = sy - y0f;
		int y1 = y0 + 1;
		y0 = rev_clamp_i32(y0, 0, (int)sh - 1);
		y1 = rev_clamp_i32(y1, 0, (int)sh - 1);

		for (u32 x = 0; x < dw; x++) {
			f64 sx = ((f64)x + 0.5) * xr - 0.5;
			f64 x0f = floor(sx);
			int x0 = (int)x0f;
			f64 fx = sx - x0f;
			int x1 = x0 + 1;
			x0 = rev_clamp_i32(x0, 0, (int)sw - 1);
			x1 = rev_clamp_i32(x1, 0, (int)sw - 1);

			u32 p00 = src[(size_t)y0 * sw + x0];
			u32 p10 = src[(size_t)y0 * sw + x1];
			u32 p01 = src[(size_t)y1 * sw + x0];
			u32 p11 = src[(size_t)y1 * sw + x1];

			/* premultiplied interpolation to avoid halos */
			u32 a00 = p00 >> 24, a10 = p10 >> 24, a01 = p01 >> 24, a11 = p11 >> 24;
			u32 wa = (u32)((a00 * (1 - fx) + a10 * fx) * (1 - fy) +
			               (a01 * (1 - fx) + a11 * fx) * fy + 0.5);
			u32 wr = 0, wg = 0, wb = 0;
			if (wa) {
				f64 pr = (p00 & 0xff) * a00 * (1 - fx) * (1 - fy) +
				         (p10 & 0xff) * a10 * fx * (1 - fy) +
				         (p01 & 0xff) * a01 * (1 - fx) * fy +
				         (p11 & 0xff) * a11 * fx * fy;
				f64 pg = ((p00 >> 8) & 0xff) * a00 * (1 - fx) * (1 - fy) +
				         ((p10 >> 8) & 0xff) * a10 * fx * (1 - fy) +
				         ((p01 >> 8) & 0xff) * a01 * (1 - fx) * fy +
				         ((p11 >> 8) & 0xff) * a11 * fx * fy;
				f64 pb = ((p00 >> 16) & 0xff) * a00 * (1 - fx) * (1 - fy) +
				         ((p10 >> 16) & 0xff) * a10 * fx * (1 - fy) +
				         ((p01 >> 16) & 0xff) * a01 * (1 - fx) * fy +
				         ((p11 >> 16) & 0xff) * a11 * fx * fy;
				wr = (u32)(pr / wa + 0.5);
				wg = (u32)(pg / wa + 0.5);
				wb = (u32)(pb / wa + 0.5);
			}
			dst[(size_t)y * dw + x] =
				(wa > 255 ? 255 : wa) << 24 |
				(wb > 255 ? 255 : wb) << 16 |
				(wg > 255 ? 255 : wg) << 8 |
				(wr > 255 ? 255 : wr);
		}
	}
}

void rev_resample_nearest(const u32 *src, u32 sw, u32 sh, u32 *dst, u32 dw, u32 dh)
{
	for (u32 y = 0; y < dh; y++) {
		u32 sy = (u32)(((f64)y + 0.5) * sh / dh);
		if (sy >= sh) sy = sh - 1;
		for (u32 x = 0; x < dw; x++) {
			u32 sx = (u32)(((f64)x + 0.5) * sw / dw);
			if (sx >= sw) sx = sw - 1;
			dst[(size_t)y * dw + x] = src[(size_t)sy * sw + sx];
		}
	}
}

void rev_blit(RFb *dst, const RFb *src, int dx, int dy, u32 alpha_mul)
{
	int x0 = dx < 0 ? -dx : 0;
	int y0 = dy < 0 ? -dy : 0;
	int x1 = (int)src->w - ((dx + (int)src->w > (int)dst->w) ? dx + (int)src->w - (int)dst->w : 0);
	int y1 = (int)src->h - ((dy + (int)src->h > (int)dst->h) ? dy + (int)src->h - (int)dst->h : 0);
	if (x1 <= x0 || y1 <= y0)
		return;

	if (alpha_mul >= 255) {
		for (int y = y0; y < y1; y++)
			memcpy(dst->px + (size_t)(y + dy) * dst->w + (size_t)(x0 + dx),
			       src->px + (size_t)y * src->w + x0,
			       (size_t)(x1 - x0) * 4);
		return;
	}
	for (int y = y0; y < y1; y++) {
		u32 *d = dst->px + (size_t)(y + dy) * dst->w + (size_t)(x0 + dx);
		const u32 *s = src->px + (size_t)y * src->w + x0;
		for (int x = x0; x < x1; x++) {
			u32 p = s[x - x0];
			u32 a = (u32)(((u64)(p >> 24) * alpha_mul) >> 8);
			if (a == 255) {
				d[x - x0] = p;
			} else if (a > 0) {
				/* straight-alpha scale then over */
				u32 r = (u32)(((u64)(p & 0xff) * a) / (p >> 24));
				u32 g = (u32)(((u64)((p >> 8) & 0xff) * a) / (p >> 24));
				u32 b = (u32)(((u64)((p >> 16) & 0xff) * a) / (p >> 24));
				u32 q = a << 24 | b << 16 | g << 8 | r;
				d[x - x0] = blend_pixel(d[x - x0], q, REV_BLEND_NORMAL, 255);
			}
		}
	}
}
