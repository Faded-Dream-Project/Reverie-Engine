/* ============================================================================
 * Reverie Engine — svg.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "svg.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "geom.h"
#include "log.h"
#include "raster.h"
#include "util.h"

#define REV_PI 3.14159265358979323846
#define FLATTEN_TOL 0.25

/* --------------------------------------------------------------------------
 * tiny DOM
 * ------------------------------------------------------------------------ */

typedef struct XAttr {
	const char *name;
	const char *val;
} XAttr;

typedef struct XNode {
	const char *tag;
	XAttr      *attrs;
	size_t      nattrs;
	struct XNode **kids;
	size_t      nkids;
	struct XNode *parent;
	void       *lpath;      /* prebuilt local-space path for shapes (RPath*) */
} XNode;

struct RevSvg {
	RArena *dom;        /* DOM + prebuilt local-space paths    */
	RArena *scratch;    /* per-render flatten/stroke scratch   */
	XNode  *root;
	f64     vw, vh;     /* declared viewport size (user units) */
};

static const char *xget(const XNode *n, const char *name)
{
	for (size_t i = 0; i < n->nattrs; i++)
		if (strcmp(n->attrs[i].name, name) == 0)
			return n->attrs[i].val;
	return NULL;
}

/* --------------------------------------------------------------------------
 * XML parsing (subset: elements, attributes, entities; comments/PIs/doctype
 * and CDATA are skipped; text content is ignored)
 * ------------------------------------------------------------------------ */

typedef struct {
	const char *p, *end;
	RArena *ar;
	char err[128];
	bool failed;
} Xml;

static bool xml_fail(Xml *x, const char *msg)
{
	if (!x->failed) {
		snprintf(x->err, sizeof x->err, "%s", msg);
		x->failed = true;
	}
	return false;
}

static void xml_ws(Xml *x)
{
	while (x->p < x->end && isspace((unsigned char)*x->p))
		x->p++;
}

static bool xml_name(Xml *x, const char **s, size_t *n)
{
	xml_ws(x);
	const char *st = x->p;
	while (x->p < x->end && (isalnum((unsigned char)*x->p) || *x->p == '_' ||
	       *x->p == '-' || *x->p == ':' || *x->p == '.'))
		x->p++;
	if (x->p == st)
		return false;
	*s = st;
	*n = (size_t)(x->p - st);
	return true;
}

/* decode entities into an arena string */
static char *xml_decode(Xml *x, const char *s, size_t n)
{
	char *out = (char *)arena_alloc(x->ar, n + 1);
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '&') {
			size_t semi = i + 1;
			while (semi < n && s[semi] != ';')
				semi++;
			if (semi < n) {
				size_t elen = semi - i - 1;
				const char *e = s + i + 1;
				if (elen == 3 && strncmp(e, "amp", 3) == 0)      out[o++] = '&';
				else if (elen == 2 && strncmp(e, "lt", 2) == 0)  out[o++] = '<';
				else if (elen == 2 && strncmp(e, "gt", 2) == 0)  out[o++] = '>';
				else if (elen == 4 && strncmp(e, "quot", 4) == 0) out[o++] = '"';
				else if (elen == 4 && strncmp(e, "apos", 4) == 0) out[o++] = '\'';
				else if (elen > 1 && e[0] == '#') {
					long v = 0;
					if (elen > 2 && (e[1] == 'x' || e[1] == 'X'))
						v = strtol(e + 2, NULL, 16);
					else
						v = strtol(e + 1, NULL, 10);
					if (v > 0 && v < 256)
						out[o++] = (char)v;
				} else {
					out[o++] = s[i];
				}
				i = semi;
				continue;
			}
		}
		out[o++] = s[i];
	}
	out[o] = '\0';
	return out;
}

static XNode *xml_node(Xml *x)
{
	XNode *n = (XNode *)arena_zalloc(x->ar, sizeof(XNode));
	return n;
}

static void xnode_add(XNode *parent, XNode *kid, RArena *ar)
{
	kid->parent = parent;
	if (parent->nkids % 8 == 0) {
		XNode **nk = (XNode **)arena_alloc(ar, (parent->nkids + 8) * sizeof(XNode *));
		if (parent->nkids)
			memcpy(nk, parent->kids, parent->nkids * sizeof(XNode *));
		parent->kids = nk;
	}
	parent->kids[parent->nkids++] = kid;
}

static bool xml_skip_misc(Xml *x)
{
	for (;;) {
		/* UTF-8 BOM: meaningful only at the document start, but a BOM
		 * in misc position is 3 ignorable bytes wherever it appears —
		 * tools write them, and mid-document ones would otherwise be
		 * misread as text anyway. */
		if (x->p + 3 <= x->end && (unsigned char)x->p[0] == 0xEF &&
		    (unsigned char)x->p[1] == 0xBB &&
		    (unsigned char)x->p[2] == 0xBF) {
			x->p += 3;
			continue;
		}
		/* XML's Misc is whitespace-interleaved: prolog = BOM? Misc*,
		 * Misc ::= Comment | PI | S. Skipping whitespace here is what
		 * makes the document prolog — the <?xml ...?> declaration
		 * every real exporter writes, plus the newline after it, plus
		 * any generator comments — reach the <svg> element instead of
		 * dying as "not an <svg> document" on the whitespace between
		 * the declaration and the root. */
		xml_ws(x);
		if (x->p + 4 <= x->end && strncmp(x->p, "<!--", 4) == 0) {
			const char *e = x->p + 4;
			while (e + 3 <= x->end && strncmp(e, "-->", 3) != 0)
				e++;
			if (e + 3 > x->end)
				return xml_fail(x, "unterminated comment");
			x->p = e + 3;
		} else if (x->p + 2 <= x->end && strncmp(x->p, "<?", 2) == 0) {
			const char *e = x->p + 2;
			while (e + 1 <= x->end && strncmp(e, "?>", 2) != 0)
				e++;
			if (e + 2 > x->end)
				return xml_fail(x, "unterminated processing instruction");
			x->p = e + 2;
		} else if (x->p + 2 <= x->end && strncmp(x->p, "<!", 2) == 0) {
			/* DOCTYPE / CDATA-ish: skip to matching '>' (naive, no nested decls) */
			const char *e = x->p + 2;
			while (e < x->end && *e != '>')
				e++;
			if (e >= x->end)
				return xml_fail(x, "unterminated declaration");
			x->p = e + 1;
		} else {
			return true;
		}
	}
}

static XNode *xml_element(Xml *x, XNode *parent);

