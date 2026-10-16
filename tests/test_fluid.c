/* ============================================================================
 * Reverie Engine — tests/test_fluid.c
 * Pins the ORGANIC fluid scheduler (src/fluid_sched.h — the shared math
 * behind all three solver tiers) and the solver's basic invariants:
 *
 *   cadence   — the wisp interval is irregular (varied, rarely repeated),
 *               bounded, and never the fixed 0.9 s comb it replaced
 *   trajectory — positions stay in-viewport, cover the viewport over time
 *               (not clustered), impulses are bounded and vary in strength
 *   breathing — the ambient gain is bounded, oscillates over minutes, and
 *               does not repeat within a 10-minute window
 *   solver    — dye decays without input, appears under the pointer, and
 *               the field stays finite after long ambient-only running
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fluid.h"
#include "fluid_sched.h"

static int failures;

#define CHECK(cond, ...)                                                    \
	do {                                                                    \
		if (cond) {                                                         \
			printf("  ok\n");                                              \
		} else {                                                            \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
			printf(__VA_ARGS__);                                            \
			printf("\n");                                                   \
			failures++;                                                     \
		}                                                                   \
	} while (0)

/* rendered energy: sum of channel bytes over the tonemapped output */
static u64 render_energy(RevFluid *f, u32 *px, u32 dw, u32 dh)
{
	fluid_render(f, px, dw, dh);
	u64 s = 0;
	for (u32 k = 0; k < dw * dh; k += 37)
		s += (px[k] & 0xff) + ((px[k] >> 8) & 0xff)
		    + ((px[k] >> 16) & 0xff) + (px[k] >> 24);
	return s;
}

