/*
 * This file is part of the Advance project.
 *
 * Copyright (C) 1999, 2000, 2001, 2002, 2003 Andrea Mazzoleni
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * In addition, as a special exception, Andrea Mazzoleni
 * gives permission to link the code of this program with
 * the MAME library (or with modified versions of MAME that use the
 * same license as MAME), and distribute linked combinations including
 * the two.  You must obey the GNU General Public License in all
 * respects for all of the code used other than MAME.  If you modify
 * this file, you may extend this exception to your version of the
 * file, but you are not obligated to do so.  If you do not wish to
 * do so, delete this exception statement from your version.
 */

#include "blit.h"
#include "log.h"
#include "error.h"
#include "endianrw.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Define if you want to assume fast write in video memory */
#ifndef __MSDOS__
#define USE_MTRR
#endif

/***************************************************************************/
/* mmx */

#if defined(USE_ASM_i586)

static void blit_cpuid(unsigned level, unsigned* regs)
{
	__asm__ __volatile__(
		"pushal\n"
		".byte 0x0F, 0xA2\n"
		"movl %%eax, (%1)\n"
		"movl %%ebx, 4(%1)\n"
		"movl %%ecx, 8(%1)\n"
		"movl %%edx, 12(%1)\n"
		"popal\n"
		:
		: "a" (level), "D" (regs)
		: "cc"
	);
}

static adv_bool blit_has_mmx(void)
{
	unsigned regs[4];
	unsigned a, b;

	__asm__ __volatile__(
		"pushfl\n"
		"pushfl\n"
		"popl %0\n"
		"movl %0, %1\n"
		"xorl $0x200000, %0\n"
		"pushl %0\n"
		"popfl\n"
		"pushfl\n"
		"popl %0\n"
		"popfl"
		: "=r" (a), "=r" (b)
		:
		: "cc"
	);

	if (a == b) {
		log_std(("blit: no cpuid\n"));
		return 0; /* no cpuid */
	}

	blit_cpuid(0, regs);
	if (regs[0] > 0) {
		blit_cpuid(1, regs);
		if ((regs[3] & 0x800000) != 0) {
			log_std(("blit: mmx\n"));
			return 1;
		}
	}

	log_std(("blit: no mmx\n"));
	return 0;
}

/* Support the the both condition. MMX present or not */
adv_bool the_blit_mmx = 0;
#define BLITTER(name) (the_blit_mmx ? name##_mmx : name##_def)

static adv_error blit_set_mmx(void)
{
	the_blit_mmx = blit_has_mmx();

	return 0;
}

static inline void internal_end(void)
{
	if (the_blit_mmx) {
		__asm__ __volatile__ (
			"emms"
		);
	}
}

#else

/* Assume that MMX is NOT present. */

#define the_blit_mmx 0
#define BLITTER(name) (name##_def)

static adv_error blit_set_mmx(void)
{
	return 0;
}

static inline void internal_end(void)
{
}

#endif

/***************************************************************************/
/* internal */

#include "icopy.h"
#include "idouble.h"
#include "imax.h"
#include "imean.h"
#include "irgb.h"

#ifndef USE_BLIT_TINY
#include "scale2x.h"
#include "scale3x.h"
#include "lq2x.h"
#include "lq3x.h"
#include "lq4x.h"
#ifndef USE_BLIT_SMALL
#include "hq2x.h"
#include "hq3x.h"
#include "hq4x.h"
#endif
#endif

/***************************************************************************/
/* video stage */

#define STAGE_SIZE(stage, _type, _sdx, _sdp, _sbpp, _ddx, _dbpp) \
	do { \
		stage->type = (_type); \
		stage->sdx = (_sdx); \
		stage->sdp = (_sdp); \
		stage->sbpp = (_sbpp); \
		slice_set(&stage->slice, (_sdx), (_ddx)); \
		stage->palette = 0; \
		stage->buffer_size = (_dbpp)*(_ddx); \
		stage->buffer_extra_size = 0; \
		stage->put_plain = 0; \
		stage->put = 0; \
	} while (0)

#define STAGE_TYPE(stage, _type) \
	do { \
		stage->type = (_type); \
	} while (0)

#define STAGE_PUT(stage, _put_plain, _put) \
	do { \
		stage->put_plain = (_put_plain); \
		stage->put = (stage->sbpp == stage->sdp) ? stage->put_plain : (_put); \
	} while (0)

#define STAGE_EXTRA(stage) \
	do { \
		stage->buffer_extra_size = stage->buffer_size; \
	} while (0)

#define STAGE_PALETTE(stage, _palette) \
	do { \
		stage->palette = _palette; \
	} while (0)

#define STAGE_CONVERSION(stage, _sdef, _ddef) \
	do { \
		union adv_color_def_union tmp_sdef; \
		union adv_color_def_union tmp_ddef; \
		tmp_sdef.ordinal = (_sdef); \
		tmp_ddef.ordinal = (_ddef); \
		stage->red_shift = rgb_conv_shift_get(tmp_sdef.nibble.red_len, tmp_sdef.nibble.red_pos, tmp_ddef.nibble.red_len, tmp_ddef.nibble.red_pos); \
		stage->red_mask = rgb_conv_mask_get(tmp_sdef.nibble.red_len, tmp_sdef.nibble.red_pos, tmp_ddef.nibble.red_len, tmp_ddef.nibble.red_pos); \
		stage->green_shift = rgb_conv_shift_get(tmp_sdef.nibble.green_len, tmp_sdef.nibble.green_pos, tmp_ddef.nibble.green_len, tmp_ddef.nibble.green_pos); \
		stage->green_mask = rgb_conv_mask_get(tmp_sdef.nibble.green_len, tmp_sdef.nibble.green_pos, tmp_ddef.nibble.green_len, tmp_ddef.nibble.green_pos); \
		stage->blue_shift = rgb_conv_shift_get(tmp_sdef.nibble.blue_len, tmp_sdef.nibble.blue_pos, tmp_ddef.nibble.blue_len, tmp_ddef.nibble.blue_pos); \
		stage->blue_mask = rgb_conv_mask_get(tmp_sdef.nibble.blue_len, tmp_sdef.nibble.blue_pos, tmp_ddef.nibble.blue_len, tmp_ddef.nibble.blue_pos); \
		stage->ssp = color_def_bytes_per_pixel_get(tmp_sdef.ordinal); \
		stage->dsp = color_def_bytes_per_pixel_get(tmp_ddef.ordinal); \
	} while (0)


#include "vstretch.h"
#include "vmax.h"
#include "vmean.h"
#include "vcopy.h"
#include "vrot.h"
#include "vswap.h"
#include "vfilter.h"
#include "vconv.h"
#include "vpalette.h"

#ifndef USE_BLIT_TINY
#include "vrgb.h"
#endif

/***************************************************************************/
/* fast_buffer */

/* A very fast dynamic buffers allocations */

/* Max number of allocable buffers */
#define FAST_BUFFER_MAX 128

/* Total size of the buffers */
#define FAST_BUFFER_SIZE (256*1024)

/* Align mask */
#define FAST_BUFFER_ALIGN_MASK 0x1F

void* fast_buffer; /* raw pointer */
void* fast_buffer_aligned; /* aligned pointer */
unsigned fast_buffer_map[FAST_BUFFER_MAX]; /* stack of incremental size used */
unsigned fast_buffer_mac; /* top of the stack */

static void* video_buffer_alloc(unsigned size)
{
	unsigned size_aligned = (size + FAST_BUFFER_ALIGN_MASK) & ~FAST_BUFFER_ALIGN_MASK;
	assert( fast_buffer_mac < FAST_BUFFER_MAX );

	++fast_buffer_mac;
	fast_buffer_map[fast_buffer_mac] = fast_buffer_map[fast_buffer_mac-1] + size_aligned;

	assert( fast_buffer_map[fast_buffer_mac] <= FAST_BUFFER_SIZE);

	return (uint8*)fast_buffer_aligned + fast_buffer_map[fast_buffer_mac-1];
}

/* Buffers must be allocated and freed in exact reverse order */
static void video_buffer_free(void* buffer)
{
	(void)buffer;
	--fast_buffer_mac;
}

/* Debug version of the alloc functions */
#ifndef NDEBUG

#define WRAP_SIZE 32

static void* video_buffer_alloc_wrap(unsigned size)
{
	uint8* buffer8 = (uint8*)video_buffer_alloc(size + WRAP_SIZE);
	unsigned i;
	for(i=0;i<WRAP_SIZE;++i)
		buffer8[i] = i;
	return buffer8 + WRAP_SIZE;
}

static void video_buffer_free_wrap(void* buffer)
{
	uint8* buffer8 = (uint8*)buffer - WRAP_SIZE;
	unsigned i;
	for(i=0;i<WRAP_SIZE;++i)
		assert(buffer8[i] == i);
	video_buffer_free(buffer8);
}

#define video_buffer_free video_buffer_free_wrap
#define video_buffer_alloc video_buffer_alloc_wrap

#endif

static void video_buffer_init(void)
{
	fast_buffer = malloc(FAST_BUFFER_SIZE + FAST_BUFFER_ALIGN_MASK);
	fast_buffer_aligned = (void*)(((unsigned)fast_buffer + FAST_BUFFER_ALIGN_MASK) & ~FAST_BUFFER_ALIGN_MASK);
	fast_buffer_mac = 0;
	fast_buffer_map[0] = 0;
}

static void video_buffer_done(void)
{
	assert(fast_buffer_mac == 0);
	free(fast_buffer);
}

/***************************************************************************/
/* init/done */

adv_error video_blit_init(void)
{
	unsigned i;

	if (blit_set_mmx() != 0) {
		error_set("This executable requires an MMX processor.\n");
		return -1;
	}

	video_buffer_init();

	return 0;
}

void video_blit_done(void)
{
	video_buffer_done();
}

/***************************************************************************/
/* stage helper */

static void stage_copy(const struct video_stage_horz_struct* stage, void* dst, void* src)
{
	if ((int)stage->sbpp == stage->sdp) {
		BLITTER(internal_copy8)(dst, src, stage->sdx * stage->sbpp);
	} else {
		switch (stage->sbpp) {
			case 1 : BLITTER(internal_copy8_step)(dst, src, stage->sdx, stage->sdp); break;
			case 2 : BLITTER(internal_copy16_step)(dst, src, stage->sdx, stage->sdp); break;
			case 4 : BLITTER(internal_copy32_step)(dst, src, stage->sdx, stage->sdp); break;
		}
	}
}

static void stage_max_vert_self(const struct video_stage_horz_struct* stage, void* dst, void* src)
{
	if ((int)stage->sbpp == stage->sdp) {
		switch (stage->sbpp) {
			case 1 : internal_max8_vert_self(dst, src, stage->sdx); break;
			case 2 : internal_max16_vert_self(dst, src, stage->sdx); break;
			case 4 : internal_max32_vert_self(dst, src, stage->sdx); break;
		}
	} else {
		switch (stage->sbpp) {
			case 1 : internal_max8_vert_self_step(dst, src, stage->sdx, stage->sdp); break;
			case 2 : internal_max16_vert_self_step(dst, src, stage->sdx, stage->sdp); break;
			case 4 : internal_max32_vert_self_step(dst, src, stage->sdx, stage->sdp); break;
		}
	}
}

static void stage_max_rgb_vert_self(const struct video_stage_horz_struct* stage, void* dst, void* src)
{
	if ((int)stage->sbpp == stage->sdp) {
		switch (stage->sbpp) {
			case 1 : internal_max_rgb8_vert_self(dst, src, stage->sdx); break;
			case 2 : internal_max_rgb16_vert_self(dst, src, stage->sdx); break;
			case 4 : internal_max_rgb32_vert_self(dst, src, stage->sdx); break;
		}
	} else {
		switch (stage->sbpp) {
			case 1 : internal_max_rgb8_vert_self_step(dst, src, stage->sdx, stage->sdp); break;
			case 2 : internal_max_rgb16_vert_self_step(dst, src, stage->sdx, stage->sdp); break;
			case 4 : internal_max_rgb32_vert_self_step(dst, src, stage->sdx, stage->sdp); break;
		}
	}
}

static void stage_mean_vert_self(const struct video_stage_horz_struct* stage, void* dst, void* src)
{
	if ((int)stage->sbpp == stage->sdp) {
		switch (stage->sbpp) {
			case 1 : BLITTER(internal_mean8_vert_self)(dst, src, stage->sdx); break;
			case 2 : BLITTER(internal_mean16_vert_self)(dst, src, stage->sdx); break;
			case 4 : BLITTER(internal_mean32_vert_self)(dst, src, stage->sdx); break;
		}
	} else {
		switch (stage->sbpp) {
			case 1 : internal_mean8_vert_self_step(dst, src, stage->sdx, stage->sdp); break;
			case 2 : internal_mean16_vert_self_step(dst, src, stage->sdx, stage->sdp); break;
			case 4 : internal_mean32_vert_self_step(dst, src, stage->sdx, stage->sdp); break;
		}
	}
}

#ifndef USE_BLIT_TINY
static void scale2x(void* dst0, void* dst1, void* src0, void* src1, void* src2, unsigned bytes_per_pixel, unsigned count)
{
	switch (bytes_per_pixel) {
		case 1 : BLITTER(scale2x_8)(dst0, dst1, src0, src1, src2, count); break;
		case 2 : BLITTER(scale2x_16)(dst0, dst1, src0, src1, src2, count); break;
		case 4 : BLITTER(scale2x_32)(dst0, dst1, src0, src1, src2, count); break;
	}
}

static void lq2x(void* dst0, void* dst1, void* src0, void* src1, void* src2, unsigned bytes_per_pixel, unsigned count)
{
	switch (bytes_per_pixel) {
		case 2 : lq2x_16_def(dst0, dst1, src0, src1, src2, count); break;
		case 4 : lq2x_32_def(dst0, dst1, src0, src1, src2, count); break;
	}
}

