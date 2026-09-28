/*
 * Phoenix-RTOS
 *
 * Raspberry Pi 4 (BCM2711) V3D 4.2 asynchronous render server - smoke-test job generators
 *
 * See v3da_clgen.h. Packet sequences are transcribed from Mesa's gallium v3d
 * driver for exactly one configuration (1 raster RGBA8 RT, no Z/S, no MSAA, no
 * double-buffer, one layer, zero draws, V3D_VERSION == 42); each step names the
 * Mesa function it mirrors.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "v3da_clgen.h"


/* ------------------------------------------------------------------------ */
/* Packet packers - verbatim from Mesa                                       */
/* ------------------------------------------------------------------------ */

/*
 * The enums, structs, *_header / *_opcode / *_length defines and *_pack()
 * functions between the BEGIN/END markers below are copied VERBATIM (unmodified,
 * in header order; the *_unpack() variants are omitted) from Mesa's generated
 * src/broadcom/cle/v3d_packet_v42_pack.h ("Generated code, see vc4_packet.xml,
 * v3d_packet.xml and gen_pack_header.py"), produced by Mesa's MIT-licensed
 * generator src/broadcom/cle/gen_pack_header.py from src/broadcom/cle/
 * v3d_packet.xml (no per-file notice; part of Mesa's MIT-licensed broadcom tree).
 * Notice of the generator (and of v3d_packet_helpers.h, same terms):
 *
 *   Copyright (C) 2016 Intel Corporation
 *   Copyright (C) 2016 Broadcom
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the "Software"),
 *   to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *   and/or sell copies of the Software, and to permit persons to whom the
 *   Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice (including the next
 *   paragraph) shall be included in all copies or substantial portions of the
 *   Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 *   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *   IN THE SOFTWARE.
 *
 * The helpers they use are replaced by the minimal self-contained definitions
 * below (originals: src/util/bitpack_helpers.h, src/broadcom/cle/
 * v3d_packet_helpers.h, and the gallium driver's v3d_cl.h address glue).
 * Addresses here are plain 32-bit GPU virtual addresses: no BO, no relocation.
 */

/* util_bitpack_uint() (bitpack_helpers.h) minus its valgrind hook; the range
 * assert is kept (the generators only pass validated values). */
static inline uint64_t util_bitpack_uint(uint64_t v, uint32_t start, uint32_t end)
{
	const uint32_t bits = end - start + 1u;

	assert((bits >= 64u) || (v <= (UINT64_MAX >> (64u - bits))));
	(void)bits;

	return v << start;
}

#define __gen_user_data          void
#define __gen_address_type       uint32_t
#define __gen_address_offset(a)  (*(a))
#define __gen_emit_reloc(d, a)   ((void)0)

/* Address-free packets never touch their 'data' argument; the copies stay
 * byte-verbatim, so silence that one warning around them. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

/* ---- BEGIN verbatim copy from Mesa v3d_packet_v42_pack.h ---- */

enum V3D42_Memory_Format {
        V3D_MEMORY_FORMAT_RASTER             =      0,
        V3D_MEMORY_FORMAT_LINEARTILE         =      1,
        V3D_MEMORY_FORMAT_UB_LINEAR_1_UIF_BLOCK_WIDE =      2,
        V3D_MEMORY_FORMAT_UB_LINEAR_2_UIF_BLOCKS_WIDE =      3,
        V3D_MEMORY_FORMAT_UIF_NO_XOR         =      4,
        V3D_MEMORY_FORMAT_UIF_XOR            =      5,
};

enum V3D42_Decimate_Mode {
        V3D_DECIMATE_MODE_SAMPLE_0           =      0,
        V3D_DECIMATE_MODE_4X                 =      1,
        V3D_DECIMATE_MODE_ALL_SAMPLES        =      3,
};

enum V3D42_Internal_Type {
        V3D_INTERNAL_TYPE_8I                 =      0,
        V3D_INTERNAL_TYPE_8UI                =      1,
        V3D_INTERNAL_TYPE_8                  =      2,
        V3D_INTERNAL_TYPE_16I                =      4,
        V3D_INTERNAL_TYPE_16UI               =      5,
        V3D_INTERNAL_TYPE_16F                =      6,
        V3D_INTERNAL_TYPE_32I                =      8,
        V3D_INTERNAL_TYPE_32UI               =      9,
        V3D_INTERNAL_TYPE_32F                =     10,
};

enum V3D42_Internal_BPP {
        V3D_INTERNAL_BPP_32                  =      0,
        V3D_INTERNAL_BPP_64                  =      1,
        V3D_INTERNAL_BPP_128                 =      2,
};

enum V3D42_Internal_Depth_Type {
        V3D_INTERNAL_TYPE_DEPTH_32F          =      0,
        V3D_INTERNAL_TYPE_DEPTH_24           =      1,
        V3D_INTERNAL_TYPE_DEPTH_16           =      2,
};

enum V3D42_Render_Target_Clamp {
        V3D_RENDER_TARGET_CLAMP_NONE         =      0,
        V3D_RENDER_TARGET_CLAMP_NORM         =      1,
        V3D_RENDER_TARGET_CLAMP_POS          =      2,
        V3D_RENDER_TARGET_CLAMP_INT          =      3,
};

enum V3D42_Output_Image_Format {
        V3D_OUTPUT_IMAGE_FORMAT_SRGB8_ALPHA8 =      0,
        V3D_OUTPUT_IMAGE_FORMAT_SRGB         =      1,
        V3D_OUTPUT_IMAGE_FORMAT_RGB10_A2UI   =      2,
        V3D_OUTPUT_IMAGE_FORMAT_RGB10_A2     =      3,
        V3D_OUTPUT_IMAGE_FORMAT_ABGR1555     =      4,
        V3D_OUTPUT_IMAGE_FORMAT_ALPHA_MASKED_ABGR1555 =      5,
        V3D_OUTPUT_IMAGE_FORMAT_ABGR4444     =      6,
        V3D_OUTPUT_IMAGE_FORMAT_BGR565       =      7,
        V3D_OUTPUT_IMAGE_FORMAT_R11F_G11F_B10F =      8,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA32F      =      9,
        V3D_OUTPUT_IMAGE_FORMAT_RG32F        =     10,
        V3D_OUTPUT_IMAGE_FORMAT_R32F         =     11,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA32I      =     12,
        V3D_OUTPUT_IMAGE_FORMAT_RG32I        =     13,
        V3D_OUTPUT_IMAGE_FORMAT_R32I         =     14,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA32UI     =     15,
        V3D_OUTPUT_IMAGE_FORMAT_RG32UI       =     16,
        V3D_OUTPUT_IMAGE_FORMAT_R32UI        =     17,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA16F      =     18,
        V3D_OUTPUT_IMAGE_FORMAT_RG16F        =     19,
        V3D_OUTPUT_IMAGE_FORMAT_R16F         =     20,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA16I      =     21,
        V3D_OUTPUT_IMAGE_FORMAT_RG16I        =     22,
        V3D_OUTPUT_IMAGE_FORMAT_R16I         =     23,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA16UI     =     24,
        V3D_OUTPUT_IMAGE_FORMAT_RG16UI       =     25,
        V3D_OUTPUT_IMAGE_FORMAT_R16UI        =     26,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA8        =     27,
        V3D_OUTPUT_IMAGE_FORMAT_RGB8         =     28,
        V3D_OUTPUT_IMAGE_FORMAT_RG8          =     29,
        V3D_OUTPUT_IMAGE_FORMAT_R8           =     30,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA8I       =     31,
        V3D_OUTPUT_IMAGE_FORMAT_RG8I         =     32,
        V3D_OUTPUT_IMAGE_FORMAT_R8I          =     33,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI      =     34,
        V3D_OUTPUT_IMAGE_FORMAT_RG8UI        =     35,
        V3D_OUTPUT_IMAGE_FORMAT_R8UI         =     36,
        V3D_OUTPUT_IMAGE_FORMAT_BSTC8        =     39,
        V3D_OUTPUT_IMAGE_FORMAT_D32F         =     40,
        V3D_OUTPUT_IMAGE_FORMAT_D24          =     41,
        V3D_OUTPUT_IMAGE_FORMAT_D16          =     42,
        V3D_OUTPUT_IMAGE_FORMAT_D24S8        =     43,
        V3D_OUTPUT_IMAGE_FORMAT_S8           =     44,
        V3D_OUTPUT_IMAGE_FORMAT_RGBA5551     =     45,
};

enum V3D42_Dither_Mode {
        V3D_DITHER_MODE_NONE                 =      0,
        V3D_DITHER_MODE_RGB                  =      1,
        V3D_DITHER_MODE_A                    =      2,
        V3D_DITHER_MODE_RGBA                 =      3,
};

#define V3D42_FLUSH_opcode                     4
#define V3D42_FLUSH_header                      \
   .opcode                              =      4

struct V3D42_FLUSH {
   uint32_t                             opcode;
};

static inline void
V3D42_FLUSH_pack(__gen_user_data *data, uint8_t * restrict cl,
                 const struct V3D42_FLUSH * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

}