static XNode *xml_element(Xml *x, XNode *parent)
{
	if (x->p >= x->end || *x->p != '<')
		return NULL;

	if (x->p + 9 <= x->end && strncmp(x->p, "<![CDATA[", 9) == 0) {
		const char *e = x->p + 9;
		while (e + 3 <= x->end && strncmp(e, "]]>", 3) != 0)
			e++;
		x->p = (e + 3 <= x->end) ? e + 3 : x->end;
		return (XNode *)1;   /* sentinel: skipped chunk */
	}

	const char *sav = x->p;
	x->p++;
	const char *name;
	size_t nlen;
	if (!xml_name(x, &name, &nlen)) {
		x->p = sav;
		return NULL;
	}

	XNode *n = xml_node(x);
	n->tag = arena_strndup(x->ar, name, nlen);

	/* attributes */
	for (;;) {
		xml_ws(x);
		if (x->p >= x->end)
			return xml_fail(x, "EOF in tag"), (XNode *)NULL;
		if (*x->p == '>' || (x->p + 1 < x->end && *x->p == '/' && x->p[1] == '>')) {
			bool selfclose = (*x->p == '/');
			x->p += selfclose ? 2 : 1;
			if (parent)
				xnode_add(parent, n, x->ar);
			if (!selfclose) {
				/* children until </tag> */
				for (;;) {
					xml_skip_misc(x);
					if (x->p >= x->end)
						return xml_fail(x, "unexpected EOF"), (XNode *)NULL;
					if (*x->p == '<') {
						if (x->p + 1 < x->end && x->p[1] == '/') {
							x->p += 2;
							const char *ename;
							size_t elen2;
							xml_name(x, &ename, &elen2);
							xml_ws(x);
							if (x->p < x->end && *x->p == '>')
								x->p++;
							break;
						}
						XNode *kid = xml_element(x, n);
						if (x->failed)
							return NULL;
						if ((void *)kid == (void *)1)
							continue;
						if (!kid)
							return xml_fail(x, "bad child element"), (XNode *)NULL;
					} else {
						/* text node: skip */
						while (x->p < x->end && *x->p != '<')
							x->p++;
					}
				}
			}
			return n;
		}
		const char *aname;
		size_t anlen;
		if (!xml_name(x, &aname, &anlen))
			return xml_fail(x, "bad attribute"), (XNode *)NULL;
		xml_ws(x);
		if (x->p < x->end && *x->p == '=') {
			x->p++;
			xml_ws(x);
			if (x->p < x->end && (*x->p == '"' || *x->p == '\'')) {
				char q = *x->p++;
				const char *vst = x->p;
				while (x->p < x->end && *x->p != q)
					x->p++;
				if (x->p >= x->end)
					return xml_fail(x, "unterminated attribute value"), (XNode *)NULL;
				if (n->nattrs % 8 == 0) {
					XAttr *na = (XAttr *)arena_alloc(x->ar, (n->nattrs + 8) * sizeof(XAttr));
					if (n->nattrs)
						memcpy(na, n->attrs, n->nattrs * sizeof(XAttr));
					n->attrs = na;
				}
				n->attrs[n->nattrs].name = arena_strndup(x->ar, aname, anlen);
				n->attrs[n->nattrs].val = xml_decode(x, vst, (size_t)(x->p - vst));
				n->nattrs++;
				x->p++;
			} else {
				const char *vst = x->p;
				while (x->p < x->end && !isspace((unsigned char)*x->p) &&
				       *x->p != '>' && *x->p != '/')
					x->p++;
				if (n->nattrs % 8 == 0) {
					XAttr *na = (XAttr *)arena_alloc(x->ar, (n->nattrs + 8) * sizeof(XAttr));
					if (n->nattrs)
						memcpy(na, n->attrs, n->nattrs * sizeof(XAttr));
					n->attrs = na;
				}
				n->attrs[n->nattrs].name = arena_strndup(x->ar, aname, anlen);
				n->attrs[n->nattrs].val = xml_decode(x, vst, (size_t)(x->p - vst));
				n->nattrs++;
			}
		} else {
			if (n->nattrs % 8 == 0) {
				XAttr *na = (XAttr *)arena_alloc(x->ar, (n->nattrs + 8) * sizeof(XAttr));
				if (n->nattrs)
					memcpy(na, n->attrs, n->nattrs * sizeof(XAttr));
				n->attrs = na;
			}
			n->attrs[n->nattrs].name = arena_strndup(x->ar, aname, anlen);
			n->attrs[n->nattrs].val = "";
			n->nattrs++;
		}
	}
}

/* --------------------------------------------------------------------------
 * named colors (SVG/CSS)
 * ------------------------------------------------------------------------ */

typedef struct { const char *n; u32 rgb; } NamedColor;

static const NamedColor NAMED[] = {
	{"aliceblue",0xF0F8FF},{"antiquewhite",0xFAEBD7},{"aqua",0x00FFFF},
	{"aquamarine",0x7FFFD4},{"azure",0xF0FFFF},{"beige",0xF5F5DC},
	{"bisque",0xFFE4C4},{"black",0x000000},{"blanchedalmond",0xFFEBCD},
	{"blue",0x0000FF},{"blueviolet",0x8A2BE2},{"brown",0xA52A2A},
	{"burlywood",0xDEB887},{"cadetblue",0x5F9EA0},{"chartreuse",0x7FFF00},
	{"chocolate",0xD2691E},{"coral",0xFF7F50},{"cornflowerblue",0x6495ED},
	{"cornsilk",0xFFF8DC},{"crimson",0xDC143C},{"cyan",0x00FFFF},
	{"darkblue",0x00008B},{"darkcyan",0x008B8B},{"darkgoldenrod",0xB8860B},
	{"darkgray",0xA9A9A9},{"darkgreen",0x006400},{"darkgrey",0xA9A9A9},
	{"darkkhaki",0xBDB76B},{"darkmagenta",0x8B008B},{"darkolivegreen",0x556B2F},
	{"darkorange",0xFF8C00},{"darkorchid",0x9932CC},{"darkred",0x8B0000},
	{"darksalmon",0xE9967A},{"darkseagreen",0x8FBC8F},{"darkslateblue",0x483D8B},
	{"darkslategray",0x2F4F4F},{"darkslategrey",0x2F4F4F},{"darkturquoise",0x00CED1},
	{"darkviolet",0x9400D3},{"deeppink",0xFF1493},{"deepskyblue",0x00BFFF},
	{"dimgray",0x696969},{"dimgrey",0x696969},{"dodgerblue",0x1E90FF},
	{"firebrick",0xB22222},{"floralwhite",0xFFFAF0},{"forestgreen",0x228B22},
	{"fuchsia",0xFF00FF},{"gainsboro",0xDCDCDC},{"ghostwhite",0xF8F8FF},
	{"gold",0xFFD700},{"goldenrod",0xDAA520},{"gray",0x808080},
	{"green",0x008000},{"greenyellow",0xADFF2F},{"grey",0x808080},
	{"honeydew",0xF0FFF0},{"hotpink",0xFF69B4},{"indianred",0xCD5C5C},
	{"indigo",0x4B0082},{"ivory",0xFFFFF0},{"khaki",0xF0E68C},
	{"lavender",0xE6E6FA},{"lavenderblush",0xFFF0F5},{"lawngreen",0x7CFC00},
	{"lemonchiffon",0xFFFACD},{"lightblue",0xADD8E6},{"lightcoral",0xF08080},
	{"lightcyan",0xE1FFFF},{"lightgoldenrodyellow",0xFAFAD2},{"lightgray",0xD3D3D3},
	{"lightgreen",0x90EE90},{"lightgrey",0xD3D3D3},{"lightpink",0xFFB6C1},
	{"lightsalmon",0xFFA07A},{"lightseagreen",0x20B2AA},{"lightskyblue",0x87CEFA},
	{"lightslategray",0x778899},{"lightslategrey",0x778899},{"lightsteelblue",0xB0C4DE},
	{"lightyellow",0xFFFFE0},{"lime",0x00FF00},{"limegreen",0x32CD32},
	{"linen",0xFAF0E6},{"magenta",0xFF00FF},{"maroon",0x800000},
	{"mediumaquamarine",0x66CDAA},{"mediumblue",0x0000CD},{"mediumorchid",0xBA55D3},
	{"mediumpurple",0x9370DB},{"mediumseagreen",0x3CB371},{"mediumslateblue",0x7B68EE},
	{"mediumspringgreen",0x00FA9A},{"mediumturquoise",0x48D1CC},{"mediumvioletred",0xC71585},
	{"midnightblue",0x191970},{"mintcream",0xF5FFFA},{"mistyrose",0xFFE4E1},
	{"moccasin",0xFFE4B5},{"navajowhite",0xFFDEAD},{"navy",0x000080},
	{"oldlace",0xFDF5E6},{"olive",0x808000},{"olivedrab",0x6B8E23},
	{"orange",0xFFA500},{"orangered",0xFF4500},{"orchid",0xDA70D6},
	{"palegoldenrod",0xEEE8AA},{"palegreen",0x98FB98},{"paleturquoise",0xAFEEEE},
	{"palevioletred",0xDB7093},{"papayawhip",0xFFEFD5},{"peachpuff",0xFFDAB9},
	{"peru",0xCD853F},{"pink",0xFFC0CB},{"plum",0xDDA0DD},
	{"powderblue",0xB0E0E6},{"purple",0x800080},{"red",0xFF0000},
	{"rosybrown",0xBC8F8F},{"royalblue",0x4169E1},{"saddlebrown",0x8B4513},
	{"salmon",0xFA8072},{"sandybrown",0xF4A460},{"seagreen",0x2E8B57},
	{"seashell",0xFFF5EE},{"sienna",0xA0522D},{"silver",0xC0C0C0},
	{"skyblue",0x87CEEB},{"slateblue",0x6A5ACD},{"slategray",0x708090},
	{"slategrey",0x708090},{"snow",0xFFFAFA},{"springgreen",0x00FF7F},
	{"steelblue",0x4682B4},{"tan",0xD2B48C},{"teal",0x008080},
	{"thistle",0xD8BFD8},{"tomato",0xFF6347},{"turquoise",0x40E0D0},
	{"violet",0xEE82EE},{"wheat",0xF5DEB3},{"white",0xFFFFFF},
	{"whitesmoke",0xF5F5F5},{"yellow",0xFFFF00},{"yellowgreen",0x9ACD32},
};