#ifndef USE_BLIT_SMALL
static void hq2x(void* dst0, void* dst1, void* src0, void* src1, void* src2, unsigned bytes_per_pixel, unsigned count)
{
	switch (bytes_per_pixel) {
		case 2 : hq2x_16_def(dst0, dst1, src0, src1, src2, count); break;
		case 4 : hq2x_32_def(dst0, dst1, src0, src1, src2, count); break;
	}
}
#endif

static void scale3x(void* dst0, void* dst1, void* dst2, void* src0, void* src1, void* src2, unsigned bytes_per_pixel, unsigned count)
{
	switch (bytes_per_pixel) {
		case 1 : scale3x_8_def(dst0, dst1, dst2, src0, src1, src2, count); break;
		case 2 : scale3x_16_def(dst0, dst1, dst2, src0, src1, src2, count); break;
		case 4 : scale3x_32_def(dst0, dst1, dst2, src0, src1, src2, count); break;
	}
}

static void lq3x(void* dst0, void* dst1, void* dst2, void* src0, void* src1, void* src2, unsigned bytes_per_pixel, unsigned count)
{
	switch (bytes_per_pixel) {
		case 2 : lq3x_16_def(dst0, dst1, dst2, src0, src1, src2, count); break;
		case 4 : lq3x_32_def(dst0, dst1, dst2, src0, src1, src2, count); break;
	}
}

#ifndef USE_BLIT_SMALL
static void hq3x(void* dst0, void* dst1, void* dst2, void* src0, void* src1, void* src2, unsigned bytes_per_pixel, unsigned count)
{
	switch (bytes_per_pixel) {
		case 2 : hq3x_16_def(dst0, dst1, dst2, src0, src1, src2, count); break;
		case 4 : hq3x_32_def(dst0, dst1, dst2, src0, src1, src2, count); break;
	}
}
#endif

static void stage_scale2x(const struct video_stage_horz_struct* stage, void* dst0, void* dst1, void* src0, void* src1, void* src2)
{
	if ((int)stage->sbpp == stage->sdp) {
		scale2x(dst0, dst1, src0, src1, src2, stage->sbpp, stage->sdx);
	}
}

static void stage_lq2x(const struct video_stage_horz_struct* stage, void* dst0, void* dst1, void* src0, void* src1, void* src2)
{
	if ((int)stage->sbpp == stage->sdp) {
		lq2x(dst0, dst1, src0, src1, src2, stage->sbpp, stage->sdx);
	}
}

#ifndef USE_BLIT_SMALL
static void stage_hq2x(const struct video_stage_horz_struct* stage, void* dst0, void* dst1, void* src0, void* src1, void* src2)
{
	if ((int)stage->sbpp == stage->sdp) {
		hq2x(dst0, dst1, src0, src1, src2, stage->sbpp, stage->sdx);
	}
}
#endif

static void stage_scale3x(const struct video_stage_horz_struct* stage, void* dst0, void* dst1, void* dst2, void* src0, void* src1, void* src2)
{
	if ((int)stage->sbpp == stage->sdp) {
		scale3x(dst0, dst1, dst2, src0, src1, src2, stage->sbpp, stage->sdx);
	}
}

static void stage_lq3x(const struct video_stage_horz_struct* stage, void* dst0, void* dst1, void* dst2, void* src0, void* src1, void* src2)
{
	if ((int)stage->sbpp == stage->sdp) {
		lq3x(dst0, dst1, dst2, src0, src1, src2, stage->sbpp, stage->sdx);
	}
}

#ifndef USE_BLIT_SMALL
static void stage_hq3x(const struct video_stage_horz_struct* stage, void* dst0, void* dst1, void* dst2, void* src0, void* src1, void* src2)
{
	if ((int)stage->sbpp == stage->sdp) {
		hq3x(dst0, dst1, dst2, src0, src1, src2, stage->sbpp, stage->sdx);
	}
}
#endif

static void lq4x(void* dst0, void* dst1, void* dst2, void* dst3, void* src0, void* src1, void* src2, unsigned bytes_per_pixel, unsigned count)
{
	switch (bytes_per_pixel) {
		case 2 : lq4x_16_def(dst0, dst1, dst2, dst3, src0, src1, src2, count); break;
		case 4 : lq4x_32_def(dst0, dst1, dst2, dst3, src0, src1, src2, count); break;
	}
}

#ifndef USE_BLIT_SMALL
static void hq4x(void* dst0, void* dst1, void* dst2, void* dst3, void* src0, void* src1, void* src2, unsigned bytes_per_pixel, unsigned count)
{
	switch (bytes_per_pixel) {
		case 2 : hq4x_16_def(dst0, dst1, dst2, dst3, src0, src1, src2, count); break;
		case 4 : hq4x_32_def(dst0, dst1, dst2, dst3, src0, src1, src2, count); break;
	}
}
#endif

static void stage_scale4x(const struct video_stage_horz_struct* stage, void* dst0, void* dst1, void* dst2, void* dst3, void* src0, void* src1, void* src2, void* src3)
{
	scale2x(dst0, dst1, src0, src1, src2, stage->sbpp, 2 * stage->sdx);
	scale2x(dst2, dst3, src1, src2, src3, stage->sbpp, 2 * stage->sdx);
}

static void stage_lq4x(const struct video_stage_horz_struct* stage, void* dst0, void* dst1, void* dst2, void* dst3, void* src0, void* src1, void* src2)
{
	if ((int)stage->sbpp == stage->sdp) {
		lq4x(dst0, dst1, dst2, dst3, src0, src1, src2, stage->sbpp, stage->sdx);
	}
}

#ifndef USE_BLIT_SMALL
static void stage_hq4x(const struct video_stage_horz_struct* stage, void* dst0, void* dst1, void* dst2, void* dst3, void* src0, void* src1, void* src2)
{
	if ((int)stage->sbpp == stage->sdp) {
		hq4x(dst0, dst1, dst2, dst3, src0, src1, src2, stage->sbpp, stage->sdx);
	}
}
#endif

static void stage_scale2x_direct(const struct video_stage_vert_struct* stage, void* dst0, void* dst1, void* src0, void* src1, void* src2)
{
	if ((int)stage->stage_pivot_sbpp == stage->stage_pivot_sdp) {
		scale2x(dst0, dst1, src0, src1, src2, stage->stage_pivot_sbpp, stage->stage_pivot_sdx);
	}
}

static void stage_lq2x_direct(const struct video_stage_vert_struct* stage, void* dst0, void* dst1, void* src0, void* src1, void* src2)
{
	if ((int)stage->stage_pivot_sbpp == stage->stage_pivot_sdp) {
		lq2x(dst0, dst1, src0, src1, src2, stage->stage_pivot_sbpp, stage->stage_pivot_sdx);
	}
}

#ifndef USE_BLIT_SMALL
static void stage_hq2x_direct(const struct video_stage_vert_struct* stage, void* dst0, void* dst1, void* src0, void* src1, void* src2)
{
	if ((int)stage->stage_pivot_sbpp == stage->stage_pivot_sdp) {
		hq2x(dst0, dst1, src0, src1, src2, stage->stage_pivot_sbpp, stage->stage_pivot_sdx);
	}
}
#endif

static void stage_scale3x_direct(const struct video_stage_vert_struct* stage, void* dst0, void* dst1, void* dst2, void* src0, void* src1, void* src2)
{
	if ((int)stage->stage_pivot_sbpp == stage->stage_pivot_sdp) {
		scale3x(dst0, dst1, dst2, src0, src1, src2, stage->stage_pivot_sbpp, stage->stage_pivot_sdx);
	}
}

static void stage_lq3x_direct(const struct video_stage_vert_struct* stage, void* dst0, void* dst1, void* dst2, void* src0, void* src1, void* src2)
{
	if ((int)stage->stage_pivot_sbpp == stage->stage_pivot_sdp) {
		lq3x(dst0, dst1, dst2, src0, src1, src2, stage->stage_pivot_sbpp, stage->stage_pivot_sdx);
	}
}

#ifndef USE_BLIT_SMALL
static void stage_hq3x_direct(const struct video_stage_vert_struct* stage, void* dst0, void* dst1, void* dst2, void* src0, void* src1, void* src2)
{
	if ((int)stage->stage_pivot_sbpp == stage->stage_pivot_sdp) {
		hq3x(dst0, dst1, dst2, src0, src1, src2, stage->stage_pivot_sbpp, stage->stage_pivot_sdx);
	}
}
#endif

static void stage_scale4x_direct(const struct video_stage_vert_struct* stage, void* dst0, void* dst1, void* dst2, void* dst3, void* src0, void* src1, void* src2, void* src3)
{
	scale2x(dst0, dst1, src0, src1, src2, stage->stage_pivot_sbpp, 2 * stage->stage_pivot_sdx);
	scale2x(dst2, dst3, src1, src2, src3, stage->stage_pivot_sbpp, 2 * stage->stage_pivot_sdx);
}

static void stage_lq4x_direct(const struct video_stage_vert_struct* stage, void* dst0, void* dst1, void* dst2, void* dst3, void* src0, void* src1, void* src2)
{
	if ((int)stage->stage_pivot_sbpp == stage->stage_pivot_sdp) {
		lq4x(dst0, dst1, dst2, dst3, src0, src1, src2, stage->stage_pivot_sbpp, stage->stage_pivot_sdx);
	}
}

#ifndef USE_BLIT_SMALL
static void stage_hq4x_direct(const struct video_stage_vert_struct* stage, void* dst0, void* dst1, void* dst2, void* dst3, void* src0, void* src1, void* src2)
{
	if ((int)stage->stage_pivot_sbpp == stage->stage_pivot_sdp) {
		hq4x(dst0, dst1, dst2, dst3, src0, src1, src2, stage->stage_pivot_sbpp, stage->stage_pivot_sdx);
	}
}
#endif
#endif

/***************************************************************************/
/* stage/pipeline */

static unsigned char* video_line(const struct video_pipeline_target_struct* target, unsigned y)
{
	return video_write_line(y);
}

static unsigned char* memory_line(const struct video_pipeline_target_struct* target, unsigned y)
{
	return (unsigned char*)target->ptr + y * target->bytes_per_scanline;
}

void video_pipeline_init(struct video_pipeline_struct* pipeline)
{
	pipeline->stage_mac = 0;
	pipeline->target.line = &video_line;
	pipeline->target.ptr = 0;
	pipeline->target.color_def = video_color_def();
	pipeline->target.bytes_per_pixel = video_bytes_per_pixel();
	pipeline->target.bytes_per_scanline = video_bytes_per_scanline();
}

void video_pipeline_target(struct video_pipeline_struct* pipeline, void* ptr, unsigned bytes_per_scanline, adv_color_def def)
{
	pipeline->target.line = &memory_line;
	pipeline->target.ptr = ptr;
	pipeline->target.color_def = def;
	pipeline->target.bytes_per_pixel = color_def_bytes_per_pixel_get(def);
	pipeline->target.bytes_per_scanline = bytes_per_scanline;
}

void video_pipeline_done(struct video_pipeline_struct* pipeline)
{
	int i;

	if (pipeline->stage_mac) {
		/* deallocate with the same allocation order */
		for(i=pipeline->stage_mac-1;i>=0;--i) {
			struct video_stage_horz_struct* stage = &pipeline->stage_map[i];
			if (stage->buffer_extra)
				video_buffer_free(stage->buffer_extra);
		}
		for(i=pipeline->stage_mac-1;i>=0;--i) {
			struct video_stage_horz_struct* stage = &pipeline->stage_map[i];
			if (stage->buffer)
				video_buffer_free(stage->buffer);
		}
	}
}

static inline struct video_stage_horz_struct* video_pipeline_begin_mutable(struct video_pipeline_struct* pipeline)
{
	return pipeline->stage_map;
}

static inline struct video_stage_horz_struct* video_pipeline_end_mutable(struct video_pipeline_struct* pipeline)
{
	return pipeline->stage_map + pipeline->stage_mac;
}

static inline struct video_stage_vert_struct* video_pipeline_vert_mutable(struct video_pipeline_struct* pipeline)
{
	return &pipeline->stage_vert;
}

static inline struct video_stage_horz_struct* video_pipeline_insert(struct video_pipeline_struct* pipeline)
{
	struct video_stage_horz_struct* stage = video_pipeline_end_mutable(pipeline);
	++pipeline->stage_mac;
	return stage;
}

static struct video_stage_horz_struct* video_pipeline_substitute(struct video_pipeline_struct* pipeline, struct video_stage_horz_struct* begin, struct video_stage_horz_struct* end)
{
	struct video_stage_horz_struct* last = video_pipeline_end_mutable(pipeline);
	pipeline->stage_mac += 1 - (end - begin);
	while (end != last) {
		++begin;
		memcpy(begin, end, sizeof(struct video_stage_horz_struct));
		++end;
	}
	return begin;
}