#define V3D42_FLUSH_length                     1

#define V3D42_START_TILE_BINNING_opcode        6
#define V3D42_START_TILE_BINNING_header         \
   .opcode                              =      6

struct V3D42_START_TILE_BINNING {
   uint32_t                             opcode;
};

static inline void
V3D42_START_TILE_BINNING_pack(__gen_user_data *data, uint8_t * restrict cl,
                              const struct V3D42_START_TILE_BINNING * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

}

#define V3D42_START_TILE_BINNING_length        1

#define V3D42_END_OF_RENDERING_opcode         13
#define V3D42_END_OF_RENDERING_header           \
   .opcode                              =     13

struct V3D42_END_OF_RENDERING {
   uint32_t                             opcode;
};

static inline void
V3D42_END_OF_RENDERING_pack(__gen_user_data *data, uint8_t * restrict cl,
                            const struct V3D42_END_OF_RENDERING * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

}

#define V3D42_END_OF_RENDERING_length          1

#define V3D42_RETURN_FROM_SUB_LIST_opcode     18
#define V3D42_RETURN_FROM_SUB_LIST_header       \
   .opcode                              =     18

struct V3D42_RETURN_FROM_SUB_LIST {
   uint32_t                             opcode;
};

static inline void
V3D42_RETURN_FROM_SUB_LIST_pack(__gen_user_data *data, uint8_t * restrict cl,
                                const struct V3D42_RETURN_FROM_SUB_LIST * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

}

#define V3D42_RETURN_FROM_SUB_LIST_length      1

#define V3D42_FLUSH_VCD_CACHE_opcode          19
#define V3D42_FLUSH_VCD_CACHE_header            \
   .opcode                              =     19

struct V3D42_FLUSH_VCD_CACHE {
   uint32_t                             opcode;
};

static inline void
V3D42_FLUSH_VCD_CACHE_pack(__gen_user_data *data, uint8_t * restrict cl,
                           const struct V3D42_FLUSH_VCD_CACHE * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

}

#define V3D42_FLUSH_VCD_CACHE_length           1

#define V3D42_START_ADDRESS_OF_GENERIC_TILE_LIST_opcode     20
#define V3D42_START_ADDRESS_OF_GENERIC_TILE_LIST_header\
   .opcode                              =     20

struct V3D42_START_ADDRESS_OF_GENERIC_TILE_LIST {
   uint32_t                             opcode;
   __gen_address_type                   start;
   __gen_address_type                   end;
};

static inline void
V3D42_START_ADDRESS_OF_GENERIC_TILE_LIST_pack(__gen_user_data *data, uint8_t * restrict cl,
                                              const struct V3D42_START_ADDRESS_OF_GENERIC_TILE_LIST * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   __gen_emit_reloc(data, &values->start);
   cl[ 1] = __gen_address_offset(&values->start);

   cl[ 2] = __gen_address_offset(&values->start) >> 8;

   cl[ 3] = __gen_address_offset(&values->start) >> 16;

   cl[ 4] = __gen_address_offset(&values->start) >> 24;

   __gen_emit_reloc(data, &values->end);
   cl[ 5] = __gen_address_offset(&values->end);

   cl[ 6] = __gen_address_offset(&values->end) >> 8;

   cl[ 7] = __gen_address_offset(&values->end) >> 16;

   cl[ 8] = __gen_address_offset(&values->end) >> 24;

}

#define V3D42_START_ADDRESS_OF_GENERIC_TILE_LIST_length      9

#define V3D42_BRANCH_TO_IMPLICIT_TILE_LIST_opcode     21
#define V3D42_BRANCH_TO_IMPLICIT_TILE_LIST_header\
   .opcode                              =     21

struct V3D42_BRANCH_TO_IMPLICIT_TILE_LIST {
   uint32_t                             opcode;
   uint32_t                             tile_list_set_number;
};

static inline void
V3D42_BRANCH_TO_IMPLICIT_TILE_LIST_pack(__gen_user_data *data, uint8_t * restrict cl,
                                        const struct V3D42_BRANCH_TO_IMPLICIT_TILE_LIST * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->tile_list_set_number, 0, 7);

}

#define V3D42_BRANCH_TO_IMPLICIT_TILE_LIST_length      2

#define V3D42_SUPERTILE_COORDINATES_opcode     23
#define V3D42_SUPERTILE_COORDINATES_header      \
   .opcode                              =     23

struct V3D42_SUPERTILE_COORDINATES {
   uint32_t                             opcode;
   uint32_t                             row_number_in_supertiles;
   uint32_t                             column_number_in_supertiles;
};

static inline void
V3D42_SUPERTILE_COORDINATES_pack(__gen_user_data *data, uint8_t * restrict cl,
                                 const struct V3D42_SUPERTILE_COORDINATES * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->column_number_in_supertiles, 0, 7);

   cl[ 2] = util_bitpack_uint(values->row_number_in_supertiles, 0, 7);

}

#define V3D42_SUPERTILE_COORDINATES_length      3

#define V3D42_CLEAR_TILE_BUFFERS_opcode       25
#define V3D42_CLEAR_TILE_BUFFERS_header         \
   .opcode                              =     25

struct V3D42_CLEAR_TILE_BUFFERS {
   uint32_t                             opcode;
   bool                                 clear_z_stencil_buffer;
   bool                                 clear_all_render_targets;
};

static inline void
V3D42_CLEAR_TILE_BUFFERS_pack(__gen_user_data *data, uint8_t * restrict cl,
                              const struct V3D42_CLEAR_TILE_BUFFERS * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->clear_z_stencil_buffer, 1, 1) |
            util_bitpack_uint(values->clear_all_render_targets, 0, 0);

}

#define V3D42_CLEAR_TILE_BUFFERS_length        2

#define V3D42_END_OF_LOADS_opcode             26
#define V3D42_END_OF_LOADS_header               \
   .opcode                              =     26

struct V3D42_END_OF_LOADS {
   uint32_t                             opcode;
};

static inline void
V3D42_END_OF_LOADS_pack(__gen_user_data *data, uint8_t * restrict cl,
                        const struct V3D42_END_OF_LOADS * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

}

#define V3D42_END_OF_LOADS_length              1

#define V3D42_END_OF_TILE_MARKER_opcode       27
#define V3D42_END_OF_TILE_MARKER_header         \
   .opcode                              =     27

struct V3D42_END_OF_TILE_MARKER {
   uint32_t                             opcode;
};

static inline void
V3D42_END_OF_TILE_MARKER_pack(__gen_user_data *data, uint8_t * restrict cl,
                              const struct V3D42_END_OF_TILE_MARKER * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

}

#define V3D42_END_OF_TILE_MARKER_length        1

#define V3D42_STORE_TILE_BUFFER_GENERAL_opcode     29
#define V3D42_STORE_TILE_BUFFER_GENERAL_header  \
   .opcode                              =     29

struct V3D42_STORE_TILE_BUFFER_GENERAL {
   uint32_t                             opcode;
   __gen_address_type                   address;
   uint32_t                             height;
   uint32_t                             height_in_ub_or_stride;
   bool                                 r_b_swap;
   bool                                 channel_reverse;
   bool                                 clear_buffer_being_stored;
   enum V3D42_Output_Image_Format       output_image_format;
   enum V3D42_Decimate_Mode             decimate_mode;
   enum V3D42_Dither_Mode               dither_mode;
   bool                                 flip_y;
   enum V3D42_Memory_Format             memory_format;
   uint32_t                             buffer_to_store;
#define RENDER_TARGET_0                          0
#define RENDER_TARGET_1                          1
#define RENDER_TARGET_2                          2
#define RENDER_TARGET_3                          3
#define NONE                                     8
#define Z                                        9
#define STENCIL                                  10
#define ZSTENCIL                                 11
};

static inline void
V3D42_STORE_TILE_BUFFER_GENERAL_pack(__gen_user_data *data, uint8_t * restrict cl,
                                     const struct V3D42_STORE_TILE_BUFFER_GENERAL * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->flip_y, 7, 7) |
            util_bitpack_uint(values->memory_format, 4, 6) |
            util_bitpack_uint(values->buffer_to_store, 0, 3);

   cl[ 2] = util_bitpack_uint(values->output_image_format, 4, 9) |
            util_bitpack_uint(values->decimate_mode, 2, 3) |
            util_bitpack_uint(values->dither_mode, 0, 1);

   cl[ 3] = util_bitpack_uint(values->r_b_swap, 4, 4) |
            util_bitpack_uint(values->channel_reverse, 3, 3) |
            util_bitpack_uint(values->clear_buffer_being_stored, 2, 2) |
            util_bitpack_uint(values->output_image_format, 4, 9) >> 8;

   cl[ 4] = util_bitpack_uint(values->height_in_ub_or_stride, 4, 23);

   cl[ 5] = util_bitpack_uint(values->height_in_ub_or_stride, 4, 23) >> 8;

   cl[ 6] = util_bitpack_uint(values->height_in_ub_or_stride, 4, 23) >> 16;

   cl[ 7] = util_bitpack_uint(values->height, 0, 15);

   cl[ 8] = util_bitpack_uint(values->height, 0, 15) >> 8;

   __gen_emit_reloc(data, &values->address);
   cl[ 9] = __gen_address_offset(&values->address);

   cl[10] = __gen_address_offset(&values->address) >> 8;

   cl[11] = __gen_address_offset(&values->address) >> 16;

   cl[12] = __gen_address_offset(&values->address) >> 24;

}