/* --------------------------------------------------------------------------
 * value parsing
 * ------------------------------------------------------------------------ */

static bool svg_num(const char *s, f64 *out)
{
	if (!s || !*s)
		return false;
	const char *pos = NULL;
	f64 v;
	if (!rev_parse_f64(s, strlen(s), &pos, &v))
		return false;
	/* allow a single trailing unit suffix */
	while (pos && *pos && isspace((unsigned char)*pos))
		pos++;
	if (pos && *pos && strchr("pxemex%", *pos))
		return false;   /* units handled by callers where meaningful */
	*out = v;
	return true;
}

/* number with optional unit; % resolved against `base` */
static bool svg_len(const char *s, f64 base, f64 *out)
{
	if (!s || !*s)
		return false;
	const char *pos = NULL;
	f64 v;
	if (!rev_parse_f64(s, strlen(s), &pos, &v))
		return false;
	while (pos && *pos && isspace((unsigned char)*pos))
		pos++;
	if (pos && *pos == '%') {
		*out = v / 100.0 * base;
		return true;
	}
	/* ignore absolute units: treat as user units (px-equivalent) */
	*out = v;
	return true;
}

static bool svg_color(const char *s, RColor *out)
{
	if (!s || !*s)
		return false;
	while (*s && isspace((unsigned char)*s))
		s++;
	if (*s == '#')
		return rev_parse_hex_color(s, strlen(s), out);

	if (strncasecmp(s, "rgb(", 4) == 0 || strncasecmp(s, "rgba(", 5) == 0) {
		bool rgba = tolower((unsigned char)s[3]) == 'a';
		const char *p = s + (rgba ? 5 : 4);
		f64 c[4] = { 0, 0, 0, 255 };
		int got = 0;
		for (int i = 0; i < (rgba ? 4 : 3); i++) {
			while (*p && (isspace((unsigned char)*p) || *p == ','))
				p++;
			if (!*p)
				break;
			const char *st = p;
			bool pct = false;
			f64 v = strtod(p, (char **)&p);
			if (*p == '%') { pct = true; p++; }
			c[i] = pct ? v * 255.0 / 100.0 : v;
			if (i == 3 && !pct)
				c[i] = v * 255.0;   /* rgba() alpha in 0..1 */
			(void)st;
			got++;
		}
		if (got < 3)
			return false;
		out->r = (u8)rev_clampf(c[0] + 0.5, 0, 255);
		out->g = (u8)rev_clampf(c[1] + 0.5, 0, 255);
		out->b = (u8)rev_clampf(c[2] + 0.5, 0, 255);
		out->a = (u8)rev_clampf(c[3] + 0.5, 0, 255);
		return true;
	}

	/* named */
	for (size_t i = 0; i < sizeof NAMED / sizeof NAMED[0]; i++)
		if (strcasecmp(s, NAMED[i].n) == 0) {
			out->r = (u8)(NAMED[i].rgb >> 16);
			out->g = (u8)(NAMED[i].rgb >> 8);
			out->b = (u8)(NAMED[i].rgb);
			out->a = 255;
			return true;
		}
	return false;
}

/* --------------------------------------------------------------------------
 * transform parsing
 * ------------------------------------------------------------------------ */

static bool parse_transform(RMat *m, const char *s)
{
	*m = rmat_identity();
	if (!s)
		return true;
	const char *p = s;
	while (*p) {
		while (*p && (isspace((unsigned char)*p) || *p == ','))
			p++;
		if (!*p)
			break;
		char fn[16] = { 0 };
		int fl = 0;
		while (*p && *p != '(' && fl < 15)
			fn[fl++] = *p++;
		if (*p != '(')
			return false;
		p++;
		f64 v[6] = { 0, 0, 0, 0, 0, 0 };
		int nv = 0;
		while (nv < 6) {
			while (*p && (isspace((unsigned char)*p) || *p == ','))
				p++;
			if (*p == ')' || !*p)
				break;
			const char *pos = NULL;
			if (!rev_parse_f64(p, strlen(p), &pos, &v[nv]))
				return false;
			p = pos;
			nv++;
		}
		while (*p && *p != ')')
			p++;
		if (*p == ')')
			p++;

		RMat t;
		if (strcmp(fn, "translate") == 0) {
			t = rmat_translate(v[0], nv > 1 ? v[1] : 0.0);
		} else if (strcmp(fn, "scale") == 0) {
			f64 sy = nv > 1 ? v[1] : v[0];
			t = rmat_scale(v[0], sy);
		} else if (strcmp(fn, "rotate") == 0) {
			f64 rad = v[0] * REV_PI / 180.0;
			if (nv > 2)
				t = rmat_rotate_at(rad, v[1], v[2]);
			else
				t = rmat_rotate(rad);
		} else if (strcmp(fn, "skewX") == 0) {
			t = rmat_skew_x(v[0] * REV_PI / 180.0);
		} else if (strcmp(fn, "skewY") == 0) {
			t = rmat_skew_y(v[0] * REV_PI / 180.0);
		} else if (strcmp(fn, "matrix") == 0 && nv == 6) {
			t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5];
		} else {
			return false;
		}
		*m = rmat_mul(*m, t);
	}
	return true;
}

/* --------------------------------------------------------------------------
 * path data parsing (full command set)
 * ------------------------------------------------------------------------ */

typedef struct {
	const char *p;
	bool err;
} PScan;

static void ps_ws(PScan *s)
{
	while (*s->p && isspace((unsigned char)*s->p))
		s->p++;
}

static bool ps_num(PScan *s, f64 *out)
{
	ps_ws(s);
	const char *st = s->p;
	char *end = NULL;
	errno = 0;
	f64 v = strtod(s->p, &end);
	if (end == s->p)
		return false;
	s->p = end;
	(void)st;
	*out = v;
	return true;
}