int main(void)
{
	printf("test_fluid:\n");

	/* ---- cadence ------------------------------------------------------ */
	{
		enum { N = 2000 };
		double iv[N], sum = 0, mn = 1e9, mx = -1e9;
		int repeats = 0, exact_ticking = 1;
		for (int i = 0; i < N; i++) {
			iv[i] = fluid_wisp_interval((u64)i);
			sum += iv[i];
			if (iv[i] < mn) mn = iv[i];
			if (iv[i] > mx) mx = iv[i];
			if (i && fabs(iv[i] - iv[i - 1]) < 1e-12)
				repeats++;
			if (fabs(iv[i] - 0.9) < 1e-12)
				exact_ticking = 0;   /* never the old fixed cadence */
		}
		double mean = sum / N;
		double var = 0;
		for (int i = 0; i < N; i++)
			var += (iv[i] - mean) * (iv[i] - mean);
		var /= N;
		CHECK(mn >= 0.45 && mx <= 1.75, "intervals within [0.45, 1.75] (%.3f..%.3f)",
		      mn, mx);
		CHECK(sqrt(var) > 0.25, "cadence is irregular (std %.3f)", sqrt(var));
		CHECK(repeats < N / 100, "consecutive repeats are rare (%d/%d)", repeats, N);
		CHECK(exact_ticking, "the fixed 0.9s comb never appears");
	}

	/* ---- trajectory --------------------------------------------------- */
	{
		enum { N = 4000 };
		double sx = 0, sy = 0, sxx = 0, syy = 0, vmax = 0, vsum = 0;
		bool bounds_ok = true;
		for (u64 i = 1; i <= N; i++) {
			double x, y, vx, vy;
			bool alt;
			/* sample along the ambient clock as the solver would */
			fluid_wisp_at((f64)i * 1.05, i, &x, &y, &vx, &vy, &alt);
			if (x < 0.05 || x > 0.95 || y < 0.05 || y > 0.95)
				bounds_ok = false;
			double vm = hypot(vx, vy);
			if (vm > vmax) vmax = vm;
			vsum += vm;
			sx += x; sy += y; sxx += x * x; syy += y * y;
		}
		double mx = sx / N, my = sy / N;
		double stdx = sqrt(sxx / N - mx * mx), stdy = sqrt(syy / N - my * my);
		CHECK(bounds_ok, "wisp positions stay inside [0.05, 0.95]");
		CHECK(mx > 0.35 && mx < 0.65 && my > 0.35 && my < 0.65,
		      "positions center on the viewport (mean %.3f, %.3f)", mx, my);
		CHECK(stdx > 0.12 && stdy > 0.12,
		      "positions spread (std %.3f, %.3f — not clustered)", stdx, stdy);
		CHECK(vmax < 1.6 && vsum / N > 0.08,
		      "impulses bounded (max %.3f) and alive (mean %.3f)", vmax, vsum / N);
	}

	/* ---- ambient breathing --------------------------------------------- */
	{
		double mn = 1e9, mx = -1e9;
		for (double t = 0; t < 600.0; t += 0.25) {
			double g = fluid_ambient_gain(t);
			if (g < mn) mn = g;
			if (g > mx) mx = g;
		}
		CHECK(mn >= 0.25 && mx <= 1.0, "gain clamped to [0.25, 1.0] (%.3f..%.3f)",
		      mn, mx);
		CHECK(mx - mn > 0.4, "gain actually breathes (span %.3f)", mx - mn);
		/* non-repetition: 0..300 vs 300..600 must differ substantially */
		double d = 0;
		int n = 0;
		for (double t = 0; t < 300.0; t += 0.5) {
			d += fabs(fluid_ambient_gain(t) - fluid_ambient_gain(t + 300.0));
			n++;
		}
		CHECK(d / n > 0.01, "no 300s periodicity (mean |delta| %.4f)", d / n);
	}

	/* ---- solver invariants (public API only: the struct is opaque) ------ */
	{
		/* small grid, still the real solver paths */
		RevFluid *f = fluid_new(320, 180, 0.25);
		CHECK(f != NULL, "fluid_new");
		if (f) {
			u32 dw = 0, dh = 0;
			fluid_dye_size(f, &dw, &dh);
			u32 *px = (u32 *)malloc((size_t)dw * dh * 4);
			CHECK(px != NULL, "probe buffer");
			fluid_configure(f, rev_rgb(80, 180, 255), rev_rgb(255, 90, 190),
			                8.0, 0.02, 0.08, 0.45);
			/* 1) ambient-only for a simulated 20s: dye appears and is alive */
			for (int i = 0; i < 1200; i++)
				fluid_step(f, 1.0 / 60.0);
			u64 e_amb = render_energy(f, px, dw, dh);
			CHECK(e_amb > 100, "wisps keep injecting dye (energy %llu)",
			      (unsigned long long)e_amb);

			/* 2) dissipation: without ambient or pointer the dye drains */
			fluid_configure(f, rev_rgb(80, 180, 255), rev_rgb(255, 90, 190),
			                8.0, 0.02, 0.9, 0.0);
			u64 e0 = render_energy(f, px, dw, dh);
			for (int i = 0; i < 600; i++)
				fluid_step(f, 1.0 / 60.0);
			u64 e1 = render_energy(f, px, dw, dh);
			CHECK(e1 < e0 / 2 + 1, "dye decays without input (%llu -> %llu)",
			      (unsigned long long)e0, (unsigned long long)e1);

			/* 3) pointer interaction injects dye */
			fluid_configure(f, rev_rgb(80, 180, 255), rev_rgb(255, 90, 190),
			                30.0, 0.05, 0.02, 0.0);
			fluid_pointer(f, 0.5, 0.5, 3.0, 2.0, true);
			fluid_step(f, 1.0 / 60.0);
			fluid_pointer(f, 0.5, 0.5, 0.0, 0.0, false);
			fluid_step(f, 1.0 / 60.0);
			u64 e_ptr = render_energy(f, px, dw, dh);
			CHECK(e_ptr > 100, "pointer splat injects dye (energy %llu)",
			      (unsigned long long)e_ptr);

			free(px);
			fluid_free(f);
		}
	}

	printf(failures ? "fluid: %d FAILURE(S)\n" : "fluid: OK\n", failures);
	return failures ? 1 : 0;
}