#define V3D42_STORE_TILE_BUFFER_GENERAL_length     13

#define V3D42_SET_INSTANCEID_opcode           54
#define V3D42_SET_INSTANCEID_header             \
   .opcode                              =     54

struct V3D42_SET_INSTANCEID {
   uint32_t                             opcode;
   uint32_t                             instance_id;
};

static inline void
V3D42_SET_INSTANCEID_pack(__gen_user_data *data, uint8_t * restrict cl,
                          const struct V3D42_SET_INSTANCEID * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);


   memcpy(&cl[1], &values->instance_id, sizeof(values->instance_id));
}

#define V3D42_SET_INSTANCEID_length            5

#define V3D42_PRIM_LIST_FORMAT_opcode         56
#define V3D42_PRIM_LIST_FORMAT_header           \
   .opcode                              =     56

struct V3D42_PRIM_LIST_FORMAT {
   uint32_t                             opcode;
   bool                                 tri_strip_or_fan;
   uint32_t                             primitive_type;
#define LIST_POINTS                              0
#define LIST_LINES                               1
#define LIST_TRIANGLES                           2
};

static inline void
V3D42_PRIM_LIST_FORMAT_pack(__gen_user_data *data, uint8_t * restrict cl,
                            const struct V3D42_PRIM_LIST_FORMAT * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->tri_strip_or_fan, 7, 7) |
            util_bitpack_uint(values->primitive_type, 0, 5);

}

#define V3D42_PRIM_LIST_FORMAT_length          2

#define V3D42_OCCLUSION_QUERY_COUNTER_opcode     92
#define V3D42_OCCLUSION_QUERY_COUNTER_header    \
   .opcode                              =     92

struct V3D42_OCCLUSION_QUERY_COUNTER {
   uint32_t                             opcode;
   __gen_address_type                   address;
};

static inline void
V3D42_OCCLUSION_QUERY_COUNTER_pack(__gen_user_data *data, uint8_t * restrict cl,
                                   const struct V3D42_OCCLUSION_QUERY_COUNTER * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   __gen_emit_reloc(data, &values->address);
   cl[ 1] = __gen_address_offset(&values->address);

   cl[ 2] = __gen_address_offset(&values->address) >> 8;

   cl[ 3] = __gen_address_offset(&values->address) >> 16;

   cl[ 4] = __gen_address_offset(&values->address) >> 24;

}

#define V3D42_OCCLUSION_QUERY_COUNTER_length      5

#define V3D42_NUMBER_OF_LAYERS_opcode        119
#define V3D42_NUMBER_OF_LAYERS_header           \
   .opcode                              =    119

struct V3D42_NUMBER_OF_LAYERS {
   uint32_t                             opcode;
   uint32_t                             number_of_layers;
};

static inline void
V3D42_NUMBER_OF_LAYERS_pack(__gen_user_data *data, uint8_t * restrict cl,
                            const struct V3D42_NUMBER_OF_LAYERS * restrict values)
{
   assert(values->number_of_layers >= 1);
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->number_of_layers - 1, 0, 7);

}

#define V3D42_NUMBER_OF_LAYERS_length          2

#define V3D42_TILE_BINNING_MODE_CFG_opcode    120
#define V3D42_TILE_BINNING_MODE_CFG_header      \
   .opcode                              =    120

struct V3D42_TILE_BINNING_MODE_CFG {
   uint32_t                             opcode;
   uint32_t                             height_in_pixels;
   uint32_t                             width_in_pixels;
   bool                                 double_buffer_in_non_ms_mode;
   bool                                 multisample_mode_4x;
   enum V3D42_Internal_BPP              maximum_bpp_of_all_render_targets;
   uint32_t                             number_of_render_targets;
   uint32_t                             tile_allocation_block_size;
#define TILE_ALLOCATION_BLOCK_SIZE_64B           0
#define TILE_ALLOCATION_BLOCK_SIZE_128B          1
#define TILE_ALLOCATION_BLOCK_SIZE_256B          2
   uint32_t                             tile_allocation_initial_block_size;
#define TILE_ALLOCATION_INITIAL_BLOCK_SIZE_64B   0
#define TILE_ALLOCATION_INITIAL_BLOCK_SIZE_128B  1
#define TILE_ALLOCATION_INITIAL_BLOCK_SIZE_256B  2
};

static inline void
V3D42_TILE_BINNING_MODE_CFG_pack(__gen_user_data *data, uint8_t * restrict cl,
                                 const struct V3D42_TILE_BINNING_MODE_CFG * restrict values)
{
   assert(values->height_in_pixels >= 1);
   assert(values->width_in_pixels >= 1);
   assert(values->number_of_render_targets >= 1);
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->tile_allocation_block_size, 4, 5) |
            util_bitpack_uint(values->tile_allocation_initial_block_size, 2, 3);

   cl[ 2] = util_bitpack_uint(values->double_buffer_in_non_ms_mode, 7, 7) |
            util_bitpack_uint(values->multisample_mode_4x, 6, 6) |
            util_bitpack_uint(values->maximum_bpp_of_all_render_targets, 4, 5) |
            util_bitpack_uint(values->number_of_render_targets - 1, 0, 3);

   cl[ 3] = 0;
   cl[ 4] = 0;
   cl[ 5] = util_bitpack_uint(values->width_in_pixels - 1, 0, 15);

   cl[ 6] = util_bitpack_uint(values->width_in_pixels - 1, 0, 15) >> 8;

   cl[ 7] = util_bitpack_uint(values->height_in_pixels - 1, 0, 15);

   cl[ 8] = util_bitpack_uint(values->height_in_pixels - 1, 0, 15) >> 8;

}

#define V3D42_TILE_BINNING_MODE_CFG_length      9

#define V3D42_TILE_RENDERING_MODE_CFG_COMMON_opcode    121
#define V3D42_TILE_RENDERING_MODE_CFG_COMMON_header\
   .opcode                              =    121,  \
   .sub_id                              =      0

struct V3D42_TILE_RENDERING_MODE_CFG_COMMON {
   uint32_t                             opcode;
   uint32_t                             pad;
   bool                                 early_depth_stencil_clear;
   enum V3D42_Internal_Depth_Type       internal_depth_type;
   bool                                 early_z_disable;
   uint32_t                             early_z_test_and_update_direction;
#define EARLY_Z_DIRECTION_LT_LE                  0
#define EARLY_Z_DIRECTION_GT_GE                  1
   bool                                 double_buffer_in_non_ms_mode;
   bool                                 multisample_mode_4x;
   enum V3D42_Internal_BPP              maximum_bpp_of_all_render_targets;
#define RENDER_TARGET_MAXIMUM_32BPP              0
#define RENDER_TARGET_MAXIMUM_64BPP              1
#define RENDER_TARGET_MAXIMUM_128BPP             2
   uint32_t                             image_height_pixels;
   uint32_t                             image_width_pixels;
   uint32_t                             number_of_render_targets;
   uint32_t                             sub_id;
};

static inline void
V3D42_TILE_RENDERING_MODE_CFG_COMMON_pack(__gen_user_data *data, uint8_t * restrict cl,
                                          const struct V3D42_TILE_RENDERING_MODE_CFG_COMMON * restrict values)
{
   assert(values->number_of_render_targets >= 1);
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->number_of_render_targets - 1, 4, 7) |
            util_bitpack_uint(values->sub_id, 0, 3);

   cl[ 2] = util_bitpack_uint(values->image_width_pixels, 0, 15);

   cl[ 3] = util_bitpack_uint(values->image_width_pixels, 0, 15) >> 8;

   cl[ 4] = util_bitpack_uint(values->image_height_pixels, 0, 15);

   cl[ 5] = util_bitpack_uint(values->image_height_pixels, 0, 15) >> 8;

   cl[ 6] = util_bitpack_uint(values->internal_depth_type, 7, 10) |
            util_bitpack_uint(values->early_z_disable, 6, 6) |
            util_bitpack_uint(values->early_z_test_and_update_direction, 5, 5) |
            util_bitpack_uint(values->double_buffer_in_non_ms_mode, 3, 3) |
            util_bitpack_uint(values->multisample_mode_4x, 2, 2) |
            util_bitpack_uint(values->maximum_bpp_of_all_render_targets, 0, 1);

   cl[ 7] = util_bitpack_uint(values->pad, 4, 15) |
            util_bitpack_uint(values->early_depth_stencil_clear, 3, 3) |
            util_bitpack_uint(values->internal_depth_type, 7, 10) >> 8;

   cl[ 8] = util_bitpack_uint(values->pad, 4, 15) >> 8;

}