static bool ps_flag(PScan *s, bool *out)
{
	ps_ws(s);
	if (*s->p == '0' || *s->p == '1') {
		*out = (*s->p == '1');
		s->p++;
		return true;
	}
	return false;
}

static bool parse_path_data(RPath *dst, const char *d)
{
	PScan s = { d, false };
	f64 x = 0, y = 0, sx = 0, sy = 0;      /* current + subpath start */
	f64 pcx = 0, pcy = 0;                   /* previous cubic ctrl (for S) */
	f64 pqx = 0, pqy = 0;                   /* previous quad ctrl (for T) */
	char cmd = 0;
	bool have_cmd = false;

	while (*s.p) {
		ps_ws(&s);
		if (!*s.p)
			break;
		if (isalpha((unsigned char)*s.p)) {
			cmd = *s.p++;
			have_cmd = true;
		} else if (!have_cmd) {
			return false;
		}
		/* else: implicit repeat of previous command */

		bool rel = islower((unsigned char)cmd);
		char C = (char)toupper((unsigned char)cmd);
		f64 v[7];

		switch (C) {
		case 'M':
			if (!ps_num(&s, &v[0]) || !ps_num(&s, &v[1])) return false;
			if (rel) { v[0] += x; v[1] += y; }
			rpath_move_to(dst, (Vec2){ v[0], v[1] });
			x = sx = v[0]; y = sy = v[1];
			cmd = rel ? 'l' : 'L';   /* subsequent pairs are line-tos */
			break;
		case 'L':
			if (!ps_num(&s, &v[0]) || !ps_num(&s, &v[1])) return false;
			if (rel) { v[0] += x; v[1] += y; }
			rpath_line_to(dst, (Vec2){ v[0], v[1] });
			x = v[0]; y = v[1];
			break;
		case 'H':
			if (!ps_num(&s, &v[0])) return false;
			v[0] = rel ? x + v[0] : v[0];
			rpath_line_to(dst, (Vec2){ v[0], y });
			x = v[0];
			break;
		case 'V':
			if (!ps_num(&s, &v[0])) return false;
			v[0] = rel ? y + v[0] : v[0];
			rpath_line_to(dst, (Vec2){ x, v[0] });
			y = v[0];
			break;
		case 'C':
			for (int i = 0; i < 6; i++)
				if (!ps_num(&s, &v[i])) return false;
			if (rel) for (int i = 0; i < 6; i += 2) { v[i] += x; v[i+1] += y; }
			rpath_cubic_to(dst, (Vec2){ v[0], v[1] }, (Vec2){ v[2], v[3] }, (Vec2){ v[4], v[5] });
			pcx = v[2]; pcy = v[3];
			x = v[4]; y = v[5];
			break;
		case 'S':
			for (int i = 0; i < 4; i++)
				if (!ps_num(&s, &v[i])) return false;
			if (rel) for (int i = 0; i < 4; i += 2) { v[i] += x; v[i+1] += y; }
			{ Vec2 c1 = { 2 * x - pcx, 2 * y - pcy };
			  rpath_cubic_to(dst, c1, (Vec2){ v[0], v[1] }, (Vec2){ v[2], v[3] }); }
			pcx = v[0]; pcy = v[1];
			x = v[2]; y = v[3];
			break;
		case 'Q':
			for (int i = 0; i < 4; i++)
				if (!ps_num(&s, &v[i])) return false;
			if (rel) for (int i = 0; i < 4; i += 2) { v[i] += x; v[i+1] += y; }
			rpath_quad_to(dst, (Vec2){ v[0], v[1] }, (Vec2){ v[2], v[3] });
			pqx = v[0]; pqy = v[1];
			x = v[2]; y = v[3];
			break;
		case 'T':
			for (int i = 0; i < 2; i++)
				if (!ps_num(&s, &v[i])) return false;
			if (rel) { v[0] += x; v[1] += y; }
			pqx = 2 * x - pqx; pqy = 2 * y - pqy;
			rpath_quad_to(dst, (Vec2){ pqx, pqy }, (Vec2){ v[0], v[1] });
			x = v[0]; y = v[1];
			break;
		case 'A':
			for (int i = 0; i < 7; i++) {
				if (i == 3 || i == 4) {
					bool fl;
					if (!ps_flag(&s, &fl)) return false;
					v[i] = fl ? 1 : 0;
				} else {
					if (!ps_num(&s, &v[i])) return false;
				}
			}
			if (rel) { v[5] += x; v[6] += y; }
			rpath_arc_to(dst, v[0], v[1], v[2], v[3] != 0, v[4] != 0, (Vec2){ v[5], v[6] });
			x = v[5]; y = v[6];
			break;
		case 'Z':
			rpath_close(dst);
			x = sx; y = sy;
			break;
		default:
			return false;
		}
	}
	return true;
}

/* --------------------------------------------------------------------------
 * shapes -> paths
 * ------------------------------------------------------------------------ */

static void shape_rect(RPath *p, const XNode *n)
{
	const char *as;
	f64 x = 0, y = 0, w = 0, h = 0, rx = -1, ry = -1;
	if ((as = xget(n, "x")) && !svg_len(as, 0, &x)) return;
	if ((as = xget(n, "y")) && !svg_len(as, 0, &y)) return;
	if ((as = xget(n, "width")) && !svg_len(as, 0, &w)) return;
	if ((as = xget(n, "height")) && !svg_len(as, 0, &h)) return;
	if ((as = xget(n, "rx")) && !svg_len(as, 0, &rx)) return;
	if ((as = xget(n, "ry")) && !svg_len(as, 0, &ry)) return;
	if (w <= 0 || h <= 0)
		return;
	if (rx < 0 && ry >= 0) rx = ry;
	if (ry < 0 && rx >= 0) ry = rx;
	if (rx < 0) { rx = 0; ry = 0; }
	if (rx > w / 2) rx = w / 2;
	if (ry > h / 2) ry = h / 2;

	if (rx == 0) {
		rpath_move_to(p, (Vec2){ x, y });
		rpath_line_to(p, (Vec2){ x + w, y });
		rpath_line_to(p, (Vec2){ x + w, y + h });
		rpath_line_to(p, (Vec2){ x, y + h });
		rpath_close(p);
	} else {
		f64 k = 0.552284749831;   /* cubic circle approximation factor */
		rpath_move_to(p, (Vec2){ x + rx, y });
		rpath_line_to(p, (Vec2){ x + w - rx, y });
		rpath_cubic_to(p, (Vec2){ x + w - rx + k * rx, y },
		                  (Vec2){ x + w, y + ry - k * ry },
		                  (Vec2){ x + w, y + ry });
		rpath_line_to(p, (Vec2){ x + w, y + h - ry });
		rpath_cubic_to(p, (Vec2){ x + w, y + h - ry + k * ry },
		                  (Vec2){ x + w - rx + k * rx, y + h },
		                  (Vec2){ x + w - rx, y + h });
		rpath_line_to(p, (Vec2){ x + rx, y + h });
		rpath_cubic_to(p, (Vec2){ x + rx - k * rx, y + h },
		                  (Vec2){ x, y + h - ry + k * ry },
		                  (Vec2){ x, y + h - ry });
		rpath_line_to(p, (Vec2){ x, y + ry });
		rpath_cubic_to(p, (Vec2){ x, y + ry - k * ry },
		                  (Vec2){ x + rx - k * rx, y },
		                  (Vec2){ x + rx, y });
		rpath_close(p);
	}
}