static void video_pipeline_realize(struct video_pipeline_struct* pipeline, int sdx, int sdp, int sbpp)
{
	struct video_stage_vert_struct* stage_vert = video_pipeline_vert_mutable(pipeline);
	struct video_stage_horz_struct* stage_begin = video_pipeline_begin_mutable(pipeline);
	struct video_stage_horz_struct* stage_end = video_pipeline_end_mutable(pipeline);

	if (stage_begin != stage_end && stage_vert->stage_pivot != stage_end) {
		/* the vertical stage is in the middle of the pipeline */
		struct video_stage_horz_struct* stage = stage_begin;
		struct video_stage_horz_struct* stage_blit = stage_end - 1;
		while (stage != stage_blit) {
			if (stage->buffer_size) {
				stage->buffer = video_buffer_alloc(stage->buffer_size);
			} else {
				stage->buffer = 0;
			}
			++stage;
		}
		stage_blit->buffer = 0;

		stage_vert->stage_pivot_sdp = stage_vert->stage_pivot->sdp;
		stage_vert->stage_pivot_sdx = stage_vert->stage_pivot->sdx;
		stage_vert->stage_pivot_sbpp = stage_vert->stage_pivot->sbpp;
	} else {
		/* the vertical stage is at the end of the pipeline */
		struct video_stage_horz_struct* stage = stage_begin;
		while (stage != stage_end) {
			if (stage->buffer_size) {
				stage->buffer = video_buffer_alloc(stage->buffer_size);
			} else {
				stage->buffer = 0;
			}
			++stage;
		}

		/* the pivot source is the last stage */
		stage_vert->stage_pivot_sdp = sdp;
		stage_vert->stage_pivot_sdx = sdx;
		stage_vert->stage_pivot_sbpp = sbpp;
	}

	/* allocate the extra buffer */
	{
		struct video_stage_horz_struct* stage = stage_begin;
		while (stage != stage_end) {
			if (stage->buffer_extra_size) {
				stage->buffer_extra = video_buffer_alloc(stage->buffer_extra_size);
			} else {
				stage->buffer_extra = 0;
			}

			++stage;
		}
	}

}

/* Run a partial pipeline */
static void* video_pipeline_run_partial(const struct video_stage_horz_struct* stage_begin, const struct video_stage_horz_struct* stage_end, const void* src)
{
	if (stage_begin == stage_end) {
		return (void*)src;
	} else {
		stage_begin->put(stage_begin, stage_begin->buffer, src);
		++stage_begin;

		while (stage_begin != stage_end) {
			stage_begin->put(stage_begin, stage_begin->buffer, stage_begin[-1].buffer);
			++stage_begin;
		}

		return stage_begin[-1].buffer;
	}
}

/* Run a partial pipeline and store the result in the specified buffer */
static void* video_pipeline_run_partial_on_buffer(void* dst_buffer, const struct video_stage_horz_struct* stage_begin, const struct video_stage_horz_struct* stage_end, const void* src)
{
	if (stage_begin == stage_end) {
		return (void*)src;
	} else {
		const struct video_stage_horz_struct* stage_next = stage_begin + 1;
		if (stage_next == stage_end) {
			stage_begin->put(stage_begin, dst_buffer, src);
		} else {
			stage_begin->put(stage_begin, stage_begin->buffer, src);
			++stage_begin;
			++stage_next;

			while (stage_next != stage_end) {
				stage_begin->put(stage_begin, stage_begin->buffer, stage_begin[-1].buffer);
				++stage_begin;
				++stage_next;
			}

			stage_begin->put(stage_begin, dst_buffer, stage_begin[-1].buffer);
		}

		return dst_buffer;
	}
}

static void video_pipeline_run(const struct video_stage_horz_struct* stage_begin, const struct video_stage_horz_struct* stage_end, void* dst, const void* src)
{
	--stage_end;
	if (stage_begin == stage_end) {
		stage_begin->put(stage_begin, dst, src);
	} else {
		stage_begin->put(stage_begin, stage_begin->buffer, src);
		++stage_begin;

		while (stage_begin != stage_end) {
			stage_begin->put(stage_begin, stage_begin->buffer, stage_begin[-1].buffer);
			++stage_begin;
		}

		stage_begin->put(stage_begin, dst, stage_begin[-1].buffer);
	}
}

static void video_pipeline_run_plain(const struct video_stage_horz_struct* stage_begin, const struct video_stage_horz_struct* stage_end, void* dst, void* src)
{
	--stage_end;
	if (stage_begin == stage_end) {
		stage_begin->put_plain(stage_begin, dst, src);
	} else {
		stage_begin->put_plain(stage_begin, stage_begin->buffer, src);
		++stage_begin;

		while (stage_begin != stage_end) {
			stage_begin->put(stage_begin, stage_begin->buffer, stage_begin[-1].buffer);
			++stage_begin;
		}

		stage_begin->put(stage_begin, dst, stage_begin[-1].buffer);
	}
}

static inline void video_pipeline_vert_run(const struct video_pipeline_struct* pipeline, unsigned x, unsigned y, const void* src)
{
	/* clear the states */
	const struct video_stage_horz_struct* begin = video_pipeline_begin(pipeline);
	const struct video_stage_horz_struct* end = video_pipeline_end(pipeline);
	while (begin != end) {
		assert(begin);
		((struct video_stage_horz_struct*)begin)->state_mutable = 0;
		++begin;
	}

	/* draw */
	assert(pipeline->stage_vert.put);
	assert(video_pipeline_vert(pipeline)->put);
	video_pipeline_vert(pipeline)->put(&pipeline->target, video_pipeline_vert(pipeline), x, y, src);

	/* restore the MMX micro state */
	internal_end();
}

const char* pipe_name(enum video_stage_enum pipe)
{
	switch (pipe) {
		case pipe_x_stretch : return "hstretch";
		case pipe_x_max : return "hmax";
		case pipe_x_mean : return "hmean";
		case pipe_x_double : return "hcopy x2";
		case pipe_x_triple : return "hcopy x3";
		case pipe_x_quadruple : return "hcopy x4";
		case pipe_x_filter : return "hfilter";
		case pipe_x_copy : return "hcopy";
		case pipe_rotation : return "rotation";
		case pipe_x_rgb_triad3pix : return "rgb 3";
		case pipe_x_rgb_triad6pix : return "rgb 6";
		case pipe_x_rgb_triad16pix : return "rgb 16";
		case pipe_x_rgb_triadstrong3pix : return "rgb strong 3";
		case pipe_x_rgb_triadstrong6pix : return "rgb strong 6";
		case pipe_x_rgb_triadstrong16pix : return "rgb strong 16";
		case pipe_x_rgb_scandoublehorz : return "hscanline x2";
		case pipe_x_rgb_scantriplehorz : return "hscanline x3";
		case pipe_x_rgb_scandoublevert : return "vscanline x2";
		case pipe_x_rgb_scantriplevert : return "vscanline x3";
		case pipe_swap_even : return "swap even";
		case pipe_swap_odd : return "swap odd";
		case pipe_interlace_filter : return "vfilter";
		case pipe_palette8to8 : return "palette 8>8";
		case pipe_palette8to16 : return "palette 8>16";
		case pipe_palette8to32 : return "palette 8>32";
		case pipe_palette16to8 : return "palette 16>8";
		case pipe_palette16to16 : return "palette 16>16";
		case pipe_palette16to32 : return "palette 16>32";
		case pipe_imm16to8 : return "conv 16>8";
		case pipe_imm16to32 : return "conv 16>32";
		case pipe_bgra8888tobgr332 : return "bgra 8888>bgr 332";
		case pipe_bgra8888tobgr565 : return "bgra 8888>bgr 565";
		case pipe_bgra8888tobgra5551 : return "bgra 8888>bgra 5551";
		case pipe_bgra8888toyuy2 : return "bgra 8888>yuy2";
		case pipe_bgra5551tobgr332 : return "bgra 5551>bgr 332";
		case pipe_bgra5551tobgr565 : return "bgra 5551>bgr 565";
		case pipe_bgra5551tobgra8888 : return "bgra 5551>bgra 8888";
		case pipe_bgra5551toyuy2 : return "bgra 5551>yuy2";
		case pipe_rgb888tobgra8888 : return "rgb 888>bgra 8888";
		case pipe_rgba8888tobgra8888 : return "rgba 8888>bgra 8888";
		case pipe_bgr888tobgra8888 : return "bgr 888>bgra 8888";
		case pipe_rgbtorgb : return "rgb>rgb";
		case pipe_rgbtoyuy2 : return "rgb>yuy2";
		case pipe_y_copy : return "vcopy";
		case pipe_y_reduction_copy : return "vreduction";
		case pipe_y_expansion_copy : return "vexpansion";
		case pipe_y_mean : return "vcopy mean";
		case pipe_y_reduction_mean : return "vreduction mean";
		case pipe_y_expansion_mean : return "vexpansion mean";
		case pipe_y_filter : return "vcopy lowpass";
		case pipe_y_reduction_filter : return "vreduction low pass";
		case pipe_y_expansion_filter : return "vexpansion low pass";
		case pipe_y_reduction_max  : return "vreduction max";
		case pipe_y_scale2x : return "scale2x";
		case pipe_y_scale3x : return "scale3x";
		case pipe_y_scale4x : return "scale4x";
		case pipe_y_lq2x : return "lq2x";
		case pipe_y_lq3x : return "lq3x";
		case pipe_y_lq4x : return "lq4x";
#ifndef USE_BLIT_SMALL
		case pipe_y_hq2x : return "hq2x";
		case pipe_y_hq3x : return "hq3x";
		case pipe_y_hq4x : return "hq4x";
#endif
	}
	return 0;
}

/* Check is the stage change the color format */
/* These stages MUST be BEFORE any RGB color operation */
static adv_bool pipe_is_conversion(enum video_stage_enum pipe)
{
	switch (pipe) {
		case pipe_palette8to8 :
		case pipe_palette8to16 :
		case pipe_palette8to32 :
		case pipe_palette16to8 :
		case pipe_palette16to16 :
		case pipe_palette16to32 :
		case pipe_imm16to8 :
		case pipe_imm16to32 :
		case pipe_bgra8888tobgr332 :
		case pipe_bgra8888tobgr565 :
		case pipe_bgra8888tobgra5551 :
		case pipe_bgra8888toyuy2 :
		case pipe_bgra5551tobgr332 :
		case pipe_bgra5551tobgr565 :
		case pipe_bgra5551tobgra8888 :
		case pipe_bgra5551toyuy2 :
		case pipe_rgb888tobgra8888 :
		case pipe_rgba8888tobgra8888 :
		case pipe_bgr888tobgra8888 :
		case pipe_rgbtorgb :
		case pipe_rgbtoyuy2 :
			return 1;
		default:
			return 0;
	}
}

/* Check is the stage decorate the image */
/* These stages MUST be AFTER any change of size */
static adv_bool pipe_is_decoration(enum video_stage_enum pipe)
{
	switch (pipe) {
		case pipe_x_rgb_triad3pix :
		case pipe_x_rgb_triad6pix :
		case pipe_x_rgb_triad16pix :
		case pipe_x_rgb_triadstrong3pix :
		case pipe_x_rgb_triadstrong6pix :
		case pipe_x_rgb_triadstrong16pix :
		case pipe_x_rgb_scandoublehorz :
		case pipe_x_rgb_scantriplehorz :
		case pipe_x_rgb_scandoublevert :
		case pipe_x_rgb_scantriplevert :
		case pipe_swap_even :
		case pipe_swap_odd :
		case pipe_interlace_filter :
			return 1;
		default:
			return 0;
	}
}

/* Check if the write operation is done writing the biggest register size */
static adv_bool pipe_is_fastwrite(const struct video_stage_horz_struct* stage)
{
#ifdef USE_MTRR
	/* if MTRR is enabled, write is always fast */
	return 1;
#else
	if (the_blit_mmx) {
		adv_bool is_plain = stage->sbpp == stage->sdp;
		switch (stage->type) {
			case pipe_x_copy : return 1;
			case pipe_rotation : return 1;
			case pipe_x_double : return is_plain;
			case pipe_x_rgb_triad3pix : return is_plain;
			case pipe_x_rgb_triad6pix : return is_plain;
			case pipe_x_rgb_triad16pix : return is_plain;
			case pipe_x_rgb_triadstrong3pix : return is_plain;
			case pipe_x_rgb_triadstrong6pix : return is_plain;
			case pipe_x_rgb_triadstrong16pix : return is_plain;
			case pipe_x_rgb_scandoublehorz : return is_plain;
			case pipe_x_rgb_scantriplehorz : return is_plain;
			case pipe_x_rgb_scandoublevert : return is_plain;
			case pipe_x_rgb_scantriplevert : return is_plain;
			case pipe_swap_even : return 1;
			case pipe_swap_odd : return 1;
			case pipe_interlace_filter : return 1;
			case pipe_palette8to16 : return is_plain;
			case pipe_palette16to8 : return 1;
			case pipe_palette16to16 : return 1;
			case pipe_palette16to32 : return 1;
			case pipe_bgra8888tobgr332 : return is_plain;
			case pipe_bgra8888tobgra5551 : return is_plain;
			case pipe_bgra8888tobgr565 : return is_plain;
			case pipe_bgra8888toyuy2 : return 1;
			case pipe_bgra5551tobgr332 : return is_plain;
			case pipe_bgra5551tobgr565 : return is_plain;
			case pipe_bgra5551tobgra8888 : return is_plain;
			case pipe_bgra5551toyuy2 : return 1;
			case pipe_x_filter : return is_plain;
			default: return 0;
		}
	} else {
		switch (stage->type) {
			case pipe_x_stretch : return 0;
			case pipe_x_max : return 0;
			case pipe_x_mean : return 0;
			default: return 1;
		}
	}
#endif
}

/* Check if the write operation is done converting the RGB values */
static adv_bool combine_is_rgb(unsigned stage)
{
	switch (stage) {
	case VIDEO_COMBINE_Y_MEAN :
	case VIDEO_COMBINE_Y_FILTER :
#ifndef USE_BLIT_TINY
	case VIDEO_COMBINE_Y_LQ2X :
	case VIDEO_COMBINE_Y_LQ3X :
	case VIDEO_COMBINE_Y_LQ4X :
#ifndef USE_BLIT_SMALL
	case VIDEO_COMBINE_Y_HQ2X :
	case VIDEO_COMBINE_Y_HQ3X :
	case VIDEO_COMBINE_Y_HQ4X :
#endif
#endif
		return 1;
	default:
		return 0;
	}
}