#define V3D42_TILE_RENDERING_MODE_CFG_COMMON_length      9

#define V3D42_TILE_RENDERING_MODE_CFG_COLOR_opcode    121
#define V3D42_TILE_RENDERING_MODE_CFG_COLOR_header\
   .opcode                              =    121,  \
   .sub_id                              =      1

struct V3D42_TILE_RENDERING_MODE_CFG_COLOR {
   uint32_t                             opcode;
   uint32_t                             pad;
   enum V3D42_Render_Target_Clamp       render_target_3_clamp;
   enum V3D42_Internal_Type             render_target_3_internal_type;
   enum V3D42_Internal_BPP              render_target_3_internal_bpp;
   enum V3D42_Render_Target_Clamp       render_target_2_clamp;
   enum V3D42_Internal_Type             render_target_2_internal_type;
   enum V3D42_Internal_BPP              render_target_2_internal_bpp;
   enum V3D42_Render_Target_Clamp       render_target_1_clamp;
   enum V3D42_Internal_Type             render_target_1_internal_type;
   enum V3D42_Internal_BPP              render_target_1_internal_bpp;
   enum V3D42_Render_Target_Clamp       render_target_0_clamp;
   enum V3D42_Internal_Type             render_target_0_internal_type;
   enum V3D42_Internal_BPP              render_target_0_internal_bpp;
   uint32_t                             sub_id;
};

static inline void
V3D42_TILE_RENDERING_MODE_CFG_COLOR_pack(__gen_user_data *data, uint8_t * restrict cl,
                                         const struct V3D42_TILE_RENDERING_MODE_CFG_COLOR * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->render_target_0_internal_type, 6, 9) |
            util_bitpack_uint(values->render_target_0_internal_bpp, 4, 5) |
            util_bitpack_uint(values->sub_id, 0, 3);

   cl[ 2] = util_bitpack_uint(values->render_target_1_internal_type, 6, 9) |
            util_bitpack_uint(values->render_target_1_internal_bpp, 4, 5) |
            util_bitpack_uint(values->render_target_0_clamp, 2, 3) |
            util_bitpack_uint(values->render_target_0_internal_type, 6, 9) >> 8;

   cl[ 3] = util_bitpack_uint(values->render_target_2_internal_type, 6, 9) |
            util_bitpack_uint(values->render_target_2_internal_bpp, 4, 5) |
            util_bitpack_uint(values->render_target_1_clamp, 2, 3) |
            util_bitpack_uint(values->render_target_1_internal_type, 6, 9) >> 8;

   cl[ 4] = util_bitpack_uint(values->render_target_3_internal_type, 6, 9) |
            util_bitpack_uint(values->render_target_3_internal_bpp, 4, 5) |
            util_bitpack_uint(values->render_target_2_clamp, 2, 3) |
            util_bitpack_uint(values->render_target_2_internal_type, 6, 9) >> 8;

   cl[ 5] = util_bitpack_uint(values->pad, 4, 31) |
            util_bitpack_uint(values->render_target_3_clamp, 2, 3) |
            util_bitpack_uint(values->render_target_3_internal_type, 6, 9) >> 8;

   cl[ 6] = util_bitpack_uint(values->pad, 4, 31) >> 8;

   cl[ 7] = util_bitpack_uint(values->pad, 4, 31) >> 16;

   cl[ 8] = util_bitpack_uint(values->pad, 4, 31) >> 24;

}

#define V3D42_TILE_RENDERING_MODE_CFG_COLOR_length      9

#define V3D42_TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES_opcode    121
#define V3D42_TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES_header\
   .opcode                              =    121,  \
   .sub_id                              =      2

struct V3D42_TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES {
   uint32_t                             opcode;
   uint32_t                             unused;
   float                                z_clear_value;
   uint32_t                             stencil_clear_value;
   uint32_t                             sub_id;
};

static inline void
V3D42_TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES_pack(__gen_user_data *data, uint8_t * restrict cl,
                                                   const struct V3D42_TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->sub_id, 0, 3);

   cl[ 2] = util_bitpack_uint(values->stencil_clear_value, 0, 7);


   memcpy(&cl[3], &values->z_clear_value, sizeof(values->z_clear_value));
   cl[ 7] = util_bitpack_uint(values->unused, 0, 15);

   cl[ 8] = util_bitpack_uint(values->unused, 0, 15) >> 8;

}

#define V3D42_TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES_length      9

#define V3D42_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1_opcode    121
#define V3D42_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1_header\
   .opcode                              =    121,  \
   .sub_id                              =      3

struct V3D42_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1 {
   uint32_t                             opcode;
   uint32_t                             clear_color_next_24_bits;
   uint32_t                             clear_color_low_32_bits;
   uint32_t                             render_target_number;
   uint32_t                             sub_id;
};

static inline void
V3D42_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1_pack(__gen_user_data *data, uint8_t * restrict cl,
                                                      const struct V3D42_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1 * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->render_target_number, 4, 7) |
            util_bitpack_uint(values->sub_id, 0, 3);


   memcpy(&cl[2], &values->clear_color_low_32_bits, sizeof(values->clear_color_low_32_bits));
   cl[ 6] = util_bitpack_uint(values->clear_color_next_24_bits, 0, 23);

   cl[ 7] = util_bitpack_uint(values->clear_color_next_24_bits, 0, 23) >> 8;

   cl[ 8] = util_bitpack_uint(values->clear_color_next_24_bits, 0, 23) >> 16;

}

#define V3D42_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1_length      9

#define V3D42_TILE_COORDINATES_opcode        124
#define V3D42_TILE_COORDINATES_header           \
   .opcode                              =    124

struct V3D42_TILE_COORDINATES {
   uint32_t                             opcode;
   uint32_t                             tile_row_number;
   uint32_t                             tile_column_number;
};

static inline void
V3D42_TILE_COORDINATES_pack(__gen_user_data *data, uint8_t * restrict cl,
                            const struct V3D42_TILE_COORDINATES * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->tile_column_number, 0, 11);

   cl[ 2] = util_bitpack_uint(values->tile_row_number, 4, 15) |
            util_bitpack_uint(values->tile_column_number, 0, 11) >> 8;

   cl[ 3] = util_bitpack_uint(values->tile_row_number, 4, 15) >> 8;

}

#define V3D42_TILE_COORDINATES_length          4

#define V3D42_MULTICORE_RENDERING_SUPERTILE_CFG_opcode    122
#define V3D42_MULTICORE_RENDERING_SUPERTILE_CFG_header\
   .opcode                              =    122

struct V3D42_MULTICORE_RENDERING_SUPERTILE_CFG {
   uint32_t                             opcode;
   uint32_t                             number_of_bin_tile_lists;
   bool                                 supertile_raster_order;
   bool                                 multicore_enable;
   uint32_t                             total_frame_height_in_tiles;
   uint32_t                             total_frame_width_in_tiles;
   uint32_t                             total_frame_height_in_supertiles;
   uint32_t                             total_frame_width_in_supertiles;
   uint32_t                             supertile_height_in_tiles;
   uint32_t                             supertile_width_in_tiles;
};

static inline void
V3D42_MULTICORE_RENDERING_SUPERTILE_CFG_pack(__gen_user_data *data, uint8_t * restrict cl,
                                             const struct V3D42_MULTICORE_RENDERING_SUPERTILE_CFG * restrict values)
{
   assert(values->number_of_bin_tile_lists >= 1);
   assert(values->supertile_height_in_tiles >= 1);
   assert(values->supertile_width_in_tiles >= 1);
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->supertile_width_in_tiles - 1, 0, 7);

   cl[ 2] = util_bitpack_uint(values->supertile_height_in_tiles - 1, 0, 7);

   cl[ 3] = util_bitpack_uint(values->total_frame_width_in_supertiles, 0, 7);

   cl[ 4] = util_bitpack_uint(values->total_frame_height_in_supertiles, 0, 7);

   cl[ 5] = util_bitpack_uint(values->total_frame_width_in_tiles, 0, 11);

   cl[ 6] = util_bitpack_uint(values->total_frame_height_in_tiles, 4, 15) |
            util_bitpack_uint(values->total_frame_width_in_tiles, 0, 11) >> 8;

   cl[ 7] = util_bitpack_uint(values->total_frame_height_in_tiles, 4, 15) >> 8;

   cl[ 8] = util_bitpack_uint(values->number_of_bin_tile_lists - 1, 5, 7) |
            util_bitpack_uint(values->supertile_raster_order, 4, 4) |
            util_bitpack_uint(values->multicore_enable, 0, 0);

}

#define V3D42_MULTICORE_RENDERING_SUPERTILE_CFG_length      9

#define V3D42_MULTICORE_RENDERING_TILE_LIST_SET_BASE_opcode    123
#define V3D42_MULTICORE_RENDERING_TILE_LIST_SET_BASE_header\
   .opcode                              =    123

struct V3D42_MULTICORE_RENDERING_TILE_LIST_SET_BASE {
   uint32_t                             opcode;
   __gen_address_type                   address;
   uint32_t                             tile_list_set_number;
};