static void shape_circle(RPath *p, const XNode *n)
{
	const char *as;
	f64 cx = 0, cy = 0, r = 0;
	if ((as = xget(n, "cx")) && !svg_len(as, 0, &cx)) return;
	if ((as = xget(n, "cy")) && !svg_len(as, 0, &cy)) return;
	if (!(as = xget(n, "r")) || !svg_len(as, 0, &r) || r <= 0) return;
	rpath_move_to(p, (Vec2){ cx + r, cy });
	rpath_arc_to(p, r, r, 0, false, true, (Vec2){ cx - r, cy });
	rpath_arc_to(p, r, r, 0, false, true, (Vec2){ cx + r, cy });
	rpath_close(p);
}

static void shape_ellipse(RPath *p, const XNode *n)
{
	const char *as;
	f64 cx = 0, cy = 0, rx = 0, ry = 0;
	if ((as = xget(n, "cx")) && !svg_len(as, 0, &cx)) return;
	if ((as = xget(n, "cy")) && !svg_len(as, 0, &cy)) return;
	if (!(as = xget(n, "rx")) || !svg_len(as, 0, &rx) || rx <= 0) return;
	if (!(as = xget(n, "ry")) || !svg_len(as, 0, &ry) || ry <= 0) return;
	rpath_move_to(p, (Vec2){ cx + rx, cy });
	rpath_arc_to(p, rx, ry, 0, false, true, (Vec2){ cx - rx, cy });
	rpath_arc_to(p, rx, ry, 0, false, true, (Vec2){ cx + rx, cy });
	rpath_close(p);
}

static void shape_poly(RPath *p, const XNode *n, bool close)
{
	const char *pts = xget(n, "points");
	if (!pts)
		return;
	const char *q = pts;
	bool first = true;
	while (*q) {
		f64 x, y;
		const char *pos = NULL;
		if (!rev_parse_f64(q, strlen(q), &pos, &x))
			break;
		q = pos;
		while (*q && (isspace((unsigned char)*q) || *q == ','))
			q++;
		if (!rev_parse_f64(q, strlen(q), &pos, &y))
			break;
		q = pos;
		if (first) {
			rpath_move_to(p, (Vec2){ x, y });
			first = false;
		} else {
			rpath_line_to(p, (Vec2){ x, y });
		}
	}
	if (close && !first)
		rpath_close(p);
}

/* builds the element's local-space path (arena-owned) */
static RPath *element_path(const XNode *n, RArena *ar)
{
	RPath *p = rpath_new(ar);
	const char *tag = n->tag;
	if (strcmp(tag, "path") == 0) {
		const char *d = xget(n, "d");
		if (d && !parse_path_data(p, d))
			LOGW("svg: failed to parse path data");
	} else if (strcmp(tag, "rect") == 0) {
		shape_rect(p, n);
	} else if (strcmp(tag, "circle") == 0) {
		shape_circle(p, n);
	} else if (strcmp(tag, "ellipse") == 0) {
		shape_ellipse(p, n);
	} else if (strcmp(tag, "line") == 0) {
		const char *as;
		f64 x1 = 0, y1 = 0, x2 = 0, y2 = 0;
		if ((as = xget(n, "x1")) && !svg_len(as, 0, &x1)) {}
		if ((as = xget(n, "y1")) && !svg_len(as, 0, &y1)) {}
		if ((as = xget(n, "x2")) && !svg_len(as, 0, &x2)) {}
		if ((as = xget(n, "y2")) && !svg_len(as, 0, &y2)) {}
		rpath_move_to(p, (Vec2){ x1, y1 });
		rpath_line_to(p, (Vec2){ x2, y2 });
	} else if (strcmp(tag, "polyline") == 0) {
		shape_poly(p, n, false);
	} else if (strcmp(tag, "polygon") == 0) {
		shape_poly(p, n, true);
	}
	return p;
}

static bool is_shape(const XNode *n)
{
	static const char *shapes[] = { "path", "rect", "circle", "ellipse",
	                                "line", "polyline", "polygon" };
	for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
		if (strcmp(n->tag, shapes[i]) == 0)
			return true;
	return false;
}

/* --------------------------------------------------------------------------
 * style / property resolution
 * ------------------------------------------------------------------------ */

typedef struct {
	const char *fill;         /* paint spec string  */
	const char *stroke;
	const char *color;        /* currentColor       */
	f64 fill_opacity, stroke_opacity, opacity;
	f64 stroke_width, miterlimit, dashoff;
	f64 dash[16];
	int  ndash;
	int  cap, join;
	bool evenodd;
} Props;

static const Props DEF_PROPS = {
	NULL, NULL, NULL,
	1.0, 1.0, 1.0,
	1.0, 4.0, 0.0,
	{ 0 }, 0,
	0, 0, false,
};

/* parse style="a:b;c:d" into up to `max` pairs */
typedef struct { const char *k, *v; } KV;

static int parse_style(const char *style, KV *out, int max)
{
	int n = 0;
	const char *p = style;
	while (*p && n < max) {
		while (*p && (isspace((unsigned char)*p) || *p == ';'))
			p++;
		if (!*p)
			break;
		const char *ks = p;
		while (*p && *p != ':' && *p != ';')
			p++;
		if (*p != ':') {
			while (*p && *p != ';')
				p++;
			continue;
		}
		size_t klen = (size_t)(p - ks);
		p++;
		while (*p && isspace((unsigned char)*p))
			p++;
		const char *vs = p;
		while (*p && *p != ';')
			p++;
		size_t vlen = (size_t)(p - vs);
		while (vlen > 0 && isspace((unsigned char)vs[vlen - 1]))
			vlen--;
		/* trim key */
		while (klen > 0 && isspace((unsigned char)ks[klen - 1]))
			klen--;
		if (klen && vlen) {
			out[n].k = ks;
			out[n].v = vs;
			n++;
		}
	}
	return n;
}

static const char *prop_val(const XNode *n, const KV *style, int nstyle, const char *name)
{
	for (int i = 0; i < nstyle; i++)
		if (strlen(style[i].k) == strlen(name) && strncmp(style[i].k, name, strlen(name)) == 0)
			return style[i].v;
	return xget(n, name);
}

static bool truthy_none(const char *s)
{
	return s && strcasecmp(s, "none") == 0;
}

static void resolve_props(const XNode *n, const Props *inh, Props *out)
{
	*out = *inh;
	KV style[16];
	int nstyle = 0;
	const char *sv = xget(n, "style");
	if (sv)
		nstyle = parse_style(sv, style, 16);

	const char *v;

	if ((v = prop_val(n, style, nstyle, "fill"))) {
		out->fill = truthy_none(v) ? NULL : v;
	}
	if ((v = prop_val(n, style, nstyle, "stroke"))) {
		out->stroke = truthy_none(v) ? NULL : v;
	}
	if ((v = prop_val(n, style, nstyle, "color")))
		out->color = v;
	if ((v = prop_val(n, style, nstyle, "fill-opacity")))
		svg_num(v, &out->fill_opacity);
	if ((v = prop_val(n, style, nstyle, "stroke-opacity")))
		svg_num(v, &out->stroke_opacity);
	if ((v = prop_val(n, style, nstyle, "opacity")))
		svg_num(v, &out->opacity);
	else
		out->opacity = 1.0;   /* opacity is NOT inherited in SVG */
	if ((v = prop_val(n, style, nstyle, "stroke-width")))
		svg_len(v, 0, &out->stroke_width);
	if ((v = prop_val(n, style, nstyle, "stroke-miterlimit")))
		svg_num(v, &out->miterlimit);
	if ((v = prop_val(n, style, nstyle, "stroke-linecap"))) {
		if (strcasecmp(v, "round") == 0) out->cap = RCAP_ROUND;
		else if (strcasecmp(v, "square") == 0) out->cap = RCAP_SQUARE;
		else out->cap = RCAP_BUTT;
	}
	if ((v = prop_val(n, style, nstyle, "stroke-linejoin"))) {
		if (strcasecmp(v, "round") == 0) out->join = RJOIN_ROUND;
		else if (strcasecmp(v, "bevel") == 0) out->join = RJOIN_BEVEL;
		else out->join = RJOIN_MITER;
	}
	if ((v = prop_val(n, style, nstyle, "fill-rule")))
		out->evenodd = (strcasecmp(v, "evenodd") == 0);
	if ((v = prop_val(n, style, nstyle, "stroke-dashoffset")))
		svg_len(v, 0, &out->dashoff);
	if ((v = prop_val(n, style, nstyle, "stroke-dasharray"))) {
		if (!truthy_none(v)) {
			out->ndash = 0;
			const char *q = v;
			while (*q && out->ndash < 16) {
				f64 d;
				const char *pos = NULL;
				if (!rev_parse_f64(q, strlen(q), &pos, &d))
					break;
				q = pos;
				out->dash[out->ndash++] = d;
			}
			if (out->ndash == 0)
				out->ndash = 0;
		} else {
			out->ndash = 0;
		}
	}
}