/* Check if the write operation support direct writing without requiring a final stage */
static adv_bool combine_is_direct(unsigned stage)
{
	switch (stage) {
#ifndef USE_BLIT_TINY
	case VIDEO_COMBINE_Y_SCALE2X :
	case VIDEO_COMBINE_Y_SCALE3X :
	case VIDEO_COMBINE_Y_SCALE4X :
	case VIDEO_COMBINE_Y_LQ2X :
	case VIDEO_COMBINE_Y_LQ3X :
	case VIDEO_COMBINE_Y_LQ4X :
#ifndef USE_BLIT_SMALL
	case VIDEO_COMBINE_Y_HQ2X :
	case VIDEO_COMBINE_Y_HQ3X :
	case VIDEO_COMBINE_Y_HQ4X :
#endif
		return 1;
#endif
	default:
		return 0;
	}
}

/* Check if the write operation is done writing the biggest register size */
static adv_bool combine_is_fastwrite(unsigned stage, unsigned bytes_per_pixel)
{
#ifdef USE_MTRR
	/* if MTRR is enabled, write is always fast */
	return 1;
#else
	if (the_blit_mmx) {
#ifndef USE_BLIT_TINY
		switch (stage) {
		case VIDEO_COMBINE_Y_SCALE2X :
		case VIDEO_COMBINE_Y_SCALE4X :
			return 1;
		default :
			return 0;
		}
#else
		return 0;
#endif
	} else {
		return bytes_per_pixel >= 4;
	}
#endif
}

/***************************************************************************/
/* stretchy reduction */

/* This example explains the difference of the effects in reduction

  ORI   COPY  MEAN  FILTER
  A     A     A+B+C A         (first line)
  B     D     D+E   C+D
  C     F     F+G+H E+F
  D     I     I+J   H+I
  ...

*/

static void video_stage_stretchy_x1(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;

	int whole = stage_vert->slice.whole;
	int up = stage_vert->slice.up;
	int down = stage_vert->slice.down;
	int error = stage_vert->slice.error;
	int count = stage_vert->slice.count;

	while (count) {
		void* dst;
		unsigned run = whole;
		if ((error += up) > 0) {
			++run;
			error -= down;
		}

		dst = target->line(target, y) + x_off;
		video_pipeline_run(stage_vert->stage_begin, stage_vert->stage_end, dst, src);
		++y;

		PADD(src, stage_vert->sdw * run);
		--count;
	}
}

static void video_stage_stretchy_max_x1(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* buffer = video_buffer_alloc(stage_vert->stage_begin->sdx * stage_vert->stage_begin->sbpp);

	int whole = stage_vert->slice.whole;
	int up = stage_vert->slice.up;
	int down = stage_vert->slice.down;
	int error = stage_vert->slice.error;
	int count = stage_vert->slice.count;

	while (count) {
		void* dst;
		unsigned run = whole;
		if ((error += up) > 0) {
			++run;
			error -= down;
		}

		dst = target->line(target, y) + x_off;

		if (count == 1)
			run = 1;
		if (run == 1) {
			video_pipeline_run(stage_begin, stage_end, dst, src);
			PADD(src, stage_vert->sdw);
		} else {
			void* src_buffer = video_pipeline_run_partial(stage_begin, stage_pivot, src);
			stage_copy(stage_pivot, buffer, src_buffer);
			PADD(src, stage_vert->sdw);
			--run;

			while (run) {
				src_buffer = video_pipeline_run_partial(stage_begin, stage_pivot, src);
				if (color_def_type_get(target->color_def) == adv_color_type_rgb)
					stage_max_rgb_vert_self(stage_pivot, buffer, src_buffer);
				else
					stage_max_vert_self(stage_pivot, buffer, src_buffer);
				PADD(src, stage_vert->sdw);
				--run;
			}

			video_pipeline_run_plain(stage_vert->stage_pivot, stage_end, dst, buffer);
		}
		++y;
		--count;
	}

	video_buffer_free(buffer);
}

/* Compute the mean of every lines reduced to a single line */
static void video_stage_stretchy_mean_x1(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* buffer = video_buffer_alloc(stage_pivot->sdx * stage_pivot->sbpp);

	int whole = stage_vert->slice.whole;
	int up = stage_vert->slice.up;
	int down = stage_vert->slice.down;
	int error = stage_vert->slice.error;
	int count = stage_vert->slice.count;

	while (count) {
		void* dst;
		unsigned run = whole;
		if ((error += up) > 0) {
			++run;
			error -= down;
		}

		dst = target->line(target, y) + x_off;

		if (count == 1)
			run = 1;
		if (run == 1) {
			video_pipeline_run(stage_begin, stage_end, dst, src);
			PADD(src, stage_vert->sdw);
		} else {
			void* src_buffer = video_pipeline_run_partial(stage_begin, stage_pivot, src);
			stage_copy(stage_pivot, buffer, src_buffer);
			PADD(src, stage_vert->sdw);
			--run;

			while (run) {
				src_buffer = video_pipeline_run_partial(stage_begin, stage_pivot, src);
				stage_mean_vert_self(stage_pivot, buffer, src_buffer);
				PADD(src, stage_vert->sdw);
				--run;
			}

			video_pipeline_run_plain(stage_pivot, stage_end, dst, buffer);
		}
		++y;
		--count;
	}

	video_buffer_free(buffer);
}

/* Compute the mean of the previous line and the first of every iteration */
static void video_stage_stretchy_filter_x1(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	adv_bool buffer_full = 0;
	void* buffer = video_buffer_alloc(stage_pivot->sdx * stage_pivot->sbpp);

	int whole = stage_vert->slice.whole;
	int up = stage_vert->slice.up;
	int down = stage_vert->slice.down;
	int error = stage_vert->slice.error;
	int count = stage_vert->slice.count;

	while (count) {
		void* dst;
		void* src_buffer;
		unsigned run = whole;
		if ((error += up) > 0) {
			++run;
			error -= down;
		}

		dst = target->line(target, y) + x_off;
		src_buffer = video_pipeline_run_partial(stage_begin, stage_pivot, src);
		if (buffer_full) {
			stage_mean_vert_self(stage_pivot, buffer, src_buffer);
			video_pipeline_run_plain(stage_pivot, stage_end, dst, buffer);
		} else {
			video_pipeline_run(stage_pivot, stage_end, dst, src_buffer);
			buffer_full = 1;
		}

		if (count > 1) {
			if (run > 1) {
				PADD(src, (run - 1) * stage_vert->sdw);
				src_buffer = video_pipeline_run_partial(stage_begin, stage_pivot, src);
			}

			stage_copy(stage_pivot, buffer, src_buffer);
			PADD(src, stage_vert->sdw);

			++y;
		}

		--count;
	}

	video_buffer_free(buffer);
}

/***************************************************************************/
/* stretchy expansion */

/* This example explains the difference of effects in expansion

  ORI   COPY  MEAN/FILTER
  A     A     A     A         (first line)
	A     A     A (== A+A)
  B     B     A+B   A+B
	B     B     B (== B+B)
	B     B     B (== B+B)
  C     C     B+C   B+C
	C     C     C (== C+C)
  D     D     C+D   C+D
  E     E     E     D+E
	E     E     E (== E+E)
*/

static void video_stage_stretchy_1x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;

	int whole = stage_vert->slice.whole;
	int up = stage_vert->slice.up;
	int down = stage_vert->slice.down;
	int error = stage_vert->slice.error;
	int count = stage_vert->slice.count;

	while (count) {
		void* buffer;

		unsigned run = whole;
		if ((error += up) > 0) {
			++run;
			error -= down;
		}

		buffer = video_pipeline_run_partial(stage_vert->stage_begin, stage_vert->stage_pivot, src);
		while (run) {
			void* dst = target->line(target, y) + x_off;
			video_pipeline_run(stage_vert->stage_pivot, stage_vert->stage_end, dst, buffer);
			++y;
			--run;
		}

		PADD(src, stage_vert->sdw);
		--count;
	}
}

/* The mean effect is applied only at the first added line, if no line */
/* duplication is done, no effect is applied */
static void video_stage_stretchy_mean_1x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* buffer = video_buffer_alloc(stage_pivot->sdx * stage_pivot->sbpp);
	void* previous_buffer = 0;

	int whole = stage_vert->slice.whole;
	int up = stage_vert->slice.up;
	int down = stage_vert->slice.down;
	int error = stage_vert->slice.error;
	int count = stage_vert->slice.count;

	while (count) {
		void* src_buffer;
		void* dst;
		unsigned run = whole;
		if ((error += up) > 0) {
			++run;
			error -= down;
		}

		/* save the previous buffer if required for computation */
		if (previous_buffer)
			stage_copy(stage_pivot, buffer, previous_buffer);

		src_buffer = video_pipeline_run_partial(stage_begin, stage_pivot, src);
		dst = target->line(target, y) + x_off;

		/* don't apply the effect if no duplication is required */
		if (previous_buffer) {
			/* apply the mean effect only at the first line */
			stage_mean_vert_self(stage_pivot, buffer, src_buffer);
			video_pipeline_run_plain(stage_pivot, stage_end, dst, buffer);
		} else {
			video_pipeline_run(stage_pivot, stage_end, dst, src_buffer);
		}

		/* If some lines are duplicated save the current buffer */
		if (run >= 2)
			previous_buffer = src_buffer;
		else
			previous_buffer = 0;

		/* first line done */
		++y;
		--run;

		/* do other lines without any effects */
		while (run) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run(stage_pivot, stage_end, dst, src_buffer);
			++y;
			--run;
		}

		PADD(src, stage_vert->sdw);
		--count;
	}

	video_buffer_free(buffer);
}

/* The effect is applied at every line */
static void video_stage_stretchy_filter_1x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* buffer = video_buffer_alloc(stage_pivot->sdx * stage_pivot->sbpp);
	void* previous_buffer = 0;

	int whole = stage_vert->slice.whole;
	int up = stage_vert->slice.up;
	int down = stage_vert->slice.down;
	int error = stage_vert->slice.error;
	int count = stage_vert->slice.count;

	while (count) {
		void* src_buffer;
		void* dst;
		unsigned run = whole;
		if ((error += up) > 0) {
			++run;
			error -= down;
		}

		/* save the previous buffer if required for computation */
		if (previous_buffer)
			stage_copy(stage_pivot, buffer, previous_buffer);

		src_buffer = video_pipeline_run_partial(stage_begin, stage_pivot, src);
		dst = target->line(target, y) + x_off;

		if (previous_buffer) {
			/* apply the mean effect only at the first line */
			stage_mean_vert_self(stage_pivot, buffer, src_buffer);
			video_pipeline_run_plain(stage_pivot, stage_end, dst, buffer);
		} else {
			video_pipeline_run(stage_pivot, stage_end, dst, src_buffer);
		}

		/* save always the current buffer (this is the difference from filter and mean) */
		previous_buffer = src_buffer;

		/* first line done */
		++y;
		--run;

		/* do other lines without any effects */
		while (run) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run(stage_pivot, stage_end, dst, src_buffer);
			++y;
			--run;
		}

		PADD(src, stage_vert->sdw);
		--count;
	}

	video_buffer_free(buffer);
}

/***************************************************************************/
/* stretch scale2x */