static inline void
V3D42_MULTICORE_RENDERING_TILE_LIST_SET_BASE_pack(__gen_user_data *data, uint8_t * restrict cl,
                                                  const struct V3D42_MULTICORE_RENDERING_TILE_LIST_SET_BASE * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   __gen_emit_reloc(data, &values->address);
   cl[ 1] = __gen_address_offset(&values->address) |
            util_bitpack_uint(values->tile_list_set_number, 0, 3);

   cl[ 2] = __gen_address_offset(&values->address) >> 8;

   cl[ 3] = __gen_address_offset(&values->address) >> 16;

   cl[ 4] = __gen_address_offset(&values->address) >> 24;

}

#define V3D42_MULTICORE_RENDERING_TILE_LIST_SET_BASE_length      5

#define V3D42_TILE_COORDINATES_IMPLICIT_opcode    125
#define V3D42_TILE_COORDINATES_IMPLICIT_header  \
   .opcode                              =    125

struct V3D42_TILE_COORDINATES_IMPLICIT {
   uint32_t                             opcode;
};

static inline void
V3D42_TILE_COORDINATES_IMPLICIT_pack(__gen_user_data *data, uint8_t * restrict cl,
                                     const struct V3D42_TILE_COORDINATES_IMPLICIT * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

}

#define V3D42_TILE_COORDINATES_IMPLICIT_length      1

#define V3D42_TILE_LIST_INITIAL_BLOCK_SIZE_opcode    126
#define V3D42_TILE_LIST_INITIAL_BLOCK_SIZE_header\
   .opcode                              =    126

struct V3D42_TILE_LIST_INITIAL_BLOCK_SIZE {
   uint32_t                             opcode;
   bool                                 use_auto_chained_tile_lists;
   uint32_t                             size_of_first_block_in_chained_tile_lists;
#define TILE_ALLOCATION_BLOCK_SIZE_64B           0
#define TILE_ALLOCATION_BLOCK_SIZE_128B          1
#define TILE_ALLOCATION_BLOCK_SIZE_256B          2
};

static inline void
V3D42_TILE_LIST_INITIAL_BLOCK_SIZE_pack(__gen_user_data *data, uint8_t * restrict cl,
                                        const struct V3D42_TILE_LIST_INITIAL_BLOCK_SIZE * restrict values)
{
   cl[ 0] = util_bitpack_uint(values->opcode, 0, 7);

   cl[ 1] = util_bitpack_uint(values->use_auto_chained_tile_lists, 2, 2) |
            util_bitpack_uint(values->size_of_first_block_in_chained_tile_lists, 0, 1);

}

#define V3D42_TILE_LIST_INITIAL_BLOCK_SIZE_length      2

/* ---- END verbatim copy from Mesa v3d_packet_v42_pack.h ---- */

#pragma GCC diagnostic pop


/* ------------------------------------------------------------------------ */
/* CL writer                                                                 */
/* ------------------------------------------------------------------------ */

/* Packets are packed into a local staging buffer (the generated packers do byte
 * and unaligned 32-bit stores, which fault on a Device-memory mapping) and then
 * copied out with aligned 32-bit stores. */
typedef struct {
	uint8_t *buf;
	uint32_t len;
	uint32_t cap;
	int overflow;
	uint8_t scratch[16]; /* sink for a packet that does not fit (longest is 13 B) */
} clw_t;


static uint8_t *clw_reserve(clw_t *w, uint32_t n)
{
	uint8_t *p;

	if ((w->overflow != 0) || (n > w->cap - w->len)) {
		w->overflow = 1;
		return w->scratch;
	}
	p = w->buf + w->len;
	w->len += n;
	return p;
}


/* Mesa's cl_emit() (v3d_cl.h): a one-shot for loop whose body fills the packet
 * struct, packed into the CL when the body ends. The *_header macro presets the
 * opcode (and sub_id); every other field starts at zero, as in Mesa. */