/* --------------------------------------------------------------------------
 * gradients
 * ------------------------------------------------------------------------ */

typedef struct {
	const XNode *node;     /* linearGradient or radialGradient */
} GradEntry;

typedef struct {
	GradEntry e[64];
	int n;
} GradMap;

static void collect_gradients(const XNode *n, GradMap *gm)
{
	if (!n || gm->n >= 64)
		return;
	if (strcmp(n->tag, "linearGradient") == 0 || strcmp(n->tag, "radialGradient") == 0) {
		const char *id = xget(n, "id");
		if (id && *id)
			gm->e[gm->n++].node = n;
	}
	for (size_t i = 0; i < n->nkids; i++)
		collect_gradients(n->kids[i], gm);
}

static const XNode *lookup_grad(const GradMap *gm, const char *id, size_t idlen)
{
	if (!id)
		return NULL;
	for (int i = 0; i < gm->n; i++) {
		const char *gid = xget(gm->e[i].node, "id");
		if (gid && strlen(gid) == idlen && strncmp(gid, id, idlen) == 0)
			return gm->e[i].node;
	}
	return NULL;
}

/* parse "url(#id)" -> pointer to id start + its length */
static const char *parse_url_ref(const char *s, size_t *idlen)
{
	if (!s)
		return NULL;
	while (*s && isspace((unsigned char)*s))
		s++;
	if (strncasecmp(s, "url(#", 5) != 0)
		return NULL;
	const char *id = s + 5;
	const char *e = strchr(id, ')');
	if (!e || e == id)
		return NULL;
	*idlen = (size_t)(e - id);
	return id;
}

/* gradient coordinate: numbers may be % (fraction for oBB) or plain units */
static f64 grad_coord(const XNode *g, const char *name, f64 def)
{
	const char *v = xget(g, name);
	if (!v || !*v)
		return def;
	const char *pos = NULL;
	f64 out;
	if (!rev_parse_f64(v, strlen(v), &pos, &out))
		return def;
	while (pos && *pos && isspace((unsigned char)*pos))
		pos++;
	if (pos && *pos == '%')
		return out / 100.0;
	return out;
}

/* collect stops from a gradient's children */
static int grad_stops(const XNode *g, RGradStop *stops, int max)
{
	int n = 0;
	for (size_t i = 0; i < g->nkids && n < max; i++) {
		const XNode *k = g->kids[i];
		if (strcmp(k->tag, "stop") != 0)
			continue;
		RGradStop st;
		st.off = grad_coord(k, "offset", 0.0);
		st.off = rev_clampf(st.off, 0.0, 1.0);
		st.c = rev_rgb(0, 0, 0);
		st.c.a = 255;

		/* style attr may override stop-color / stop-opacity */
		const char *sc = xget(k, "stop-color");
		const char *so = xget(k, "stop-opacity");
		KV style[8];
		int nstyle = 0;
		const char *sv = xget(k, "style");
		if (sv)
			nstyle = parse_style(sv, style, 8);
		for (int q = 0; q < nstyle; q++) {
			if (strcmp(style[q].k, "stop-color") == 0) sc = style[q].v;
			if (strcmp(style[q].k, "stop-opacity") == 0) so = style[q].v;
		}
		if (sc) {
			RColor c;
			if (strcasecmp(sc, "currentColor") == 0 && sc)
				c = rev_rgb(0, 0, 0);
			else if (svg_color(sc, &c))
				st.c = c;
		}
		if (so) {
			f64 o;
			if (svg_num(so, &o))
				st.c.a = (u8)rev_clampf(o * 255.0, 0, 255);
		}
		stops[n++] = st;
	}
	return n;
}

/* --------------------------------------------------------------------------
 * rendering
 * ------------------------------------------------------------------------ */

typedef struct {
	GradMap gm;
	RArena *scratch;
} RenderCtx;

/* Build an RPaint for a paint spec ("#abc", "red", "url(#g)"). */
static bool build_paint(RPaint *out, const char *spec, const Props *props,
                        RMat m, Vec2 blo, Vec2 bhi, RenderCtx *ctx)
{
	memset(out, 0, sizeof *out);
	if (!spec || !*spec)
		return false;

	size_t idlen = 0;
	const char *id = parse_url_ref(spec, &idlen);
	if (id) {
		const XNode *g = lookup_grad(&ctx->gm, id, idlen);
		if (!g) {
			LOGW("svg: unknown paint server '%.*s'", (int)idlen, id);
			return false;
		}

		RMat gt;
		if (!parse_transform(&gt, xget(g, "gradientTransform")))
			gt = rmat_identity();

		bool obb = true;
		const char *gu = xget(g, "gradientUnits");
		if (gu && strcasecmp(gu, "userSpaceOnUse") == 0)
			obb = false;

		const char *sp = xget(g, "spreadMethod");
		out->spread = (sp && strcasecmp(sp, "reflect") == 0) ? RSPREAD_REFLECT
		              : (sp && strcasecmp(sp, "repeat") == 0) ? RSPREAD_REPEAT
		              : RSPREAD_PAD;

		out->stops = (RGradStop *)arena_alloc(ctx->scratch, 64 * sizeof(RGradStop));
		out->nstops = grad_stops(g, out->stops, 64);
		if (out->nstops == 0)
			return false;

		/* gradient vector in local user space, then to device space */
		RMat vm = rmat_mul(m, gt);

		bool is_linear = (strcmp(g->tag, "linearGradient") == 0);
		out->kind = is_linear ? RPAINT_LINEAR : RPAINT_RADIAL;
		if (is_linear) {
			Vec2 l0 = { grad_coord(g, "x1", 0.0), grad_coord(g, "y1", 0.0) };
			Vec2 l1 = { grad_coord(g, "x2", 1.0), grad_coord(g, "y2", 0.0) };
			if (obb) {
				l0.x = blo.x + l0.x * (bhi.x - blo.x);
				l0.y = blo.y + l0.y * (bhi.y - blo.y);
				l1.x = blo.x + l1.x * (bhi.x - blo.x);
				l1.y = blo.y + l1.y * (bhi.y - blo.y);
			}
			out->lp0 = rmat_apply(vm, l0);
			out->lp1 = rmat_apply(vm, l1);
		} else {
			Vec2 c = { grad_coord(g, "cx", 0.5), grad_coord(g, "cy", 0.5) };
			f64 r = grad_coord(g, "r", 0.5);
			Vec2 f = { grad_coord(g, "fx", HUGE_VAL), grad_coord(g, "fy", HUGE_VAL) };
			if (obb) {
				c.x = blo.x + c.x * (bhi.x - blo.x);
				c.y = blo.y + c.y * (bhi.y - blo.y);
				r = r * ((bhi.x - blo.x + bhi.y - blo.y) * 0.5);
				if (f.x != HUGE_VAL && f.y != HUGE_VAL) {
					f.x = blo.x + f.x * (bhi.x - blo.x);
					f.y = blo.y + f.y * (bhi.y - blo.y);
				}
			}
			if (f.x == HUGE_VAL || f.y == HUGE_VAL)
				f = c;
			out->rc = rmat_apply(vm, c);
			out->rf = rmat_apply(vm, f);
			/* radius in device space: scale by average axis scale */
			f64 det = m.a * m.d - m.b * m.c;
			f64 s = sqrt(fabs(det));
			out->rr = r * s;
		}
		return true;
	}

	/* plain color */
	if (strcasecmp(spec, "currentColor") == 0)
		spec = props->color ? props->color : "black";
	RColor c;
	if (!svg_color(spec, &c))
		return false;
	out->kind = RPAINT_SOLID;
	out->solid = c;
	return true;
}