#ifndef USE_BLIT_TINY
static void video_stage_stretchy_scale2x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* final[2];
	const void* input[3];
	void* partial[3];
	void* partial_copy[3];
	void* tmp;
	unsigned i;

	if (stage_pivot != stage_end) {
		for(i=0;i<2;++i) {
			final[i] = video_buffer_alloc(2 * stage_pivot->sdx * stage_pivot->sbpp);
		}
	} else {
		for(i=0;i<2;++i) {
			final[i] = 0;
		}
	}

	input[0] = src;
	input[1] = src;
	input[2] = src;
	PADD(input[1], stage_vert->sdw);
	PADD(input[2], stage_vert->sdw * 2);

	for(i=0;i<3;++i) {
		partial_copy[i] = partial[i] = video_buffer_alloc(stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	partial[0] = video_pipeline_run_partial_on_buffer(partial[0], stage_begin, stage_pivot, input[0]);
	partial[1] = video_pipeline_run_partial_on_buffer(partial[1], stage_begin, stage_pivot, input[1]);

	if (stage_pivot == stage_end) {
		void* dst[2];

		/* first row */
		for(i=0;i<2;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_scale2x_direct(stage_vert, dst[0], dst[1], partial[0], partial[0], partial[1]);

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			for(i=0;i<2;++i) {
				dst[i] = target->line(target, y) + x_off;
				++y;
			}

			stage_scale2x_direct(stage_vert, dst[0], dst[1], partial[0], partial[1], partial[2]);

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		for(i=0;i<2;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_scale2x_direct(stage_vert, dst[0], dst[1], partial[1-1], partial[2-1], partial[2-1]);
	} else {
		void* dst;

		/* first row */
		stage_scale2x(stage_pivot, final[0], final[1], partial[0], partial[0], partial[1]);

		for(i=0;i<2;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			stage_scale2x(stage_pivot, final[0], final[1], partial[0], partial[1], partial[2]);

			for(i=0;i<2;++i) {
				dst = target->line(target, y) + x_off;
				video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
				++y;
			}

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		stage_scale2x(stage_pivot, final[0], final[1], partial[1-1], partial[2-1], partial[2-1]);

		for(i=0;i<2;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}
	}

	for(i=0;i<3;++i) {
		video_buffer_free(partial_copy[2 - i]);
	}

	if (stage_pivot != stage_end) {
		for(i=0;i<2;++i) {
			video_buffer_free(final[1 - i]);
		}
	}
}
#endif

/***************************************************************************/
/* stretch lq2x */

#ifndef USE_BLIT_TINY
static void video_stage_stretchy_lq2x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* final[2];
	const void* input[3];
	void* partial[3];
	void* partial_copy[3];
	void* tmp;
	unsigned i;

	if (stage_pivot != stage_end) {
		for(i=0;i<2;++i) {
			final[i] = video_buffer_alloc(2 * stage_pivot->sdx * stage_pivot->sbpp);
		}
	} else {
		for(i=0;i<2;++i) {
			final[i] = 0;
		}
	}

	input[0] = src;
	input[1] = src;
	input[2] = src;
	PADD(input[1], stage_vert->sdw);
	PADD(input[2], stage_vert->sdw * 2);

	for(i=0;i<3;++i) {
		partial_copy[i] = partial[i] = video_buffer_alloc(stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	partial[0] = video_pipeline_run_partial_on_buffer(partial[0], stage_begin, stage_pivot, input[0]);
	partial[1] = video_pipeline_run_partial_on_buffer(partial[1], stage_begin, stage_pivot, input[1]);

	if (stage_pivot == stage_end) {
		void* dst[2];

		/* first row */
		for(i=0;i<2;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_lq2x_direct(stage_vert, dst[0], dst[1], partial[0], partial[0], partial[1]);

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			for(i=0;i<2;++i) {
				dst[i] = target->line(target, y) + x_off;
				++y;
			}

			stage_lq2x_direct(stage_vert, dst[0], dst[1], partial[0], partial[1], partial[2]);

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		for(i=0;i<2;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_lq2x_direct(stage_vert, dst[0], dst[1], partial[1-1], partial[2-1], partial[2-1]);
	} else {
		void* dst;

		/* first row */
		stage_lq2x(stage_pivot, final[0], final[1], partial[0], partial[0], partial[1]);

		for(i=0;i<2;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			stage_lq2x(stage_pivot, final[0], final[1], partial[0], partial[1], partial[2]);

			for(i=0;i<2;++i) {
				dst = target->line(target, y) + x_off;
				video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
				++y;
			}

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		stage_lq2x(stage_pivot, final[0], final[1], partial[1-1], partial[2-1], partial[2-1]);

		for(i=0;i<2;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}
	}

	for(i=0;i<3;++i) {
		video_buffer_free(partial_copy[2 - i]);
	}

	if (stage_pivot != stage_end) {
		for(i=0;i<2;++i) {
			video_buffer_free(final[1 - i]);
		}
	}
}
#endif

/***************************************************************************/
/* stretch hq2x */

#ifndef USE_BLIT_TINY
#ifndef USE_BLIT_SMALL
static void video_stage_stretchy_hq2x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* final[2];
	const void* input[3];
	void* partial[3];
	void* partial_copy[3];
	void* tmp;
	unsigned i;

	if (stage_pivot != stage_end) {
		for(i=0;i<2;++i) {
			final[i] = video_buffer_alloc(2 * stage_pivot->sdx * stage_pivot->sbpp);
		}
	} else {
		for(i=0;i<2;++i) {
			final[i] = 0;
		}
	}

	input[0] = src;
	input[1] = src;
	input[2] = src;
	PADD(input[1], stage_vert->sdw);
	PADD(input[2], stage_vert->sdw * 2);

	for(i=0;i<3;++i) {
		partial_copy[i] = partial[i] = video_buffer_alloc(stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	partial[0] = video_pipeline_run_partial_on_buffer(partial[0], stage_begin, stage_pivot, input[0]);
	partial[1] = video_pipeline_run_partial_on_buffer(partial[1], stage_begin, stage_pivot, input[1]);

	if (stage_pivot == stage_end) {
		void* dst[2];

		/* first row */
		for(i=0;i<2;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_hq2x_direct(stage_vert, dst[0], dst[1], partial[0], partial[0], partial[1]);

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			for(i=0;i<2;++i) {
				dst[i] = target->line(target, y) + x_off;
				++y;
			}

			stage_hq2x_direct(stage_vert, dst[0], dst[1], partial[0], partial[1], partial[2]);

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		for(i=0;i<2;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_hq2x_direct(stage_vert, dst[0], dst[1], partial[1-1], partial[2-1], partial[2-1]);
	} else {
		void* dst;

		/* first row */
		stage_hq2x(stage_pivot, final[0], final[1], partial[0], partial[0], partial[1]);

		for(i=0;i<2;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			stage_hq2x(stage_pivot, final[0], final[1], partial[0], partial[1], partial[2]);

			for(i=0;i<2;++i) {
				dst = target->line(target, y) + x_off;
				video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
				++y;
			}

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		stage_hq2x(stage_pivot, final[0], final[1], partial[1-1], partial[2-1], partial[2-1]);

		for(i=0;i<2;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}
	}

	for(i=0;i<3;++i) {
		video_buffer_free(partial_copy[2 - i]);
	}

	if (stage_pivot != stage_end) {
		for(i=0;i<2;++i) {
			video_buffer_free(final[1 - i]);
		}
	}
}
#endif
#endif

/***************************************************************************/
/* stretch scale3x */

#ifndef USE_BLIT_TINY
static void video_stage_stretchy_scale3x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* final[3];
	const void* input[3];
	void* partial[3];
	void* partial_copy[3];
	void* tmp;
	unsigned i;

	if (stage_pivot != stage_end) {
		for(i=0;i<3;++i) {
			final[i] = video_buffer_alloc(3 * stage_pivot->sdx * stage_pivot->sbpp);
		}
	} else {
		for(i=0;i<3;++i) {
			final[i] = 0;
		}
	}

	input[0] = src;
	input[1] = src;
	input[2] = src;
	PADD(input[1], stage_vert->sdw);
	PADD(input[2], stage_vert->sdw * 2);

	for(i=0;i<3;++i) {
		partial_copy[i] = partial[i] = video_buffer_alloc(stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	partial[0] = video_pipeline_run_partial_on_buffer(partial[0], stage_begin, stage_pivot, input[0]);
	partial[1] = video_pipeline_run_partial_on_buffer(partial[1], stage_begin, stage_pivot, input[1]);

	if (stage_pivot == stage_end) {
		void* dst[3];

		/* first row */
		for(i=0;i<3;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_scale3x_direct(stage_vert, dst[0], dst[1], dst[2], partial[0], partial[0], partial[1]);

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			for(i=0;i<3;++i) {
				dst[i] = target->line(target, y) + x_off;
				++y;
			}

			stage_scale3x_direct(stage_vert, dst[0], dst[1], dst[2], partial[0], partial[1], partial[2]);

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		for(i=0;i<3;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_scale3x_direct(stage_vert, dst[0], dst[1], dst[2], partial[1-1], partial[2-1], partial[2-1]);
	} else {
		void* dst;

		/* first row */
		stage_scale3x(stage_pivot, final[0], final[1], final[2], partial[0], partial[0], partial[1]);

		for(i=0;i<3;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			stage_scale3x(stage_pivot, final[0], final[1], final[2], partial[0], partial[1], partial[2]);

			for(i=0;i<3;++i) {
				dst = target->line(target, y) + x_off;
				video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
				++y;
			}

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		stage_scale3x(stage_pivot, final[0], final[1], final[2], partial[1-1], partial[2-1], partial[2-1]);

		for(i=0;i<3;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}
	}

	for(i=0;i<3;++i) {
		video_buffer_free(partial_copy[2 - i]);
	}

	if (stage_pivot != stage_end) {
		for(i=0;i<3;++i) {
			video_buffer_free(final[2 - i]);
		}
	}
}
#endif

/***************************************************************************/
/* stretch lq3x */

#ifndef USE_BLIT_TINY
static void video_stage_stretchy_lq3x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* final[3];
	const void* input[3];
	void* partial[3];
	void* partial_copy[3];
	void* tmp;
	unsigned i;

	if (stage_pivot != stage_end) {
		for(i=0;i<3;++i) {
			final[i] = video_buffer_alloc(3 * stage_pivot->sdx * stage_pivot->sbpp);
		}
	} else {
		for(i=0;i<3;++i) {
			final[i] = 0;
		}
	}

	input[0] = src;
	input[1] = src;
	input[2] = src;
	PADD(input[1], stage_vert->sdw);
	PADD(input[2], stage_vert->sdw * 2);

	for(i=0;i<3;++i) {
		partial_copy[i] = partial[i] = video_buffer_alloc(stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	partial[0] = video_pipeline_run_partial_on_buffer(partial[0], stage_begin, stage_pivot, input[0]);
	partial[1] = video_pipeline_run_partial_on_buffer(partial[1], stage_begin, stage_pivot, input[1]);

	if (stage_pivot == stage_end) {
		void* dst[3];

		/* first row */
		for(i=0;i<3;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_lq3x_direct(stage_vert, dst[0], dst[1], dst[2], partial[0], partial[0], partial[1]);

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			for(i=0;i<3;++i) {
				dst[i] = target->line(target, y) + x_off;
				++y;
			}

			stage_lq3x_direct(stage_vert, dst[0], dst[1], dst[2], partial[0], partial[1], partial[2]);

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		for(i=0;i<3;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_lq3x_direct(stage_vert, dst[0], dst[1], dst[2], partial[1-1], partial[2-1], partial[2-1]);
	} else {
		void* dst;

		/* first row */
		stage_lq3x(stage_pivot, final[0], final[1], final[2], partial[0], partial[0], partial[1]);

		for(i=0;i<3;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			stage_lq3x(stage_pivot, final[0], final[1], final[2], partial[0], partial[1], partial[2]);

			for(i=0;i<3;++i) {
				dst = target->line(target, y) + x_off;
				video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
				++y;
			}

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		stage_lq3x(stage_pivot, final[0], final[1], final[2], partial[1-1], partial[2-1], partial[2-1]);

		for(i=0;i<3;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}
	}

	for(i=0;i<3;++i) {
		video_buffer_free(partial_copy[2 - i]);
	}

	if (stage_pivot != stage_end) {
		for(i=0;i<3;++i) {
			video_buffer_free(final[2 - i]);
		}
	}
}
#endif

/***************************************************************************/
/* stretch hq3x */

#ifndef USE_BLIT_TINY
#ifndef USE_BLIT_SMALL
static void video_stage_stretchy_hq3x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* final[3];
	const void* input[3];
	void* partial[3];
	void* partial_copy[3];
	void* tmp;
	unsigned i;

	if (stage_pivot != stage_end) {
		for(i=0;i<3;++i) {
			final[i] = video_buffer_alloc(3 * stage_pivot->sdx * stage_pivot->sbpp);
		}
	} else {
		for(i=0;i<3;++i) {
			final[i] = 0;
		}
	}

	input[0] = src;
	input[1] = src;
	input[2] = src;
	PADD(input[1], stage_vert->sdw);
	PADD(input[2], stage_vert->sdw * 2);

	for(i=0;i<3;++i) {
		partial_copy[i] = partial[i] = video_buffer_alloc(stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	partial[0] = video_pipeline_run_partial_on_buffer(partial[0], stage_begin, stage_pivot, input[0]);
	partial[1] = video_pipeline_run_partial_on_buffer(partial[1], stage_begin, stage_pivot, input[1]);

	if (stage_pivot == stage_end) {
		void* dst[3];

		/* first row */
		for(i=0;i<3;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_hq3x_direct(stage_vert, dst[0], dst[1], dst[2], partial[0], partial[0], partial[1]);

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			for(i=0;i<3;++i) {
				dst[i] = target->line(target, y) + x_off;
				++y;
			}

			stage_hq3x_direct(stage_vert, dst[0], dst[1], dst[2], partial[0], partial[1], partial[2]);

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		for(i=0;i<3;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_hq3x_direct(stage_vert, dst[0], dst[1], dst[2], partial[1-1], partial[2-1], partial[2-1]);
	} else {
		void* dst;

		/* first row */
		stage_hq3x(stage_pivot, final[0], final[1], final[2], partial[0], partial[0], partial[1]);

		for(i=0;i<3;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			stage_hq3x(stage_pivot, final[0], final[1], final[2], partial[0], partial[1], partial[2]);

			for(i=0;i<3;++i) {
				dst = target->line(target, y) + x_off;
				video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
				++y;
			}

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		stage_hq3x(stage_pivot, final[0], final[1], final[2], partial[1-1], partial[2-1], partial[2-1]);

		for(i=0;i<3;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}
	}

	for(i=0;i<3;++i) {
		video_buffer_free(partial_copy[2 - i]);
	}

	if (stage_pivot != stage_end) {
		for(i=0;i<3;++i) {
			video_buffer_free(final[2 - i]);
		}
	}
}
#endif
#endif

/***************************************************************************/
/* stretch scale4x */

#ifndef USE_BLIT_TINY
static void video_stage_stretchy_scale4x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* final[4];
	const void* input[5];
	void* partial[5];
	void* partial_copy[5];
	void* middle[6];
	void* middle_copy[6];
	void* tmp;
	unsigned i;

	if (stage_pivot != stage_end) {
		for(i=0;i<4;++i) {
			final[i] = video_buffer_alloc(4 * stage_pivot->sdx * stage_pivot->sbpp);
		}
	} else {
		for(i=0;i<4;++i) {
			final[i] = 0;
		}
	}

	input[0] = src;
	input[1] = src;
	input[2] = src;
	input[3] = src;
	input[4] = src;
	PADD(input[1], stage_vert->sdw);
	PADD(input[2], stage_vert->sdw * 2);
	PADD(input[3], stage_vert->sdw * 3);
	PADD(input[4], stage_vert->sdw * 4);

	for(i=0;i<5;++i) {
		partial_copy[i] = partial[i] = video_buffer_alloc(stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	for(i=0;i<4;++i) {
		partial[i] = video_pipeline_run_partial_on_buffer(partial[i], stage_begin, stage_pivot, input[i]);
	}

	for(i=0;i<6;++i) {
		middle_copy[i] = middle[i] = video_buffer_alloc(2 * stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	if (stage_pivot == stage_end) {
		void* dst[4];

		/* first 2 rows */
		for(i=0;i<4;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_scale2x_direct(stage_vert, middle[-2+6], middle[-1+6], partial[0], partial[0], partial[1]);
		stage_scale2x_direct(stage_vert, middle[0], middle[1], partial[0], partial[1], partial[2]);
		stage_scale2x_direct(stage_vert, middle[2], middle[3], partial[1], partial[2], partial[3]);
		stage_scale4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], middle[-2+6], middle[-2+6], middle[-1+6], middle[0]);

		for(i=0;i<4;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_scale4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], middle[-1+6], middle[0], middle[1], middle[2]);

		/* central rows */
		count -= 4;
		while (count) {
			partial[4] = video_pipeline_run_partial_on_buffer(partial[4], stage_begin, stage_pivot, input[4]);

			for(i=0;i<4;++i) {
				dst[i] = target->line(target, y) + x_off;
				++y;
			}

			stage_scale2x_direct(stage_vert, middle[4], middle[5], partial[2], partial[3], partial[4]);
			stage_scale4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], middle[1], middle[2], middle[3], middle[4]);

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = partial[3];
			partial[3] = partial[4];
			partial[4] = tmp;
			tmp = middle[0];
			middle[0] = middle[2];
			middle[2] = middle[4];
			middle[4] = tmp;
			tmp = middle[1];
			middle[1] = middle[3];
			middle[3] = middle[5];
			middle[5] = tmp;

			PADD(input[4], stage_vert->sdw);
			--count;
		}

		/* last 2 rows */
		for(i=0;i<4;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_scale2x_direct(stage_vert, middle[4], middle[5], partial[2], partial[3], partial[3]);
		stage_scale4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], middle[1], middle[2], middle[3], middle[4]);

		for(i=0;i<4;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_scale4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], middle[3], middle[4], middle[5], middle[5]);
	} else {
		void* dst;

		/* first 2 rows */
		stage_scale2x(stage_pivot, middle[-2+6], middle[-1+6], partial[0], partial[0], partial[1]);
		stage_scale2x(stage_pivot, middle[0], middle[1], partial[0], partial[1], partial[2]);
		stage_scale2x(stage_pivot, middle[2], middle[3], partial[1], partial[2], partial[3]);
		stage_scale4x(stage_pivot, final[0], final[1], final[2], final[3], middle[-2+6], middle[-2+6], middle[-1+6], middle[0]);

		for(i=0;i<4;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		stage_scale4x(stage_pivot, final[0], final[1], final[2], final[3], middle[-1+6], middle[0], middle[1], middle[2]);

		for(i=0;i<4;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		/* central rows */
		count -= 4;
		while (count) {
			partial[4] = video_pipeline_run_partial_on_buffer(partial[4], stage_begin, stage_pivot, input[4]);

			stage_scale2x(stage_pivot, middle[4], middle[5], partial[2], partial[3], partial[4]);
			stage_scale4x(stage_pivot, final[0], final[1], final[2], final[3], middle[1], middle[2], middle[3], middle[4]);

			for(i=0;i<4;++i) {
				dst = target->line(target, y) + x_off;
				video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
				++y;
			}

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = partial[3];
			partial[3] = partial[4];
			partial[4] = tmp;
			tmp = middle[0];
			middle[0] = middle[2];
			middle[2] = middle[4];
			middle[4] = tmp;
			tmp = middle[1];
			middle[1] = middle[3];
			middle[3] = middle[5];
			middle[5] = tmp;

			PADD(input[4], stage_vert->sdw);
			--count;
		}

		/* last 2 rows */
		stage_scale2x_direct(stage_vert, middle[4], middle[5], partial[2], partial[3], partial[3]);
		stage_scale4x(stage_pivot, final[0], final[1], final[2], final[3], middle[1], middle[2], middle[3], middle[4]);

		for(i=0;i<4;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		stage_scale4x(stage_pivot, final[0], final[1], final[2], final[3], middle[3], middle[4], middle[5], middle[5]);

		for(i=0;i<4;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}
	}

	for(i=0;i<6;++i) {
		video_buffer_free(middle_copy[5 - i]);
	}

	for(i=0;i<5;++i) {
		video_buffer_free(partial_copy[4 - i]);
	}

	if (stage_pivot != stage_end) {
		for(i=0;i<4;++i) {
			video_buffer_free(final[3 - i]);
		}
	}
}
#endif

/***************************************************************************/
/* stretch lq4x */

#ifndef USE_BLIT_TINY
static void video_stage_stretchy_lq4x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* final[4];
	const void* input[3];
	void* partial[3];
	void* partial_copy[3];
	void* tmp;
	unsigned i;

	if (stage_pivot != stage_end) {
		for(i=0;i<4;++i) {
			final[i] = video_buffer_alloc(4 * stage_pivot->sdx * stage_pivot->sbpp);
		}
	} else {
		for(i=0;i<4;++i) {
			final[i] = 0;
		}
	}

	input[0] = src;
	input[1] = src;
	input[2] = src;
	PADD(input[1], stage_vert->sdw);
	PADD(input[2], stage_vert->sdw * 2);

	for(i=0;i<3;++i) {
		partial_copy[i] = partial[i] = video_buffer_alloc(stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	partial[0] = video_pipeline_run_partial_on_buffer(partial[0], stage_begin, stage_pivot, input[0]);
	partial[1] = video_pipeline_run_partial_on_buffer(partial[1], stage_begin, stage_pivot, input[1]);

	if (stage_pivot == stage_end) {
		void* dst[4];

		/* first row */
		for(i=0;i<4;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_lq4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], partial[0], partial[0], partial[1]);

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			for(i=0;i<4;++i) {
				dst[i] = target->line(target, y) + x_off;
				++y;
			}

			stage_lq4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], partial[0], partial[1], partial[2]);

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		for(i=0;i<4;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_lq4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], partial[1-1], partial[2-1], partial[2-1]);
	} else {
		void* dst;

		/* first row */
		stage_lq4x(stage_pivot, final[0], final[1], final[2], final[3], partial[0], partial[0], partial[1]);

		for(i=0;i<4;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			stage_lq4x(stage_pivot, final[0], final[1], final[2], final[3], partial[0], partial[1], partial[2]);

			for(i=0;i<4;++i) {
				dst = target->line(target, y) + x_off;
				video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
				++y;
			}

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		stage_lq4x(stage_pivot, final[0], final[1], final[2], final[3], partial[1-1], partial[2-1], partial[2-1]);

		for(i=0;i<4;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}
	}

	for(i=0;i<3;++i) {
		video_buffer_free(partial_copy[2 - i]);
	}

	if (stage_pivot != stage_end) {
		for(i=0;i<4;++i) {
			video_buffer_free(final[3 - i]);
		}
	}
}
#endif

/***************************************************************************/
/* stretch hq4x */

#ifndef USE_BLIT_TINY
#ifndef USE_BLIT_SMALL
static void video_stage_stretchy_hq4x(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	const struct video_stage_horz_struct* stage_begin = stage_vert->stage_begin;
	const struct video_stage_horz_struct* stage_end = stage_vert->stage_end;
	const struct video_stage_horz_struct* stage_pivot = stage_vert->stage_pivot;

	void* final[4];
	const void* input[3];
	void* partial[3];
	void* partial_copy[3];
	void* tmp;
	unsigned i;

	if (stage_pivot != stage_end) {
		for(i=0;i<4;++i) {
			final[i] = video_buffer_alloc(4 * stage_pivot->sdx * stage_pivot->sbpp);
		}
	} else {
		for(i=0;i<4;++i) {
			final[i] = 0;
		}
	}

	input[0] = src;
	input[1] = src;
	input[2] = src;
	PADD(input[1], stage_vert->sdw);
	PADD(input[2], stage_vert->sdw * 2);

	for(i=0;i<3;++i) {
		partial_copy[i] = partial[i] = video_buffer_alloc(stage_vert->stage_pivot_sdx * stage_vert->stage_pivot_sbpp);
	}

	partial[0] = video_pipeline_run_partial_on_buffer(partial[0], stage_begin, stage_pivot, input[0]);
	partial[1] = video_pipeline_run_partial_on_buffer(partial[1], stage_begin, stage_pivot, input[1]);

	if (stage_pivot == stage_end) {
		void* dst[4];

		/* first row */
		for(i=0;i<4;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_hq4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], partial[0], partial[0], partial[1]);

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			for(i=0;i<4;++i) {
				dst[i] = target->line(target, y) + x_off;
				++y;
			}

			stage_hq4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], partial[0], partial[1], partial[2]);

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		for(i=0;i<4;++i) {
			dst[i] = target->line(target, y) + x_off;
			++y;
		}

		stage_hq4x_direct(stage_vert, dst[0], dst[1], dst[2], dst[3], partial[1-1], partial[2-1], partial[2-1]);
	} else {
		void* dst;

		/* first row */
		stage_hq4x(stage_pivot, final[0], final[1], final[2], final[3], partial[0], partial[0], partial[1]);

		for(i=0;i<4;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}

		/* central rows */
		count -= 2;
		while (count) {
			partial[2] = video_pipeline_run_partial_on_buffer(partial[2], stage_begin, stage_pivot, input[2]);

			stage_hq4x(stage_pivot, final[0], final[1], final[2], final[3], partial[0], partial[1], partial[2]);

			for(i=0;i<4;++i) {
				dst = target->line(target, y) + x_off;
				video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
				++y;
			}

			tmp = partial[0];
			partial[0] = partial[1];
			partial[1] = partial[2];
			partial[2] = tmp;

			PADD(input[2], stage_vert->sdw);
			--count;
		}

		/* last row */
		stage_hq4x(stage_pivot, final[0], final[1], final[2], final[3], partial[1-1], partial[2-1], partial[2-1]);

		for(i=0;i<4;++i) {
			dst = target->line(target, y) + x_off;
			video_pipeline_run_plain(stage_pivot, stage_end, dst, final[i]);
			++y;
		}
	}

	for(i=0;i<3;++i) {
		video_buffer_free(partial_copy[2 - i]);
	}

	if (stage_pivot != stage_end) {
		for(i=0;i<4;++i) {
			video_buffer_free(final[3 - i]);
		}
	}
}
#endif
#endif

/***************************************************************************/
/* stretchy copy */

static void video_stage_stretchy_11(const struct video_pipeline_target_struct* target, const struct video_stage_vert_struct* stage_vert, unsigned x, unsigned y, const void* src)
{
	unsigned x_off = x * target->bytes_per_pixel;
	unsigned count = stage_vert->sdy;

	while (count) {
		void* dst;

		dst = target->line(target, y) + x_off;
		video_pipeline_run(stage_vert->stage_begin, stage_vert->stage_end, dst, src);
		++y;

		PADD(src, stage_vert->sdw);
		--count;
	}
}

/***************************************************************************/
/* stretchy */

/* set the pivot early in the pipeline */
static void video_stage_pivot_early_set(struct video_stage_vert_struct* stage_vert, unsigned combine_y)
{
	if (combine_is_rgb(combine_y)) {
		stage_vert->stage_pivot = stage_vert->stage_end;
		while (stage_vert->stage_pivot != stage_vert->stage_begin
			&& !pipe_is_conversion(stage_vert->stage_pivot[-1].type)) {
			--stage_vert->stage_pivot;
		}
	} else {
		stage_vert->stage_pivot = stage_vert->stage_begin;
	}
}

/* set the pivot late in the pipeline */
static void video_stage_pivot_late_set(struct video_stage_vert_struct* stage_vert, unsigned combine_y)
{
	if (combine_is_direct(combine_y)) {
		stage_vert->stage_pivot = stage_vert->stage_end;
	} else {
		assert(stage_vert->stage_begin != stage_vert->stage_end);
		stage_vert->stage_pivot = stage_vert->stage_end - 1;
	}
	while (stage_vert->stage_pivot != stage_vert->stage_begin
		&& pipe_is_decoration(stage_vert->stage_pivot[-1].type)) {
		--stage_vert->stage_pivot;
	}
}

/* Inizialize the vertical stage */
static void video_stage_stretchy_set(const struct video_pipeline_target_struct* target, struct video_stage_vert_struct* stage_vert, const struct video_pipeline_struct* pipeline, unsigned ddy, unsigned sdy, int sdw, unsigned combine)
{
	unsigned combine_y = combine & VIDEO_COMBINE_Y_MASK;

	stage_vert->sdy = sdy;
	stage_vert->sdw = sdw;
	stage_vert->ddy = ddy;

	stage_vert->stage_begin = video_pipeline_begin(pipeline);
	stage_vert->stage_end = video_pipeline_end(pipeline);

#ifndef USE_BLIT_TINY
	if (ddy == 2*sdy && combine_y == VIDEO_COMBINE_Y_SCALE2X) {
		/* scale2x */
		slice_set(&stage_vert->slice, sdy, ddy);

		video_stage_pivot_late_set(stage_vert, combine_y);
		stage_vert->put = video_stage_stretchy_scale2x;
		stage_vert->type = pipe_y_scale2x;
	} else if (ddy == 2*sdy && combine_y == VIDEO_COMBINE_Y_LQ2X) {
		/* lq2x */
		slice_set(&stage_vert->slice, sdy, ddy);

		video_stage_pivot_late_set(stage_vert, combine_y);
		stage_vert->put = video_stage_stretchy_lq2x;
		stage_vert->type = pipe_y_lq2x;
#ifndef USE_BLIT_SMALL
	} else if (ddy == 2*sdy && combine_y == VIDEO_COMBINE_Y_HQ2X) {
		/* hq2x */
		slice_set(&stage_vert->slice, sdy, ddy);

		video_stage_pivot_late_set(stage_vert, combine_y);
		stage_vert->put = video_stage_stretchy_hq2x;
		stage_vert->type = pipe_y_hq2x;
#endif
	} else if (ddy == 3*sdy && combine_y == VIDEO_COMBINE_Y_SCALE3X) {
		/* scale3x */
		slice_set(&stage_vert->slice, sdy, ddy);

		video_stage_pivot_late_set(stage_vert, combine_y);
		stage_vert->put = video_stage_stretchy_scale3x;
		stage_vert->type = pipe_y_scale3x;
	} else if (ddy == 3*sdy && combine_y == VIDEO_COMBINE_Y_LQ3X) {
		/* lq3x */
		slice_set(&stage_vert->slice, sdy, ddy);

		video_stage_pivot_late_set(stage_vert, combine_y);
		stage_vert->put = video_stage_stretchy_lq3x;
		stage_vert->type = pipe_y_lq3x;
#ifndef USE_BLIT_SMALL
	} else if (ddy == 3*sdy && combine_y == VIDEO_COMBINE_Y_HQ3X) {
		/* hq3x */
		slice_set(&stage_vert->slice, sdy, ddy);

		video_stage_pivot_late_set(stage_vert, combine_y);
		stage_vert->put = video_stage_stretchy_hq3x;
		stage_vert->type = pipe_y_hq3x;
#endif
	} else if (ddy == 4*sdy && combine_y == VIDEO_COMBINE_Y_SCALE4X) {
		/* scale4x */
		slice_set(&stage_vert->slice, sdy, ddy);

		video_stage_pivot_late_set(stage_vert, combine_y);
		stage_vert->put = video_stage_stretchy_scale4x;
		stage_vert->type = pipe_y_scale4x;
	} else if (ddy == 4*sdy && combine_y == VIDEO_COMBINE_Y_LQ4X) {
		/* lq4x */
		slice_set(&stage_vert->slice, sdy, ddy);

		video_stage_pivot_late_set(stage_vert, combine_y);
		stage_vert->put = video_stage_stretchy_lq4x;
		stage_vert->type = pipe_y_lq4x;
#ifndef USE_BLIT_SMALL
	} else if (ddy == 4*sdy && combine_y == VIDEO_COMBINE_Y_HQ4X) {
		/* hq4x */
		slice_set(&stage_vert->slice, sdy, ddy);

		video_stage_pivot_late_set(stage_vert, combine_y);
		stage_vert->put = video_stage_stretchy_hq4x;
		stage_vert->type = pipe_y_hq4x;
#endif
	} else
#endif
	if (sdy < ddy) { /* y expansion */
		slice_set(&stage_vert->slice, sdy, ddy);

		switch (combine_y) {
			case VIDEO_COMBINE_Y_MEAN :
				video_stage_pivot_late_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_mean_1x;
				stage_vert->type = pipe_y_expansion_mean;
				break;
			case VIDEO_COMBINE_Y_FILTER :
				video_stage_pivot_late_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_filter_1x;
				stage_vert->type = pipe_y_expansion_filter;
				break;
			default:
				video_stage_pivot_late_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_1x;
				stage_vert->type = pipe_y_expansion_copy;
				break;
		}
	} else if (sdy == ddy) { /* y copy */
		slice_set(&stage_vert->slice, sdy, ddy);

		switch (combine_y) {
			case VIDEO_COMBINE_Y_MEAN :
				video_stage_pivot_early_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_mean_1x;
				stage_vert->type = pipe_y_mean;
				break;
			case VIDEO_COMBINE_Y_FILTER :
				video_stage_pivot_early_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_filter_1x;
				stage_vert->type = pipe_y_filter;
				break;
			default:
				video_stage_pivot_early_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_11;
				stage_vert->type = pipe_y_copy;
				break;
		}
	} else { /* y reduction */
		slice_set(&stage_vert->slice, sdy, ddy);

		switch (combine_y) {
			case VIDEO_COMBINE_Y_MAX :
				video_stage_pivot_early_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_max_x1;
				stage_vert->type = pipe_y_reduction_max;
				break;
			case VIDEO_COMBINE_Y_MEAN :
				video_stage_pivot_early_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_mean_x1;
				stage_vert->type = pipe_y_reduction_mean;
				break;
			case VIDEO_COMBINE_Y_FILTER :
				video_stage_pivot_early_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_filter_x1;
				stage_vert->type = pipe_y_reduction_filter;
				break;
			default:
				video_stage_pivot_early_set(stage_vert, combine_y);
				stage_vert->put = video_stage_stretchy_x1;
				stage_vert->type = pipe_y_reduction_copy;
				break;
		}
	}

	if (color_def_type_get(target->color_def) == adv_color_type_rgb) {
		if (combine_y == VIDEO_COMBINE_Y_MEAN
			|| combine_y == VIDEO_COMBINE_Y_FILTER
			|| (combine & VIDEO_COMBINE_X_FILTER)!=0
			|| (combine & VIDEO_COMBINE_X_MEAN)!=0
			|| (combine & VIDEO_COMBINE_INTERLACE_FILTER)!=0
		)
			internal_mean_set(target);

		if (combine_y == VIDEO_COMBINE_Y_MAX
			|| (combine & VIDEO_COMBINE_X_MAX)!=0)
			internal_max_rgb_set(target);

#ifndef USE_BLIT_TINY
		if (combine_y == VIDEO_COMBINE_Y_LQ2X || combine_y == VIDEO_COMBINE_Y_LQ3X || combine_y == VIDEO_COMBINE_Y_LQ4X
#ifndef USE_BLIT_SMALL
			|| combine_y == VIDEO_COMBINE_Y_HQ2X || combine_y == VIDEO_COMBINE_Y_HQ3X || combine_y == VIDEO_COMBINE_Y_HQ4X
#endif
		)
			interp_set(target);

		if ((combine & VIDEO_COMBINE_X_RGB_TRIAD3PIX)!=0 || (combine & VIDEO_COMBINE_X_RGB_TRIADSTRONG3PIX)!=0)
			internal_rgb_triad3pix_set(target);

		if ((combine & VIDEO_COMBINE_X_RGB_TRIAD6PIX)!=0 || (combine & VIDEO_COMBINE_X_RGB_TRIADSTRONG6PIX)!=0)
			internal_rgb_triad6pix_set(target);

		if ((combine & VIDEO_COMBINE_X_RGB_TRIAD16PIX)!=0 || (combine & VIDEO_COMBINE_X_RGB_TRIADSTRONG16PIX)!=0)
			internal_rgb_triad16pix_set(target);

		if ((combine & VIDEO_COMBINE_X_RGB_SCANDOUBLEHORZ)!=0)
			internal_rgb_scandouble_set(target);

		if ((combine & VIDEO_COMBINE_X_RGB_SCANTRIPLEHORZ)!=0)
			internal_rgb_scantriple_set(target);

		if ((combine & VIDEO_COMBINE_X_RGB_SCANDOUBLEVERT)!=0)
			internal_rgb_scandoublevert_set(target);

		if ((combine & VIDEO_COMBINE_X_RGB_SCANTRIPLEVERT)!=0)
			internal_rgb_scantriplevert_set(target);
#endif
	}
}

/* Inizialize the vertical stage for the no transformation special case */
static void video_stage_stretchy_11_set(struct video_stage_vert_struct* stage_vert, const struct video_pipeline_struct* pipeline, unsigned sdy, int sdw)
{
	stage_vert->sdy = sdy;
	stage_vert->sdw = sdw;
	stage_vert->ddy = sdy;

	slice_set(&stage_vert->slice, sdy, sdy);

	stage_vert->stage_begin = video_pipeline_begin(pipeline);
	stage_vert->stage_end = video_pipeline_end(pipeline);
	stage_vert->stage_pivot = stage_vert->stage_begin;

	stage_vert->put = video_stage_stretchy_11;
	stage_vert->type = pipe_y_copy;
}

/***************************************************************************/
/* stretch */

static void video_pipeline_make(struct video_pipeline_struct* pipeline, unsigned dst_dx, unsigned src_dx, int src_dp, unsigned combine)
{
	unsigned bytes_per_pixel = video_bytes_per_pixel();
	struct video_stage_horz_struct* end;
	unsigned combine_y = combine & VIDEO_COMBINE_Y_MASK;

	/* in x reduction the filter is applied before the resize */
	if ((combine & VIDEO_COMBINE_X_FILTER) != 0
		&& src_dx > dst_dx) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_filter8_set( video_pipeline_insert(pipeline), src_dx, src_dp ); break;
			case 2 : video_stage_filter16_set( video_pipeline_insert(pipeline), src_dx, src_dp ); break;
			case 4 : video_stage_filter32_set( video_pipeline_insert(pipeline), src_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	/* do the x stretch */
#ifndef USE_BLIT_TINY
	if (dst_dx == 2*src_dx && combine_y == VIDEO_COMBINE_Y_SCALE2X) {
		/* the stretch is done by the y stage */
		src_dp = bytes_per_pixel;
	} else if (dst_dx == 2*src_dx && combine_y == VIDEO_COMBINE_Y_LQ2X) {
		/* the stretch is done by the y stage */
		src_dp = bytes_per_pixel;
#ifndef USE_BLIT_SMALL
	} else if (dst_dx == 2*src_dx && combine_y == VIDEO_COMBINE_Y_HQ2X) {
		/* the stretch is done by the y stage */
		src_dp = bytes_per_pixel;
#endif
	} else if (dst_dx == 3*src_dx && combine_y == VIDEO_COMBINE_Y_SCALE3X) {
		/* the stretch is done by the y stage */
		src_dp = bytes_per_pixel;
	} else if (dst_dx == 3*src_dx && combine_y == VIDEO_COMBINE_Y_LQ3X) {
		/* the stretch is done by the y stage */
		src_dp = bytes_per_pixel;
#ifndef USE_BLIT_SMALL
	} else if (dst_dx == 3*src_dx && combine_y == VIDEO_COMBINE_Y_HQ3X) {
		/* the stretch is done by the y stage */
		src_dp = bytes_per_pixel;
#endif
	} else if (dst_dx == 4*src_dx && combine_y == VIDEO_COMBINE_Y_SCALE4X) {
		/* the stretch is done by the y stage */
		src_dp = bytes_per_pixel;
	} else if (dst_dx == 4*src_dx && combine_y == VIDEO_COMBINE_Y_LQ4X) {
		/* the stretch is done by the y stage */
		src_dp = bytes_per_pixel;
#ifndef USE_BLIT_SMALL
	} else if (dst_dx == 4*src_dx && combine_y == VIDEO_COMBINE_Y_HQ4X) {
		/* the stretch is done by the y stage */
		src_dp = bytes_per_pixel;
#endif
	} else
#endif
	{
#ifndef USE_BLIT_TINY
		/* disable the y effect if size doesn't match */
		switch (combine_y) {
		case VIDEO_COMBINE_Y_SCALE2X :
		case VIDEO_COMBINE_Y_SCALE3X :
		case VIDEO_COMBINE_Y_SCALE4X :
		case VIDEO_COMBINE_Y_LQ2X :
		case VIDEO_COMBINE_Y_LQ3X :
		case VIDEO_COMBINE_Y_LQ4X :
#ifndef USE_BLIT_SMALL
		case VIDEO_COMBINE_Y_HQ2X :
		case VIDEO_COMBINE_Y_HQ3X :
		case VIDEO_COMBINE_Y_HQ4X :
#endif
			combine_y = VIDEO_COMBINE_Y_NONE;
			break;
		}
#endif
		if (dst_dx != src_dx) {
			if ((combine & VIDEO_COMBINE_X_MEAN) != 0) {
				switch (bytes_per_pixel) {
					case 1 : video_stage_meanx8_set( video_pipeline_insert(pipeline), dst_dx, src_dx, src_dp ); break;
					case 2 : video_stage_meanx16_set( video_pipeline_insert(pipeline), dst_dx, src_dx, src_dp ); break;
					case 4 : video_stage_meanx32_set( video_pipeline_insert(pipeline), dst_dx, src_dx, src_dp ); break;
				}
				src_dp = bytes_per_pixel;
			} else if ((combine & VIDEO_COMBINE_X_MAX) != 0) {
				switch (bytes_per_pixel) {
					case 1 : video_stage_maxx8_set( video_pipeline_insert(pipeline), dst_dx, src_dx, src_dp ); break;
					case 2 : video_stage_maxx16_set( video_pipeline_insert(pipeline), dst_dx, src_dx, src_dp ); break;
					case 4 : video_stage_maxx32_set( video_pipeline_insert(pipeline), dst_dx, src_dx, src_dp ); break;
				}
				src_dp = bytes_per_pixel;
			} else {
				switch (bytes_per_pixel) {
					case 1 : video_stage_stretchx8_set( video_pipeline_insert(pipeline), dst_dx, src_dx, src_dp ); break;
					case 2 : video_stage_stretchx16_set( video_pipeline_insert(pipeline), dst_dx, src_dx, src_dp ); break;
					case 4 : video_stage_stretchx32_set( video_pipeline_insert(pipeline), dst_dx, src_dx, src_dp ); break;
				}
				src_dp = bytes_per_pixel;
			}
		}
	}

	/* in x expansion the filter is applied after the resize */
	if ((combine & VIDEO_COMBINE_X_FILTER)!=0
		&& src_dx <= dst_dx) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_filter8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_filter16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_filter32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

#ifndef USE_BLIT_TINY
	if ((combine & VIDEO_COMBINE_X_RGB_TRIAD16PIX)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_triad16pix8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_triad16pix16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_triad16pix32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_X_RGB_TRIADSTRONG16PIX)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_triadstrong16pix8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_triadstrong16pix16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_triadstrong16pix32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_X_RGB_TRIAD6PIX)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_triad6pix8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_triad6pix16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_triad6pix32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_X_RGB_TRIADSTRONG6PIX)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_triadstrong6pix8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_triadstrong6pix16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_triadstrong6pix32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_X_RGB_TRIAD3PIX)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_triad3pix8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_triad3pix16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_triad3pix32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_X_RGB_TRIADSTRONG3PIX)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_triadstrong3pix8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_triadstrong3pix16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_triadstrong3pix32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_X_RGB_SCANDOUBLEHORZ)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_scandouble8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_scandouble16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_scandouble32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_X_RGB_SCANTRIPLEHORZ)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_scantriple8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_scantriple16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_scantriple32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_X_RGB_SCANDOUBLEVERT)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_scandoublevert8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_scandoublevert16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_scandoublevert32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_X_RGB_SCANTRIPLEVERT)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_rgb_scantriplevert8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_rgb_scantriplevert16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_rgb_scantriplevert32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}
#endif

	if ((combine & VIDEO_COMBINE_INTERLACE_FILTER)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_interlacefilter8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_interlacefilter16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_interlacefilter32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_SWAP_EVEN)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_swapeven8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_swapeven16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_swapeven32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	if ((combine & VIDEO_COMBINE_SWAP_ODD)!=0) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_swapodd8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_swapodd16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_swapodd32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}

	/* add a dummy stage if it's required of it improves the speed */
	if (
		/* if the last stage is required */
		(!combine_is_direct(combine_y) && video_pipeline_size(pipeline) == 0)
		/* if the last stage exists and it's a conversion and a conversion is not allowed as a last stage */
		|| (!combine_is_direct(combine_y) && combine_is_rgb(combine_y) && video_pipeline_size(pipeline) != 0 && pipe_is_conversion(video_pipeline_end(pipeline)[-1].type))
		/* if the last stage is a slow memory write stage */
		|| (video_pipeline_size(pipeline) != 0 && !pipe_is_fastwrite(&video_pipeline_end(pipeline)[-1]))
		/* if the last stage is a slow memory write vertical stage */
		|| (video_pipeline_size(pipeline) == 0 && !combine_is_fastwrite(combine_y, bytes_per_pixel))
	) {
		switch (bytes_per_pixel) {
			case 1 : video_stage_copy8_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 2 : video_stage_copy16_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
			case 4 : video_stage_copy32_set( video_pipeline_insert(pipeline), dst_dx, src_dp ); break;
		}
		src_dp = bytes_per_pixel;
	}
}

void video_pipeline_direct(struct video_pipeline_struct* pipeline, unsigned dst_dx, unsigned dst_dy, unsigned src_dx, unsigned src_dy, int src_dw, int src_dp, adv_color_def src_color_def, unsigned combine)
{
	adv_color_def dst_color_def = pipeline->target.color_def;
	unsigned bytes_per_pixel = pipeline->target.bytes_per_pixel;

	/* conversion */
	if (src_color_def != dst_color_def) {
		/* only conversion from rgb are supported */
		assert(color_def_type_get(src_color_def) == adv_color_type_rgb);

#if 1
		/* optimized conversion */

		/* preconversion */
		if (src_color_def == color_def_make_from_rgb_sizelenpos(4, 8, 0, 8, 8, 8, 16)) {
			video_stage_rgba8888tobgra8888_set( video_pipeline_insert(pipeline), src_dx, src_dp );
			src_color_def = color_def_make_from_rgb_sizelenpos(4, 8, 16, 8, 8, 8, 0);
			src_dp = 4;
		} else if (src_color_def == color_def_make_from_rgb_sizelenpos(3, 8, 0, 8, 8, 8, 16)) {
			video_stage_rgb888tobgra8888_set( video_pipeline_insert(pipeline), src_dx, src_dp );
			src_color_def = color_def_make_from_rgb_sizelenpos(4, 8, 16, 8, 8, 8, 0);
			src_dp = 4;
		} else if (src_color_def == color_def_make_from_rgb_sizelenpos(3, 8, 16, 8, 8, 8, 0)) {
			video_stage_bgr888tobgra8888_set( video_pipeline_insert(pipeline), src_dx, src_dp );
			src_color_def = color_def_make_from_rgb_sizelenpos(4, 8, 16, 8, 8, 8, 0);
			src_dp = 4;
		}

		/* conversion */
		if (src_color_def == color_def_make_from_rgb_sizelenpos(4, 8, 16, 8, 8, 8, 0)) {
			if (dst_color_def == color_def_make_from_rgb_sizelenpos(1, 3, 5, 3, 2, 2, 0)) {
				/* rotation */
				if (src_dp != 4) {
					video_stage_rot32_set( video_pipeline_insert(pipeline), src_dx, src_dp );
					src_dp = 4;
				}
				video_stage_bgra8888tobgr332_set( video_pipeline_insert(pipeline), src_dx, src_dp );
				src_dp = 1;
			} else if (dst_color_def == color_def_make_from_rgb_sizelenpos(2, 5, 10, 5, 5, 5, 0)) {
				/* rotation */
				if (src_dp != 4) {
					video_stage_rot32_set( video_pipeline_insert(pipeline), src_dx, src_dp );
					src_dp = 4;
				}
				video_stage_bgra8888tobgra5551_set( video_pipeline_insert(pipeline), src_dx, src_dp );
				src_dp = 2;
			} else if (dst_color_def == color_def_make_from_rgb_sizelenpos(2, 5, 11, 6, 5, 5, 0)) {
				/* rotation */
				if (src_dp != 4) {
					video_stage_rot32_set( video_pipeline_insert(pipeline), src_dx, src_dp );
					src_dp = 4;
				}
				video_stage_bgra8888tobgr565_set( video_pipeline_insert(pipeline), src_dx, src_dp );
				src_dp = 2;
			} else if (dst_color_def == color_def_make(adv_color_type_yuy2)) {
				video_stage_bgra8888toyuy2_set( video_pipeline_insert(pipeline), src_dx, src_dp );
				src_dp = 4;
			} else {
				video_stage_rgbtorgb_set( video_pipeline_insert(pipeline), src_dx, src_dp, src_color_def, dst_color_def );
				src_dp = color_def_bytes_per_pixel_get(dst_color_def);
			}
		} else if (src_color_def == color_def_make_from_rgb_sizelenpos(2, 5, 10, 5, 5, 5, 0)) {
			if (dst_color_def == color_def_make_from_rgb_sizelenpos(1, 3, 5, 3, 2, 2, 0)) {
				/* rotation */
				if (src_dp != 2) {
					video_stage_rot16_set( video_pipeline_insert(pipeline), src_dx, src_dp );
					src_dp = 2;
				}
				video_stage_bgra5551tobgr332_set( video_pipeline_insert(pipeline), src_dx, src_dp );
				src_dp = 1;
			} else if (dst_color_def == color_def_make_from_rgb_sizelenpos(2, 5, 11, 6, 5, 5, 0)) {
				/* rotation */
				if (src_dp != 2) {
					video_stage_rot16_set( video_pipeline_insert(pipeline), src_dx, src_dp );
					src_dp = 2;
				}
				video_stage_bgra5551tobgr565_set( video_pipeline_insert(pipeline), src_dx, src_dp );
				src_dp = 2;
			} else if (dst_color_def == color_def_make_from_rgb_sizelenpos(4, 8, 16, 8, 8, 8, 0)) {
				/* rotation */
				if (src_dp != 2) {
					video_stage_rot16_set( video_pipeline_insert(pipeline), src_dx, src_dp );
					src_dp = 2;
				}
				video_stage_bgra5551tobgra8888_set( video_pipeline_insert(pipeline), src_dx, src_dp );
				src_dp = 4;
			} else if (dst_color_def == color_def_make(adv_color_type_yuy2)) {
				video_stage_bgra5551toyuy2_set( video_pipeline_insert(pipeline), src_dx, src_dp );
				src_dp = 4;
			} else {
				video_stage_rgbtorgb_set( video_pipeline_insert(pipeline), src_dx, src_dp, src_color_def, dst_color_def );
				src_dp = color_def_bytes_per_pixel_get(dst_color_def);
			}
		} else {
			if (dst_color_def == color_def_make(adv_color_type_yuy2)) {
				video_stage_rgbtoyuy2_set(video_pipeline_insert(pipeline), src_dx, src_dp, src_color_def);
				src_dp = 4;
			} else {
				video_stage_rgbtorgb_set( video_pipeline_insert(pipeline), src_dx, src_dp, src_color_def, dst_color_def );
				src_dp = color_def_bytes_per_pixel_get(dst_color_def);
			}
		}
#else
		/* generic conversion */
		if (dst_color_def == color_def_make(adv_color_type_yuy2)) {
			video_stage_rgbtoyuy2_set(video_pipeline_insert(pipeline), src_dx, src_dp, src_color_def);
			src_dp = 4;
		} else {
			video_stage_rgbtorgb_set(video_pipeline_insert(pipeline), src_dx, src_dp, src_color_def, dst_color_def);
			src_dp = color_def_bytes_per_pixel_get(dst_color_def);
		}
#endif
	} else {
		/* rotation */
		if (src_dp != bytes_per_pixel) {
			switch (bytes_per_pixel) {
				case 1 : video_stage_rot8_set( video_pipeline_insert(pipeline), src_dx, src_dp ); break;
				case 2 : video_stage_rot16_set( video_pipeline_insert(pipeline), src_dx, src_dp ); break;
				case 4 : video_stage_rot32_set( video_pipeline_insert(pipeline), src_dx, src_dp ); break;
			}
			src_dp = bytes_per_pixel;
		}
	}

	video_pipeline_make(pipeline, dst_dx, src_dx, src_dp, combine);

	video_stage_stretchy_set(&pipeline->target, video_pipeline_vert_mutable(pipeline), pipeline, dst_dy, src_dy, src_dw, combine);

	video_pipeline_realize(pipeline, src_dx, src_dp, bytes_per_pixel);
}

void video_pipeline_palette16hw(struct video_pipeline_struct* pipeline, unsigned dst_dx, unsigned dst_dy, unsigned src_dx, unsigned src_dy, int src_dw, int src_dp, unsigned combine)
{
	unsigned bytes_per_pixel = pipeline->target.bytes_per_pixel;

	/* conversion and rotation */
	switch (bytes_per_pixel) {
		case 1 :
			video_stage_imm16to8_set(video_pipeline_insert(pipeline), src_dx, src_dp);
			break;
		case 2 :
			if (src_dp != bytes_per_pixel) {
				video_stage_rot16_set( video_pipeline_insert(pipeline), src_dx, src_dp );
			}
			break;
		case 4 :
			video_stage_imm16to32_set(video_pipeline_insert(pipeline), src_dx, src_dp);
			break;
	}
	src_dp = bytes_per_pixel;

	video_pipeline_make(pipeline, dst_dx, src_dx, src_dp, combine);

	video_stage_stretchy_set(&pipeline->target, video_pipeline_vert_mutable(pipeline), pipeline, dst_dy, src_dy, src_dw, combine);

	video_pipeline_realize(pipeline, src_dx, src_dp, bytes_per_pixel);
}

void video_pipeline_palette8(struct video_pipeline_struct* pipeline, unsigned dst_dx, unsigned dst_dy, unsigned src_dx, unsigned src_dy, int src_dw, int src_dp, const uint8* palette8, const uint16* palette16, const uint32* palette32, unsigned combine)
{
	unsigned bytes_per_pixel = pipeline->target.bytes_per_pixel;

	/* conversion and rotation */
	switch (bytes_per_pixel) {
		case 1 :
			video_stage_palette8to8_set( video_pipeline_insert(pipeline), src_dx, src_dp, palette8 );
			break;
		case 2 :
			video_stage_palette8to16_set( video_pipeline_insert(pipeline), src_dx, src_dp, palette16 );
			break;
		case 4 :
			video_stage_palette8to32_set( video_pipeline_insert(pipeline), src_dx, src_dp, palette32 );
			break;
	}
	src_dp = bytes_per_pixel;

	video_pipeline_make(pipeline, dst_dx, src_dx, src_dp, combine);

	video_stage_stretchy_set(&pipeline->target, video_pipeline_vert_mutable(pipeline), pipeline, dst_dy, src_dy, src_dw, combine);

	video_pipeline_realize(pipeline, src_dx, src_dp, bytes_per_pixel);
}

void video_pipeline_palette16(struct video_pipeline_struct* pipeline, unsigned dst_dx, unsigned dst_dy, unsigned src_dx, unsigned src_dy, int src_dw, int src_dp, const uint8* palette8, const uint16* palette16, const uint32* palette32, unsigned combine)
{
	unsigned bytes_per_pixel = pipeline->target.bytes_per_pixel;

	/* conversion and rotation */
	switch (bytes_per_pixel) {
		case 1 :
			video_stage_palette16to8_set( video_pipeline_insert(pipeline), src_dx, src_dp, palette8 );
			break;
		case 2 :
			video_stage_palette16to16_set( video_pipeline_insert(pipeline), src_dx, src_dp, palette16 );
			break;
		case 4 :
			video_stage_palette16to32_set( video_pipeline_insert(pipeline), src_dx, src_dp, palette32 );
			break;
	}
	src_dp = bytes_per_pixel;

	video_pipeline_make(pipeline, dst_dx, src_dx, src_dp, combine);

	video_stage_stretchy_set(&pipeline->target, video_pipeline_vert_mutable(pipeline), pipeline, dst_dy, src_dy, src_dw, combine);

	video_pipeline_realize(pipeline, src_dx, src_dp, bytes_per_pixel);
}

void video_pipeline_blit(const struct video_pipeline_struct* pipeline, unsigned dst_x, unsigned dst_y, const void* src)
{
	video_pipeline_vert_run(pipeline, dst_x, dst_y, src);
}