#define cl_emit(w, packet, name) \
	for (struct V3D42_##packet name = { V3D42_##packet##_header }, *_loop_##name = &name; \
		_loop_##name != NULL; \
		V3D42_##packet##_pack(NULL, clw_reserve((w), V3D42_##packet##_length), &name), _loop_##name = NULL)


static uint32_t align_u32(uint32_t v, uint32_t a)
{
	return (v + a - 1u) & ~(a - 1u);
}


/* Copies len staged bytes to dst and zero-fills up to total (a multiple of 4),
 * using aligned 32-bit stores only. dst is 4-byte aligned. */
static void copy_out(void *dst, const uint8_t *src, uint32_t len, uint32_t total)
{
	volatile uint32_t *d = dst;
	uint32_t i, word;

	for (i = 0u; i < total; i += 4u) {
		word = 0u;
		if (i < len) {
			memcpy(&word, src + i, ((len - i) < 4u) ? (len - i) : 4u);
		}
		d[i / 4u] = word;
	}
}


/* ------------------------------------------------------------------------ */
/* 1. clear                                                                  */
/* ------------------------------------------------------------------------ */

/* broadcom/common/v3d_limits.h, v3d_device_info.c (ver 42) */
#define V3D_TILE_ALLOC_INITIAL_BLOCK_SIZE 128u
#define V3D_MAX_SUPERTILES                256u
#define V3D_PAGE_SIZE                     4096u

/* Tile size. v3d_get_tile_buffer_size() -> v3d_choose_tile_size(), ver < 71:
 *   idx  = 0          one colour attachment (color_attachment_count = 1)
 *        + 0          no MSAA, no double-buffer (Mesa only enables DB under
 *                     V3D_DEBUG=double_buffer)
 *        + 0          max internal bpp = V3D_INTERNAL_BPP_32 (RGBA8 -> type 8, bpp 32)
 *   tile_sizes[0] = 64 x 64. */
#define CLEAR_TILE_W 64u
#define CLEAR_TILE_H 64u

/* Byte lengths of the fixed packet runs emitted below (checked after emission). */
#define CLEAR_BCL_LEN \
	(V3D42_NUMBER_OF_LAYERS_length + V3D42_TILE_BINNING_MODE_CFG_length + V3D42_FLUSH_VCD_CACHE_length + \
		V3D42_OCCLUSION_QUERY_COUNTER_length + V3D42_START_TILE_BINNING_length + V3D42_FLUSH_length)

#define CLEAR_INIT_TILE_LEN \
	(V3D42_TILE_COORDINATES_length + V3D42_END_OF_LOADS_length + V3D42_STORE_TILE_BUFFER_GENERAL_length + \
		V3D42_END_OF_TILE_MARKER_length)

/* Main RCL without the SUPERTILE_COORDINATES run. */
#define CLEAR_RCL_FIXED_LEN \
	(V3D42_TILE_RENDERING_MODE_CFG_COMMON_length + V3D42_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1_length + \
		V3D42_TILE_RENDERING_MODE_CFG_COLOR_length + V3D42_TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES_length + \
		V3D42_TILE_LIST_INITIAL_BLOCK_SIZE_length + V3D42_MULTICORE_RENDERING_TILE_LIST_SET_BASE_length + \
		V3D42_MULTICORE_RENDERING_SUPERTILE_CFG_length + 2u * CLEAR_INIT_TILE_LEN + \
		V3D42_CLEAR_TILE_BUFFERS_length + V3D42_FLUSH_VCD_CACHE_length + \
		V3D42_START_ADDRESS_OF_GENERIC_TILE_LIST_length + V3D42_END_OF_RENDERING_length)

#define CLEAR_GENERIC_LEN \
	(V3D42_TILE_COORDINATES_IMPLICIT_length + V3D42_END_OF_LOADS_length + V3D42_PRIM_LIST_FORMAT_length + \
		V3D42_SET_INSTANCEID_length + V3D42_BRANCH_TO_IMPLICIT_TILE_LIST_length + \
		V3D42_STORE_TILE_BUFFER_GENERAL_length + V3D42_CLEAR_TILE_BUFFERS_length + \
		V3D42_END_OF_TILE_MARKER_length + V3D42_RETURN_FROM_SUB_LIST_length)

/* The generic per-tile list (Mesa keeps it in the job's separate "indirect" CL)
 * is placed in the RCL buffer after END_OF_RENDERING, at this alignment. */
#define CLEAR_GENERIC_ALIGN 16u

#define CLEAR_RCL_STAGE_MAX 1024u

_Static_assert(CLEAR_RCL_FIXED_LEN + 3u * V3D_MAX_SUPERTILES + CLEAR_GENERIC_ALIGN + CLEAR_GENERIC_LEN <=
		CLEAR_RCL_STAGE_MAX,
	"RCL staging buffer too small");


typedef struct {
	uint32_t tiles_x, tiles_y;           /* job->tile_desc.draw_x / draw_y */
	uint32_t supertile_w, supertile_h;   /* in tiles */
	uint32_t frame_w_st, frame_h_st;     /* frame size in supertiles */
	uint32_t rcl_len;                    /* main RCL up to and including END_OF_RENDERING */
	uint32_t gen_off;                    /* generic per-tile list offset in the RCL buffer */
	uint32_t bcl_size, rcl_size;         /* buffer sizes incl. CLE readahead */
	uint32_t tile_alloc_size, tile_state_size;
} clear_geom_t;


static int clear_geom(uint32_t width, uint32_t height, clear_geom_t *g)
{
	uint32_t tiles_size, alloc_size;

	if ((width == 0u) || (height == 0u) || (width > V3DA_CLGEN_CLEAR_MAX_DIM) || (height > V3DA_CLGEN_CLEAR_MAX_DIM)) {
		return -EINVAL;
	}

	/* v3d_job.c v3d_get_job(): draw_x/y = DIV_ROUND_UP(fb size, tile size) */
	g->tiles_x = (width + CLEAR_TILE_W - 1u) / CLEAR_TILE_W;
	g->tiles_y = (height + CLEAR_TILE_H - 1u) / CLEAR_TILE_H;

	/* v3dx_rcl.c emit_render_layer(): size up supertiles until under the limit.
	 * The HW allows V3D_MAX_SUPERTILES but not 1xN / Nx1 frames of that size. */
	g->supertile_w = 1u;
	g->supertile_h = 1u;
	for (;;) {
		g->frame_w_st = (g->tiles_x + g->supertile_w - 1u) / g->supertile_w;
		g->frame_h_st = (g->tiles_y + g->supertile_h - 1u) / g->supertile_h;
		if ((g->frame_w_st < V3D_MAX_SUPERTILES) && (g->frame_h_st < V3D_MAX_SUPERTILES) &&
				(g->frame_w_st * g->frame_h_st <= V3D_MAX_SUPERTILES)) {
			break;
		}
		if (g->supertile_w < g->supertile_h) {
			g->supertile_w++;
		}
		else {
			g->supertile_h++;
		}
	}

	/* A clear covers the whole frame (v3d_tlb_clear: draw_min 0, draw_max = fb
	 * size, scissor disabled), so the SUPERTILE_COORDINATES loop visits every
	 * supertile: max_x_supertile = (width - 1) / (64 * supertile_w) = frame_w_st - 1. */
	g->rcl_len = CLEAR_RCL_FIXED_LEN + g->frame_w_st * g->frame_h_st * V3D42_SUPERTILE_COORDINATES_length;
	g->gen_off = align_u32(g->rcl_len, CLEAR_GENERIC_ALIGN);

	g->bcl_size = align_u32(CLEAR_BCL_LEN, 4u) + V3DA_CLGEN_CLE_READAHEAD;
	g->rcl_size = g->gen_off + align_u32(CLEAR_GENERIC_LEN, 4u) + V3DA_CLGEN_CLE_READAHEAD;

	/* broadcom/common/v3d_util.c v3d_tile_alloc_sizes(layers = 1, draws = 0,
	 * page_size = 4096), as called by v3d_job.c alloc_tile_state():
	 *   tiles_size = tiles * 128           (initial 128 B block per tile, PTB)
	 *   alloc      = align(tiles_size, 4096) + 8192   (first two 4 KiB PTB chunks)
	 *              + MIN(tiles_size * draws / 2, 512 KiB) = + 0
	 *   alloc      = align(alloc, page_size)
	 *   tile_state = tiles * 256
	 * 64x64: 1 tile -> tile_alloc 12288, tile_state 256. */
	tiles_size = g->tiles_x * g->tiles_y * V3D_TILE_ALLOC_INITIAL_BLOCK_SIZE;
	alloc_size = align_u32(tiles_size, 4096u) + 8192u;
	g->tile_alloc_size = align_u32(alloc_size, V3D_PAGE_SIZE);
	g->tile_state_size = g->tiles_x * g->tiles_y * 256u;

	return 0;
}


int v3da_clgen_clear_sizes(uint32_t width, uint32_t height, uint32_t *bcl_size, uint32_t *rcl_size,
	uint32_t *tile_alloc_size, uint32_t *tile_state_size)
{
	clear_geom_t g;
	int err = clear_geom(width, height, &g);

	if (err != 0) {
		return err;
	}
	if (bcl_size != NULL) {
		*bcl_size = g.bcl_size;
	}
	if (rcl_size != NULL) {
		*rcl_size = g.rcl_size;
	}
	if (tile_alloc_size != NULL) {
		*tile_alloc_size = g.tile_alloc_size;
	}
	if (tile_state_size != NULL) {
		*tile_state_size = g.tile_state_size;
	}
	return 0;
}


static int buf_ok(const v3da_clgen_buf_t *b, uint32_t need, uint32_t gpuva_align)
{
	if ((b == NULL) || (b->cpu == NULL) || (((uintptr_t)b->cpu & 3u) != 0u) ||
			((b->gpuva & (gpuva_align - 1u)) != 0u) || (b->gpuva == 0u)) {
		return -EINVAL;
	}
	if (b->size < need) {
		return -ENOSPC;
	}
	return 0;
}


/* The per-RT store of the generic tile list: v3dx_rcl.c store_general() for
 * RENDER_TARGET_0, PIPE_FORMAT_R8G8B8A8_UNORM, raster, single-sampled. */
static void clear_emit_rt_store(clw_t *cl, uint32_t rt_gpuva, uint32_t stride)
{
	cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
		store.buffer_to_store = RENDER_TARGET_0;
		store.address = rt_gpuva;
		store.clear_buffer_being_stored = false;
		/* v3d_get_rt_format(R8G8B8A8_UNORM): format table rt = RGBA8 */
		store.output_image_format = V3D_OUTPUT_IMAGE_FORMAT_RGBA8;
		/* v3d_format_needs_tlb_rb_swap(): swizzle[0] is X, not Z */
		store.r_b_swap = false;
		store.memory_format = V3D_MEMORY_FORMAT_RASTER;
		/* v3d_surface_get_height_in_ub_or_stride(): raster -> slice->stride (bytes) */
		store.height_in_ub_or_stride = stride;
		store.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
	}
}