/* copy a path into the scratch arena (segment array only) */
static RPath *clone_path(const RPath *src, RArena *ar)
{
	RPath *p = rpath_new(ar);
	if (src->n) {
		p->v = (RSeg *)arena_alloc(ar, src->n * sizeof(RSeg));
		memcpy(p->v, src->v, src->n * sizeof(RSeg));
		p->n = p->cap = src->n;
	}
	return p;
}

static void render_node(const XNode *n, RMat m, const Props *inh,
                        u32 *px, u32 w, u32 h, RenderCtx *ctx);

static void render_element(const XNode *n, RMat m, const Props *props,
                           u32 *px, u32 w, u32 h, RenderCtx *ctx)
{
	RArena *ar = ctx->scratch;
	RPath *dp = clone_path((RPath *)n->lpath, ar);
	rpath_transform(dp, m);
	RPoly *poly = rpath_flatten(dp, ar, FLATTEN_TOL);
	if (poly->n == 0)
		return;

	/* local bbox for objectBoundingBox gradients */
	Vec2 blo = { 0, 0 }, bhi = { 1, 1 };
	bool hasbb;
	rpath_bbox((RPath *)n->lpath, &hasbb, &blo, &bhi);
	if (!hasbb) { blo = (Vec2){ 0, 0 }; bhi = (Vec2){ 1, 1 }; }

	f64 det = m.a * m.d - m.b * m.c;
	f64 sscale = sqrt(fabs(det));

	/* fill */
	if (props->fill && props->fill_opacity > 0 && props->opacity > 0) {
		RPaint paint;
		if (build_paint(&paint, props->fill, props, m, blo, bhi, ctx)) {
			f64 a = rev_clampf(props->fill_opacity * props->opacity, 0, 1);
			if (paint.kind == RPAINT_SOLID) {
				f64 sa = paint.solid.a / 255.0 * a;
				paint.solid.a = (u8)rev_clampf(sa * 255.0 + 0.5, 0, 255);
			} else {
				for (int i = 0; i < paint.nstops; i++) {
					f64 sa = paint.stops[i].c.a / 255.0 * a;
					paint.stops[i].c.a = (u8)rev_clampf(sa * 255.0 + 0.5, 0, 255);
				}
			}
			if (paint.kind != RPAINT_SOLID || paint.solid.a > 0)
				rev_raster_fill(px, w, h, poly, &paint, props->evenodd, ar);
		}
	}

	/* stroke */
	if (props->stroke && props->stroke_width > 0 &&
	    props->stroke_opacity > 0 && props->opacity > 0) {
		RPaint paint;
		if (build_paint(&paint, props->stroke, props, m, blo, bhi, ctx)) {
			f64 a = rev_clampf(props->stroke_opacity * props->opacity, 0, 1);
			if (paint.kind == RPAINT_SOLID) {
				f64 sa = paint.solid.a / 255.0 * a;
				paint.solid.a = (u8)rev_clampf(sa * 255.0 + 0.5, 0, 255);
			} else {
				for (int i = 0; i < paint.nstops; i++) {
					f64 sa = paint.stops[i].c.a / 255.0 * a;
					paint.stops[i].c.a = (u8)rev_clampf(sa * 255.0 + 0.5, 0, 255);
				}
			}
			if (paint.kind != RPAINT_SOLID || paint.solid.a > 0) {
				RStroke st;
				memset(&st, 0, sizeof st);
				st.paint = paint;
				st.width = props->stroke_width * sscale;
				st.cap = props->cap;
				st.join = props->join;
				st.miterlimit = props->miterlimit > 0 ? props->miterlimit : 4.0;
				if (props->ndash > 0) {
					st.dashed = true;
					st.ndash = props->ndash;
					for (int i = 0; i < props->ndash; i++)
						st.dash[i] = props->dash[i] * sscale;
					st.dashoff = props->dashoff * sscale;
				}
				rev_raster_stroke(px, w, h, poly, &st, ar);
			}
		}
	}
}

static void render_node(const XNode *n, RMat m, const Props *inh,
                        u32 *px, u32 w, u32 h, RenderCtx *ctx)
{
	if (!n)
		return;

	const char *disp = xget(n, "display");
	if (disp && strcasecmp(disp, "none") == 0)
		return;

	Props props;
	resolve_props(n, inh, &props);

	RMat nm = m;
	const char *tr = xget(n, "transform");
	if (tr) {
		RMat t;
		if (parse_transform(&t, tr))
			nm = rmat_mul(m, t);
	}

	const char *tag = n->tag;
	bool in_defs = false;
	for (const XNode *a = n->parent; a; a = a->parent)
		if (strcmp(a->tag, "defs") == 0)
			in_defs = true;

	if (is_shape(n) && n->lpath && !in_defs) {
		render_element(n, nm, &props, px, w, h, ctx);
		return;
	}

	if (strcmp(tag, "g") == 0 || strcmp(tag, "svg") == 0 || strcmp(tag, "a") == 0) {
		if (props.opacity < 1.0 - 1e-6) {
			/* group opacity: render children to a temp buffer */
			u32 *tmp = (u32 *)calloc((size_t)w * h, 4);
			if (tmp) {
				for (size_t i = 0; i < n->nkids; i++)
					render_node(n->kids[i], nm, &props, tmp, w, h, ctx);
				f64 ga = rev_clampf(props.opacity, 0, 1);
				u32 amul = (u32)(ga * 256.0 + 0.5);
				size_t np = (size_t)w * h;
				for (size_t i = 0; i < np; i++) {
					u32 t = tmp[i];
					u32 ta = t >> 24;
					if (!ta)
						continue;
					u32 a = (ta * amul) >> 8;
					if (a > 255) a = 255;
					u32 r = t & 0xff, g = (t >> 8) & 0xff, b = (t >> 16) & 0xff;
					/* source-over onto px */
					u32 d = px[i];
					u32 da = d >> 24;
					if (da == 0) {
						px[i] = (a << 24) | (b << 16) | (g << 8) | r;
					} else {
						u32 oa = a + ((da * (255 - a) + 127) >> 8);
						if (oa > 255) oa = 255;
						u64 den = (u64)oa * 255;
						u64 nr = (u64)r * a * 255 + (u64)(d & 0xff) * da * (255 - a);
						u64 ng = (u64)g * a * 255 + (u64)((d >> 8) & 0xff) * da * (255 - a);
						u64 nb = (u64)b * a * 255 + (u64)((d >> 16) & 0xff) * da * (255 - a);
						u64 vr = nr / den, vg = ng / den, vb = nb / den;
						u8 rr = (u8)(vr > 255 ? 255 : vr);
						u8 gg = (u8)(vg > 255 ? 255 : vg);
						u8 bb = (u8)(vb > 255 ? 255 : vb);
						px[i] = (oa << 24) | (bb << 16) | (gg << 8) | rr;
					}
				}
				free(tmp);
				return;
			}
		}
		for (size_t i = 0; i < n->nkids; i++)
			render_node(n->kids[i], nm, &props, px, w, h, ctx);
		return;
	}
}

/* --------------------------------------------------------------------------
 * public API
 * ------------------------------------------------------------------------ */

RevSvg *rev_svg_parse(const char *text, size_t len, char *err, size_t errcap)
{
	if (err && errcap)
		err[0] = '\0';

	RevSvg *svg = (RevSvg *)calloc(1, sizeof(RevSvg));
	if (!svg)
		return NULL;
	svg->dom = arena_create(64 * 1024);
	svg->scratch = arena_create(64 * 1024);

	Xml x;
	memset(&x, 0, sizeof x);
	x.p = text;
	x.end = text + len;
	x.ar = svg->dom;

	xml_skip_misc(&x);
	XNode *root = xml_element(&x, NULL);
	if (!root || x.failed || strcmp(root->tag, "svg") != 0) {
		snprintf(err && errcap ? err : NULL, err && errcap ? errcap : 0,
		         "svg: %s", x.failed ? x.err : "not an <svg> document");
		rev_svg_destroy(svg);
		return NULL;
	}
	svg->root = root;

	/* prebuild local-space paths for all shapes */
	struct { const XNode *n; } stack[512];
	int sp = 0;
	stack[sp++].n = root;
	while (sp > 0) {
		const XNode *n = stack[--sp].n;
		if (is_shape(n))
			((XNode *)n)->lpath = element_path(n, svg->dom);
		for (size_t i = 0; i < n->nkids && sp < 512; i++)
			stack[sp++].n = n->kids[i];
	}

	/* intrinsic size */
	f64 vw = 0, vh = 0;
	const char *as;
	if ((as = xget(root, "width")) && !svg_len(as, 0, &vw)) vw = 0;
	if ((as = xget(root, "height")) && !svg_len(as, 0, &vh)) vh = 0;
	const char *vb = xget(root, "viewBox");
	if (vb) {
		f64 v[4];
		const char *pos = NULL;
		int got = 0;
		const char *q = vb;
		for (int i = 0; i < 4; i++) {
			if (!rev_parse_f64(q, strlen(q), &pos, &v[i]))
				break;
			q = pos;
			got++;
		}
		if (got == 4 && v[2] > 0 && v[3] > 0) {
			if (vw <= 0) vw = v[2];
			if (vh <= 0) vh = v[3];
		}
	}
	svg->vw = vw > 0 ? vw : 0;
	svg->vh = vh > 0 ? vh : 0;
	return svg;
}

RevSvg *rev_svg_load(const char *path, char *err, size_t errcap)
{
	size_t len = 0;
	u8 *data = rev_read_file(path, &len);
	if (!data) {
		if (err && errcap)
			snprintf(err, errcap, "svg: cannot read '%s'", path);
		return NULL;
	}
	RevSvg *svg = rev_svg_parse((const char *)data, len, err, errcap);
	free(data);
	return svg;
}

void rev_svg_destroy(RevSvg *svg)
{
	if (!svg)
		return;
	arena_destroy(svg->dom);
	arena_destroy(svg->scratch);
	free(svg);
}

void rev_svg_intrinsic_size(const RevSvg *svg, f64 *w, f64 *h)
{
	*w = svg ? svg->vw : 0;
	*h = svg ? svg->vh : 0;
}

/* root viewport matrix: viewBox -> device rect, honoring preserveAspectRatio */
static RMat viewport_matrix(const RevSvg *svg, u32 w, u32 h)
{
	const XNode *root = svg->root;
	RMat m = rmat_identity();

	const char *vbs = xget(root, "viewBox");
	if (vbs) {
		f64 v[4];
		const char *pos = NULL;
		const char *q = vbs;
		int got = 0;
		for (int i = 0; i < 4; i++) {
			if (!rev_parse_f64(q, strlen(q), &pos, &v[i]))
				break;
			q = pos;
			got++;
		}
		if (got == 4 && v[2] > 0 && v[3] > 0) {
			f64 bw = v[2], bh = v[3];

			/* preserveAspectRatio */
			const char *par = xget(root, "preserveAspectRatio");
			char align = 'M';
			bool slice = false, nonepar = false;
			int ax = 1, ay = 1;   /* 0=min 1=mid 2=max */
			if (par && *par) {
				if (strncasecmp(par, "none", 4) == 0) {
					nonepar = true;
				} else {
					if (strncasecmp(par, "xMinY", 5) == 0) { ax = 0; par += 5; }
					else if (strncasecmp(par, "xMidY", 5) == 0) { ax = 1; par += 5; }
					else if (strncasecmp(par, "xMaxY", 5) == 0) { ax = 2; par += 5; }
					if (par && *par) {
						if (*par == 'M' || *par == 'm') ay = 1;
						else if (*par == 'i' || *par == 'I') ay = 0;
						else if (*par == 'a' || *par == 'A') ay = 2;
					}
					const char *sp2 = par ? strchr(par, ' ') : NULL;
					if (sp2 && strncasecmp(sp2 + 1, "slice", 5) == 0)
						slice = true;
				}
			}
			(void)align;

			f64 sx = (f64)w / bw, sy = (f64)h / bh;
			if (nonepar) {
				m = rmat_mul(rmat_scale(sx, sy), rmat_translate(-v[0], -v[1]));
				return m;
			}
			f64 s = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
			f64 tx, ty;
			f64 pxw = bw * s, pxh = bh * s;
			tx = (ax == 0) ? 0 : (ax == 1) ? ((f64)w - pxw) / 2 : (f64)w - pxw;
			ty = (ay == 0) ? 0 : (ay == 1) ? ((f64)h - pxh) / 2 : (f64)h - pxh;
			m = rmat_mul(rmat_translate(tx, ty),
			    rmat_mul(rmat_scale(s, s), rmat_translate(-v[0], -v[1])));
			return m;
		}
	}

	/* no viewBox: scale declared size to target */
	f64 vw = svg->vw > 0 ? svg->vw : (f64)w;
	f64 vh = svg->vh > 0 ? svg->vh : (f64)h;
	if (vw > 0 && vh > 0 && (vw != (f64)w || vh != (f64)h))
		m = rmat_scale((f64)w / vw, (f64)h / vh);
	return m;
}

void rev_svg_render(const RevSvg *svg, u32 *px, u32 w, u32 h)
{
	if (!svg || !svg->root || !px || w == 0 || h == 0)
		return;

	RenderCtx ctx;
	memset(&ctx, 0, sizeof ctx);
	collect_gradients(svg->root, &ctx.gm);
	ctx.scratch = svg->scratch;

	/* reset the scratch arena by replacing it (old one freed) */
	arena_destroy(svg->scratch);
	((RevSvg *)svg)->scratch = arena_create(256 * 1024);
	ctx.scratch = svg->scratch;

	RMat m = viewport_matrix(svg, w, h);

	Props def = DEF_PROPS;
	def.fill = "black";
	render_node(svg->root, m, &def, px, w, h, &ctx);
}