int v3da_clgen_clear(uint32_t width, uint32_t height, uint32_t rt_gpuva, uint32_t clear_rgba,
	v3da_clgen_buf_t *bcl, v3da_clgen_buf_t *rcl, uint32_t tile_alloc_gpuva, uint32_t tile_alloc_size,
	uint32_t tile_state_gpuva, struct drm_v3d_submit_cl *out)
{
	clear_geom_t g;
	uint8_t bcl_stage[CLEAR_BCL_LEN];
	uint8_t rcl_stage[CLEAR_RCL_STAGE_MAX];
	clw_t b, r, gen;
	uint32_t gen_start, gen_end, x, y, max_x_st, max_y_st;
	uint32_t st_w_px, st_h_px;
	int err;

	err = clear_geom(width, height, &g);
	if (err != 0) {
		return err;
	}
	if (out == NULL) {
		return -EINVAL;
	}
	err = buf_ok(bcl, g.bcl_size, V3DA_CLGEN_ALIGN);
	if (err == 0) {
		err = buf_ok(rcl, g.rcl_size, V3DA_CLGEN_ALIGN);
	}
	if (err != 0) {
		return err;
	}
	if ((rt_gpuva == 0u) || (tile_alloc_gpuva == 0u) || (tile_state_gpuva == 0u) ||
			((rt_gpuva & (V3DA_CLGEN_ALIGN - 1u)) != 0u) ||
			((tile_alloc_gpuva & (V3DA_CLGEN_ALIGN - 1u)) != 0u) ||
			((tile_state_gpuva & (V3DA_CLGEN_ALIGN - 1u)) != 0u) ||
			((tile_alloc_size & (V3D_PAGE_SIZE - 1u)) != 0u)) {
		return -EINVAL;
	}
	if (tile_alloc_size < g.tile_alloc_size) {
		return -ENOSPC;
	}

	/* ---- BCL: v3dx_draw.c v3dX(start_binning)() + v3dx_job.c v3dX(bcl_epilogue)() ---- */
	memset(&b, 0, sizeof(b));
	b.buf = bcl_stage;
	b.cap = sizeof(bcl_stage);

	/* num_layers = util_framebuffer_get_num_layers() = 1 (> 0, so emitted) */
	cl_emit(&b, NUMBER_OF_LAYERS, config) {
		config.number_of_layers = 1u;
	}

	cl_emit(&b, TILE_BINNING_MODE_CFG, config) {
		config.width_in_pixels = width;
		config.height_in_pixels = height;
		config.number_of_render_targets = 1u;
		config.multisample_mode_4x = false;
		config.double_buffer_in_non_ms_mode = false;
		config.maximum_bpp_of_all_render_targets = V3D_INTERNAL_BPP_32;
		/* V3D_TILE_ALLOC_INITIAL_BLOCK_SIZE_ENUM = 128 >> 7 = 1 (128 B),
		 * V3D_TILE_ALLOC_OVERFLOW_BLOCK_SIZE_ENUM = 64 >> 7 = 0 (64 B) */
		config.tile_allocation_initial_block_size = TILE_ALLOCATION_INITIAL_BLOCK_SIZE_128B;
		config.tile_allocation_block_size = TILE_ALLOCATION_BLOCK_SIZE_64B;
	}

	/* There's definitely nothing in the VCD cache we want. */
	cl_emit(&b, FLUSH_VCD_CACHE, bin);

	/* Disable any leftover OQ state from another job (address 0). */
	cl_emit(&b, OCCLUSION_QUERY_COUNTER, counter);

	/* "Binning mode lists must have a Start Tile Binning item (6) after any
	 *  prefix state data before the binning list proper starts." */
	cl_emit(&b, START_TILE_BINNING, bin);

	/* Zero draws. bcl_epilogue: no TF, no primitive counts -> just FLUSH, which
	 * makes the binner cap every tile list with a return. */
	cl_emit(&b, FLUSH, flush);

	assert((b.overflow == 0) && (b.len == CLEAR_BCL_LEN));

	/* ---- RCL: v3dx_rcl.c v3dX(emit_rcl)() ---- */
	memset(rcl_stage, 0, sizeof(rcl_stage));
	memset(&r, 0, sizeof(r));
	r.buf = rcl_stage;
	r.cap = g.rcl_len;
	memset(&gen, 0, sizeof(gen));
	gen.buf = rcl_stage + g.gen_off;
	gen.cap = CLEAR_GENERIC_LEN;
	gen_start = rcl->gpuva + g.gen_off;
	gen_end = gen_start + CLEAR_GENERIC_LEN;

	/* Common config must be the first TILE_RENDERING_MODE_CFG and
	 * Z_STENCIL_CLEAR_VALUES must be last. */
	cl_emit(&r, TILE_RENDERING_MODE_CFG_COMMON, config) {
		/* no zsbuf: internal_depth_type stays 0 */
		/* no draws: !decided_global_ez_enable -> early Z disabled */
		config.early_z_disable = true;
		/* early_zs_clear = clear_tlb & DEPTHSTENCIL (none) -> false */
		config.early_depth_stencil_clear = false;
		config.image_width_pixels = width;
		config.image_height_pixels = height;
		config.number_of_render_targets = 1u;
		config.multisample_mode_4x = false;
		config.double_buffer_in_non_ms_mode = false;
		config.maximum_bpp_of_all_render_targets = V3D_INTERNAL_BPP_32;
	}

	/* V3D_VERSION == 42 clear colour for RT0. v3dx_draw.c v3d_tlb_clear():
	 * INTERNAL_TYPE_8 -> util_pack_color(R8G8B8A8_UNORM) into clear_color[0][0]
	 * (R in bits 7:0), clear_color[0][1] = 0. Internal bpp 32 and raster (no UIF
	 * pad) -> no PART2 / PART3. */
	cl_emit(&r, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1, clear) {
		clear.clear_color_low_32_bits = clear_rgba;
		clear.clear_color_next_24_bits = 0u;
		clear.render_target_number = 0u;
	}

	/* v3d_setup_render_target(): RT0 type 8 / bpp 32 / clamp NONE (not sRGB, not
	 * pure integer); RTs 1..3 have no texture and are left zero. */
	cl_emit(&r, TILE_RENDERING_MODE_CFG_COLOR, rt) {
		rt.render_target_0_internal_bpp = V3D_INTERNAL_BPP_32;
		rt.render_target_0_internal_type = V3D_INTERNAL_TYPE_8;
		rt.render_target_0_clamp = V3D_RENDER_TARGET_CLAMP_NONE;
	}

	/* Ends rendering mode config. clear_z / clear_s never set: 0. */
	cl_emit(&r, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
		clear.z_clear_value = 0.0f;
		clear.stencil_clear_value = 0u;
	}

	/* Always set initial block size before the first branch, which needs to
	 * match the value from binning mode config. */
	cl_emit(&r, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
		init.use_auto_chained_tile_lists = true;
		init.size_of_first_block_in_chained_tile_lists = TILE_ALLOCATION_BLOCK_SIZE_128B;
	}

	/* ---- emit_render_layer(layer 0) ---- */
	cl_emit(&r, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
		list.address = tile_alloc_gpuva; /* + layer 0 offset */
	}

	cl_emit(&r, MULTICORE_RENDERING_SUPERTILE_CFG, config) {
		config.number_of_bin_tile_lists = 1u;
		config.total_frame_width_in_tiles = g.tiles_x;
		config.total_frame_height_in_tiles = g.tiles_y;
		config.supertile_width_in_tiles = g.supertile_w;
		config.supertile_height_in_tiles = g.supertile_h;
		config.total_frame_width_in_supertiles = g.frame_w_st;
		config.total_frame_height_in_supertiles = g.frame_h_st;
	}

	/* Start by clearing the tile buffer. */
	cl_emit(&r, TILE_COORDINATES, coords) {
		coords.tile_column_number = 0u;
		coords.tile_row_number = 0u;
	}

	/* Initial clear of the tile buffers + the GFXH-1742 workaround (2 dummy
	 * stores between TLB type/size changes on 4.x). Pass 1 does not clear again:
	 * do_double_initial_tile_clear() needs double-buffer mode. */
	cl_emit(&r, END_OF_LOADS, end);
	cl_emit(&r, STORE_TILE_BUFFER_GENERAL, store) {
		store.buffer_to_store = NONE;
	}
	cl_emit(&r, CLEAR_TILE_BUFFERS, clear) {
		clear.clear_z_stencil_buffer = true; /* !early_zs_clear */
		clear.clear_all_render_targets = true;
	}
	cl_emit(&r, END_OF_TILE_MARKER, end);

	cl_emit(&r, TILE_COORDINATES, coords);
	cl_emit(&r, END_OF_LOADS, end);
	cl_emit(&r, STORE_TILE_BUFFER_GENERAL, store) {
		store.buffer_to_store = NONE;
	}
	cl_emit(&r, END_OF_TILE_MARKER, end);

	cl_emit(&r, FLUSH_VCD_CACHE, flush);

	/* v3d_rcl_emit_generic_per_tile_list(): the list itself ... */
	cl_emit(&gen, TILE_COORDINATES_IMPLICIT, coords);

	/* v3d_rcl_emit_loads(): job->load == 0 -> no loads */
	cl_emit(&gen, END_OF_LOADS, end);

	/* The binner starts out writing tiles assuming that the initial mode is
	 * triangles, so make sure that's the case. */
	cl_emit(&gen, PRIM_LIST_FORMAT, fmt) {
		fmt.primitive_type = LIST_TRIANGLES;
	}

	/* PTB assumes that value to be 0, but hw will not set it. */
	cl_emit(&gen, SET_INSTANCEID, set) {
		set.instance_id = 0u;
	}

	cl_emit(&gen, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

	/* v3d_rcl_emit_stores(): store RT0, then (clear_tlb != 0) the V3D 4.2
	 * CLEAR_TILE_BUFFERS (GFXH-1461/GFXH-1689). */
	clear_emit_rt_store(&gen, rt_gpuva, V3DA_CLGEN_CLEAR_RT_STRIDE(width));
	cl_emit(&gen, CLEAR_TILE_BUFFERS, clear) {
		clear.clear_z_stencil_buffer = true; /* !early_zs_clear */
		clear.clear_all_render_targets = true;
	}

	cl_emit(&gen, END_OF_TILE_MARKER, end);

	cl_emit(&gen, RETURN_FROM_SUB_LIST, ret);

	/* ... and the RCL's pointer to it. */
	cl_emit(&r, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
		branch.start = gen_start;
		branch.end = gen_end;
	}

	/* does_rasterization (set by v3d_clear) -> the supertile loop, raster order. */
	st_w_px = CLEAR_TILE_W * g.supertile_w;
	st_h_px = CLEAR_TILE_H * g.supertile_h;
	max_x_st = (width - 1u) / st_w_px;
	max_y_st = (height - 1u) / st_h_px;
	for (y = 0u; y <= max_y_st; y++) {
		for (x = 0u; x <= max_x_st; x++) {
			cl_emit(&r, SUPERTILE_COORDINATES, coords) {
				coords.column_number_in_supertiles = x;
				coords.row_number_in_supertiles = y;
			}
		}
	}

	cl_emit(&r, END_OF_RENDERING, end);

	assert((r.overflow == 0) && (r.len == g.rcl_len));
	assert((gen.overflow == 0) && (gen.len == CLEAR_GENERIC_LEN));
	if ((b.overflow != 0) || (r.overflow != 0) || (gen.overflow != 0) || (r.len != g.rcl_len)) {
		return -ENOSPC; /* unreachable: sizes are exact */
	}

	copy_out(bcl->cpu, bcl_stage, b.len, g.bcl_size);
	copy_out(rcl->cpu, rcl_stage, g.gen_off + gen.len, g.rcl_size);

	/* v3d_job_submit(): scalars for ver >= 42 */
	memset(out, 0, sizeof(*out));
	out->bcl_start = bcl->gpuva;
	out->bcl_end = bcl->gpuva + b.len;
	out->rcl_start = rcl->gpuva;
	out->rcl_end = rcl->gpuva + r.len;
	out->qma = tile_alloc_gpuva;
	out->qms = tile_alloc_size;
	out->qts = tile_state_gpuva;

	return 0;
}


/* ------------------------------------------------------------------------ */
/* 2. tfu                                                                    */
/* ------------------------------------------------------------------------ */

/* broadcom/common/v3d_tfu.h (V3D 3.3 .. 4.2 register layout) */
#define V3D33_TFU_IOA_FORMAT_SHIFT              3
#define V3D33_TFU_IOA_FORMAT_UBLINEAR_2_COLUMN  5
#define V3D33_TFU_ICFG_NUMMM_SHIFT              5
#define V3D33_TFU_ICFG_TTYPE_SHIFT              9
#define V3D33_TFU_ICFG_FORMAT_SHIFT             18
#define V3D33_TFU_ICFG_FORMAT_RASTER            0

/* enum V3D42_Texture_Data_Formats: TEXTURE_DATA_FORMAT_R32F */
#define V3D42_TEXTURE_DATA_FORMAT_R32F          29


uint32_t v3da_clgen_tfu_src_size(uint32_t width, uint32_t height)
{
	return width * 4u * height;
}


uint32_t v3da_clgen_tfu_out_size(uint32_t width, uint32_t height)
{
	/* v3d_setup_slices(), UBLINEAR_2_COLUMN: width aligned to 2 UIF blocks
	 * (16 px), height to 1 UIF block (8 px); stride = 16 * 4 = 64 bytes. */
	(void)width;
	return 64u * align_u32(height, 8u);
}


uint32_t v3da_clgen_tfu_out_offset(uint32_t x, uint32_t y)
{
	/* v3d_tiling.c v3d_get_ublinear_pixel_offset(cpp 4, x, y, 2) */
	return 256u * (2u * (y >> 3) + (x >> 3)) + (((x & 4u) != 0u) ? 64u : 0u) + (((y & 4u) != 0u) ? 128u : 0u) +
		(y & 3u) * 16u + (x & 3u) * 4u;
}


uint32_t v3da_clgen_tfu_pattern(uint32_t x, uint32_t y)
{
	/* A = 0x5A: exponent bits 0xb4/0xb5 -> always a normal float32 */
	return 0x5a000000u | ((y & 0xffu) << 16) | ((x & 0xffu) << 8) | ((x * 7u + y * 13u) & 0xffu);
}


int v3da_clgen_tfu(uint32_t width, uint32_t height, uint32_t src_gpuva, uint32_t src_size,
	uint32_t dst_gpuva, uint32_t dst_size, struct drm_v3d_submit_tfu *out)
{
	if ((out == NULL) || (width < V3DA_CLGEN_TFU_MIN_W) || (width > V3DA_CLGEN_TFU_MAX_W) || (height == 0u) ||
			(height > V3DA_CLGEN_TFU_MAX_H) || (src_gpuva == 0u) || (dst_gpuva == 0u) ||
			((src_gpuva & (V3DA_CLGEN_ALIGN - 1u)) != 0u) || ((dst_gpuva & (V3DA_CLGEN_ALIGN - 1u)) != 0u)) {
		return -EINVAL;
	}
	if ((src_size < v3da_clgen_tfu_src_size(width, height)) || (dst_size < v3da_clgen_tfu_out_size(width, height))) {
		return -ENOSPC;
	}

	/* v3dx_tfu.c v3dX(tfu)(), V3D_VERSION == 42, not for_mipmap,
	 * base_level == last_level == 0, src raster, dst UBLINEAR_2_COLUMN. */
	memset(out, 0, sizeof(*out));
	out->ios = (height << 16) | width;
	out->iia = src_gpuva;
	out->ioa = dst_gpuva;
	/* raster source: iis = stride / cpp (pixels) */
	out->iis = width;
	out->icfg = (V3D33_TFU_ICFG_FORMAT_RASTER << V3D33_TFU_ICFG_FORMAT_SHIFT);
	/* exact copy: cpp 4 -> PIPE_FORMAT_R32_FLOAT -> TEXTURE_DATA_FORMAT_R32F */
	out->icfg |= (uint32_t)V3D42_TEXTURE_DATA_FORMAT_R32F << V3D33_TFU_ICFG_TTYPE_SHIFT;
	/* last_level == base_level: no IOA_DIMTW; LINEARTILE(3) + (UB2 - LT) = 5 */
	out->ioa |= (uint32_t)V3D33_TFU_IOA_FORMAT_UBLINEAR_2_COLUMN << V3D33_TFU_IOA_FORMAT_SHIFT;
	/* NUMMM = last_level - base_level = 0; no OPAD (dst is not UIF) */
	out->icfg |= 0u << V3D33_TFU_ICFG_NUMMM_SHIFT;
	/* ica, iua, coef[0..3] = 0: not YUV (coef[0] bit 31 USECOEF clear) */

	return 0;
}


/* ------------------------------------------------------------------------ */
/* 3. csd                                                                    */
/* ------------------------------------------------------------------------ */

/* broadcom/common/v3d_csd.h */
#define V3D_CSD_CFG012_WG_COUNT_SHIFT        16
#define V3D_CSD_CFG3_BATCHES_PER_SG_M1_SHIFT 12
#define V3D_CSD_CFG3_WGS_PER_SG_SHIFT        8
#define V3D_CSD_CFG3_WG_SIZE_SHIFT           0
#define V3D_CSD_CFG5_PROPAGATE_NANS          (1u << 2)
#define V3D_CSD_CFG5_SINGLE_SEG              (1u << 1)
#define V3D_CSD_CFG5_THREADING               (1u << 0)

/* CSCONST from tools/v3d-driver-port/csd_probe.c (v3d-shader-tool output,
 * HW-proven through the old winsys): out[0] = uniform[0] via TMU write.
 * local_size = 16, threads = 4, single_seg = 0, uniforms = {value, SSBO VA}. */
static const uint64_t csconst_code[] = {
	0x3c603186bb800000ull, /* nop ; nop ; thrsw ; ldunif */
	0x3db032c6bbf40000ull, /* nop ; mov tmud, r5 ; thrsw ; ldunifrf.r0 */
	0x3c003306bbe00000ull, /* nop ; mov tmua, r0 */
	0x3c203181bb815000ull, /* tmuwt r1 ; nop ; thrsw */
	0x3c003186bb800000ull, /* nop ; nop */
	0x3c003186bb800000ull, /* nop ; nop */
};

_Static_assert(sizeof(csconst_code) == V3DA_CLGEN_CSD_SHADER_SIZE, "CSCONST size");


int v3da_clgen_csd(v3da_clgen_buf_t *shader, v3da_clgen_buf_t *uniforms, uint32_t out_gpuva, uint32_t cfg[7])
{
	volatile uint32_t *sh, *un;
	uint32_t i;
	int err;

	if ((cfg == NULL) || (out_gpuva == 0u) || ((out_gpuva & 3u) != 0u)) {
		return -EINVAL;
	}
	err = buf_ok(shader, V3DA_CLGEN_CSD_SHADER_SIZE, V3DA_CLGEN_ALIGN);
	if (err == 0) {
		err = buf_ok(uniforms, V3DA_CLGEN_CSD_UNIFORM_SIZE, 4u);
	}
	if (err != 0) {
		return err;
	}

	/* QPU instructions are little-endian 64-bit words: low half first. */
	sh = shader->cpu;
	for (i = 0u; i < sizeof(csconst_code) / sizeof(csconst_code[0]); i++) {
		sh[2u * i] = (uint32_t)csconst_code[i];
		sh[2u * i + 1u] = (uint32_t)(csconst_code[i] >> 32);
	}

	un = uniforms->cpu;
	un[0] = V3DA_CLGEN_CSD_VALUE; /* uniform[0]: the value to store */
	un[1] = out_gpuva;            /* uniform[1]: SSBO base VA */

	/* csd_probe.c STEP2 (s2.cfg): 1x1x1 workgroups of 16 invocations. */
	cfg[0] = 1u << V3D_CSD_CFG012_WG_COUNT_SHIFT;
	cfg[1] = 1u << V3D_CSD_CFG012_WG_COUNT_SHIFT;
	cfg[2] = 1u << V3D_CSD_CFG012_WG_COUNT_SHIFT;
	cfg[3] = (1u << V3D_CSD_CFG3_WGS_PER_SG_SHIFT) | /* wg_size % 16 == 0 -> 1 wg per supergroup */
		(0u << V3D_CSD_CFG3_BATCHES_PER_SG_M1_SHIFT) |
		(16u << V3D_CSD_CFG3_WG_SIZE_SHIFT);
	cfg[4] = 0u; /* num_batches - 1 = DIV_ROUND_UP(16, 16) * 1 - 1 (ver < 71) */
	cfg[5] = shader->gpuva | V3D_CSD_CFG5_PROPAGATE_NANS | V3D_CSD_CFG5_THREADING; /* single_seg = 0 */
	cfg[6] = uniforms->gpuva;

	return 0;
}
