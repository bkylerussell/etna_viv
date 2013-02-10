/*
 * Copyright (c) 2012-2013 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
/* Experiments with alpha blending
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>

#include <errno.h>

#include "etna_pipe.h"

#include "etna/common.xml.h"
#include "etna/state.xml.h"
#include "etna/state_3d.xml.h"
#include "etna/cmdstream.xml.h"

#include "write_bmp.h"
#include "viv.h"
#include "etna.h"
#include "etna_state.h"
#include "etna_rs.h"
#include "etna_fb.h"
#include "etna_bswap.h"
#include "etna_tex.h"

#include "esTransform.h"
#include "dds.h"

/*********************************************************************/
#define VERTEX_BUFFER_SIZE 0x60000

float vVertices[] = {
  -1.0f, -1.0f, +0.0f,
  +1.0f, -1.0f, +0.0f,
  -1.0f, +1.0f, +0.0f,
  +1.0f, +1.0f, +0.0f,
};

float vColors[] = {
  0.8f,  0.8f,  0.8f,
  0.9f,  0.9f,  0.9f,
  1.0f,  1.0f,  1.0f,
  1.0f,  1.0f,  1.0f,
};

float vNormals[] = {
  +0.0f, +0.0f, +1.0f,
  +0.0f, +0.0f, +1.0f,
  +0.0f, +0.0f, +1.0f,
  +0.0f, +0.0f, +1.0f,
};

#define COMPONENTS_PER_VERTEX (3)
#define NUM_VERTICES (8)

uint32_t vs[] = {
    0x07841003,0x39000800,0x00000050,0x00000000,
    0x07841002,0x39001800,0x00aa0050,0x00390048,
    0x07841002,0x39002800,0x01540050,0x00390048,
    0x07841002,0x39003800,0x01fe0050,0x00390048,
    0x07801009,0x00000000,0x00000000,0x00390028,
    0x07801003,0x39000800,0x01c80640,0x00000002,
    0x02041001,0x2a804800,0x00000000,0x003fc048,
    0x02041003,0x2a804800,0x00aa05c0,0x00000002,
};
uint32_t ps[] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000
};
size_t vs_size = sizeof(vs);
size_t ps_size = sizeof(ps);

int main(int argc, char **argv)
{
    int rv;
    int width = 256;
    int height = 256;
    
    fb_info fb;
    rv = fb_open(0, &fb);
    if(rv!=0)
    {
        exit(1);
    }
    width = fb.fb_var.xres;
    height = fb.fb_var.yres;

    rv = viv_open();
    if(rv!=0)
    {
        fprintf(stderr, "Error opening device\n");
        exit(1);
    }
    printf("Succesfully opened device\n");

    etna_ctx *ctx = 0;
    struct pipe_context *pipe = 0;
    etna_bswap_buffers *buffers = 0;
    if(etna_create(&ctx) != ETNA_OK ||
        etna_bswap_create(ctx, &buffers, (int (*)(void *, int))&fb_set_buffer, &fb) != ETNA_OK ||
        (pipe = etna_new_pipe_context(ctx)) == NULL)
    {
        printf("Unable to create etna context\n");
        exit(1);
    }

    /* resources */
    struct pipe_resource *rt_resource = etna_pipe_create_2d(pipe, ETNA_IS_RENDER_TARGET, PIPE_FORMAT_B8G8R8X8_UNORM, width, height, 0);
    struct pipe_resource *z_resource = etna_pipe_create_2d(pipe, ETNA_IS_RENDER_TARGET, PIPE_FORMAT_Z16_UNORM, width, height, 0);
    struct pipe_resource *vtx_resource = etna_pipe_create_buffer(pipe, ETNA_IS_VERTEX, VERTEX_BUFFER_SIZE);

    /* interleave vertex data */
    float *vtx_logical = vtx_resource->levels[0].logical;
    for(int vert=0; vert<NUM_VERTICES; ++vert)
    {
        int dest_idx = vert * (3 + 3 + 3);
        for(int comp=0; comp<3; ++comp)
            vtx_logical[dest_idx+comp+0] = vVertices[vert*3 + comp]; /* 0 */
        for(int comp=0; comp<3; ++comp)
            vtx_logical[dest_idx+comp+3] = vNormals[vert*3 + comp]; /* 1 */
        for(int comp=0; comp<3; ++comp)
            vtx_logical[dest_idx+comp+6] = vColors[vert*3 + comp]; /* 2 */
    }

    /* pre-compile RS states to copy to screen (XXX move this to etna_bswap?) */
    struct compiled_rs_state copy_to_screen[ETNA_BSWAP_NUM_BUFFERS] = {};

    for(int bi=0; bi<ETNA_BSWAP_NUM_BUFFERS; ++bi)
    {
        etna_compile_rs_state(&copy_to_screen[bi], &(struct rs_state){
                    .source_format = RS_FORMAT_X8R8G8B8,
                    .source_tiling = rt_resource->layout,
                    .source_addr = rt_resource->levels[0].address,
                    .source_stride = rt_resource->levels[0].stride,
                    .dest_format = RS_FORMAT_X8R8G8B8,
                    .dest_tiling = ETNA_LAYOUT_LINEAR,
                    .dest_addr = fb.physical[bi],
                    .dest_stride = fb.fb_fix.line_length,
                    .swap_rb = true,
                    .dither = {0xffffffff, 0xffffffff},
                    .clear_mode = VIVS_RS_CLEAR_CONTROL_MODE_DISABLED,
                    .width = width,
                    .height = height
                });
    }

    /* compile gallium3d states */
    void *blend = pipe->create_blend_state(pipe, &(struct pipe_blend_state) {
                .rt[0] = {
                    .blend_enable = 1,
                    .rgb_func = PIPE_BLEND_ADD,
                    .rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA,
                    .rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
                    .alpha_func = PIPE_BLEND_ADD,
                    .alpha_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA,
                    .alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
                    .colormask = 0xf
                }
            });

    void *sampler = pipe->create_sampler_state(pipe, &(struct pipe_sampler_state) {
                .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
                .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
                .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
                .min_img_filter = PIPE_TEX_FILTER_LINEAR,
                .min_mip_filter = PIPE_TEX_MIPFILTER_LINEAR,
                .mag_img_filter = PIPE_TEX_FILTER_LINEAR,
                .normalized_coords = 1,
                .lod_bias = 0.0f,
                .min_lod = 0.0f, .max_lod=1000.0f
            });

    void *rasterizer = pipe->create_rasterizer_state(pipe, &(struct pipe_rasterizer_state){
                .flatshade = 0,
                .light_twoside = 1,
                .clamp_vertex_color = 1,
                .clamp_fragment_color = 1,
                .front_ccw = 1,
                .cull_face = PIPE_FACE_BACK,      /**< PIPE_FACE_x */
                .fill_front = PIPE_POLYGON_MODE_FILL,     /**< PIPE_POLYGON_MODE_x */
                .fill_back = PIPE_POLYGON_MODE_FILL,      /**< PIPE_POLYGON_MODE_x */
                .offset_point = 0,
                .offset_line = 0,
                .offset_tri = 0,
                .scissor = 0,
                .poly_smooth = 1,
                .poly_stipple_enable = 0,
                .point_smooth = 0,
                .sprite_coord_mode = 0,     /**< PIPE_SPRITE_COORD_ */
                .point_quad_rasterization = 0, /** points rasterized as quads or points */
                .point_size_per_vertex = 0, /**< size computed in vertex shader */
                .multisample = 0,
                .line_smooth = 0,
                .line_stipple_enable = 0,
                .line_last_pixel = 0,
                .flatshade_first = 0,
                .gl_rasterization_rules = 1,
                .rasterizer_discard = 0,
                .depth_clip = 0,
                .clip_plane_enable = 0,
                .line_stipple_factor = 0,
                .line_stipple_pattern = 0,
                .sprite_coord_enable = 0,
                .line_width = 1.0f,
                .point_size = 1.0f,
                .offset_units = 0.0f,
                .offset_scale = 0.0f,
                .offset_clamp = 0.0f
            });

    void *dsa = pipe->create_depth_stencil_alpha_state(pipe, &(struct pipe_depth_stencil_alpha_state){
            .depth = {
                .enabled = 0,
                .writemask = 0,
                .func = PIPE_FUNC_LESS /* GL default */
            },
            .stencil[0] = {
                .enabled = 0
            },
            .stencil[1] = {
                .enabled = 0
            },
            .alpha = {
                .enabled = 0
            }
            });

    struct pipe_vertex_buffer vertex_buf_desc = {
            .stride = (3 + 3 + 3)*4,
            .buffer_offset = 0,
            .buffer = vtx_resource,
            .user_buffer = 0
            };
    
    struct pipe_vertex_element pipe_vertex_elements[] = {
        { /* positions */
            .src_offset = 0,
            .instance_divisor = 0,
            .vertex_buffer_index = 0,
            .src_format = PIPE_FORMAT_R32G32B32_FLOAT 
        },
        { /* normals */
            .src_offset = 0xc,
            .instance_divisor = 0,
            .vertex_buffer_index = 0,
            .src_format = PIPE_FORMAT_R32G32B32_FLOAT 
        },
        { /* texture coord */
            .src_offset = 0x18,
            .instance_divisor = 0,
            .vertex_buffer_index = 0,
            .src_format = PIPE_FORMAT_R32G32B32_FLOAT
        }
    };
    void *vertex_elements = pipe->create_vertex_elements_state(pipe, 
            sizeof(pipe_vertex_elements)/sizeof(pipe_vertex_elements[0]), pipe_vertex_elements);
    struct pipe_surface *cbuf = pipe->create_surface(pipe, rt_resource, &(struct pipe_surface){
        .texture = rt_resource,
        .format = rt_resource->format,
        .u.tex.level = 0
        });
    struct pipe_surface *zsbuf = pipe->create_surface(pipe, z_resource, &(struct pipe_surface){
        .texture = z_resource,
        .format = z_resource->format,
        .u.tex.level = 0
        });
    
    /* bind */
    pipe->bind_blend_state(pipe, blend);
    pipe->bind_fragment_sampler_states(pipe, 1, &sampler);
    pipe->bind_rasterizer_state(pipe, rasterizer);
    pipe->bind_depth_stencil_alpha_state(pipe, dsa);
    pipe->bind_vertex_elements_state(pipe, vertex_elements);

    pipe->set_blend_color(pipe, &(struct pipe_blend_color){
            .color = {0.0f,0.0f,0.0f,1.0f}
            });
    pipe->set_stencil_ref(pipe, &(struct pipe_stencil_ref){
            .ref_value[0] = 0xff,
            .ref_value[1] = 0xff
            });
    pipe->set_sample_mask(pipe, 0xf);
    pipe->set_framebuffer_state(pipe, &(struct pipe_framebuffer_state){
            .width = width,
            .height = height,
            .nr_cbufs = 1,
            .cbufs[0] = cbuf,
            .zsbuf = zsbuf
            });
    pipe->set_scissor_state(pipe, &(struct pipe_scissor_state){
            .minx = 0,
            .miny = 0,
            .maxx = 65535,
            .maxy = 65535
            });
    pipe->set_viewport_state(pipe, &(struct pipe_viewport_state){
            .scale = {width/2.0f, height/2.0f, 0.5f, 1.0f},
            .translate = {width/2.0f, height/2.0f, 0.5f, 1.0f}
            });

    pipe->set_vertex_buffers(pipe, 0, 1, &vertex_buf_desc);
    pipe->set_index_buffer(pipe, NULL);/*&(struct pipe_index_buffer){
            .index_size = 0,
            .offset = 0,
            .buffer = 0,
            .user_buffer = 0
            });*/ /* non-indexed rendering */

    for(int frame=0; frame<1000; ++frame)
    {
        if(frame%50 == 0)
            printf("*** FRAME %i ****\n", frame);

        /*   Compute transform matrices in the same way as cube egl demo */ 
        ESMatrix projection;
        GLfloat aspect = (GLfloat)(height) / (GLfloat)(width);
        esMatrixLoadIdentity(&projection);
        esFrustum(&projection, -2.8f, +2.8f, -2.8f * aspect, +2.8f * aspect, 5.0f, 10.0f);

        /* Clear render target */
        pipe->clear(pipe, PIPE_CLEAR_COLOR | PIPE_CLEAR_DEPTHSTENCIL, &(const union pipe_color_union) {
                .f = {0.2, 0.2, 0.2, 1.0}
                }, 1.0, 0xff);
        
        /* shaders etc, not yet molded into gallium state */
        etna_set_state(ctx, VIVS_GL_VARYING_NUM_COMPONENTS,  
                VIVS_GL_VARYING_NUM_COMPONENTS_VAR0(4)
                );
        etna_set_state(ctx, VIVS_GL_VARYING_TOTAL_COMPONENTS,
                VIVS_GL_VARYING_TOTAL_COMPONENTS_NUM(4)
                );
        etna_set_state_multi(ctx, VIVS_GL_VARYING_COMPONENT_USE(0), 2, (uint32_t[]){
                VIVS_GL_VARYING_COMPONENT_USE_COMP0(VARYING_COMPONENT_USE_USED) |
                VIVS_GL_VARYING_COMPONENT_USE_COMP1(VARYING_COMPONENT_USE_USED) |
                VIVS_GL_VARYING_COMPONENT_USE_COMP2(VARYING_COMPONENT_USE_USED) |
                VIVS_GL_VARYING_COMPONENT_USE_COMP3(VARYING_COMPONENT_USE_USED)
                , 0
                });

        etna_set_state(ctx, VIVS_RA_CONTROL, 0x1);
        
        etna_set_state(ctx, VIVS_PA_ATTRIBUTE_ELEMENT_COUNT, 
                VIVS_PA_ATTRIBUTE_ELEMENT_COUNT_COUNT(1));
        etna_set_state(ctx, VIVS_PA_SHADER_ATTRIBUTES(0), 0x200);

        etna_set_state(ctx, VIVS_VS_START_PC, 0x0);
        etna_set_state(ctx, VIVS_VS_END_PC, vs_size/16);
        etna_set_state_multi(ctx, VIVS_VS_INPUT_COUNT, 3, (uint32_t[]){
                /* VIVS_VS_INPUT_COUNT */ VIVS_VS_INPUT_COUNT_UNK8(1) | VIVS_VS_INPUT_COUNT_COUNT(3),
                /* VIVS_VS_TEMP_REGISTER_CONTROL */ VIVS_VS_TEMP_REGISTER_CONTROL_NUM_TEMPS(6),
                /* VIVS_VS_OUTPUT(0) */ VIVS_VS_OUTPUT_O0(4) | VIVS_VS_OUTPUT_O1(0)});
        etna_set_state(ctx, VIVS_VS_OUTPUT_COUNT, 2);
        etna_set_state(ctx, VIVS_VS_LOAD_BALANCING, 0xf3f0582); /* depends on number of inputs/outputs/varyings? XXX how exactly */
        etna_set_state(ctx, VIVS_VS_INPUT(0), VIVS_VS_INPUT_I0(0) | 
                                        VIVS_VS_INPUT_I1(1) | 
                                        VIVS_VS_INPUT_I2(2));

        etna_set_state(ctx, VIVS_PS_START_PC, 0x0);
        etna_set_state_multi(ctx, VIVS_PS_END_PC, 2, (uint32_t[]){
                /* VIVS_PS_END_PC */ ps_size/16,
                /* VIVS_PS_OUTPUT_REG */ 0x1});
        etna_set_state(ctx, VIVS_PS_INPUT_COUNT, VIVS_PS_INPUT_COUNT_UNK8(31) | VIVS_PS_INPUT_COUNT_COUNT(2));
        etna_set_state(ctx, VIVS_PS_TEMP_REGISTER_CONTROL, VIVS_PS_TEMP_REGISTER_CONTROL_NUM_TEMPS(2));
        etna_set_state(ctx, VIVS_PS_CONTROL, VIVS_PS_CONTROL_UNK1);
        
        etna_set_state_multi(ctx, VIVS_VS_INST_MEM(0), vs_size/4, vs);
        etna_set_state_multi(ctx, VIVS_PS_INST_MEM(0), ps_size/4, ps);

        etna_set_state_f32(ctx, VIVS_VS_UNIFORMS(19), 2.0); /* u4.w */
        etna_set_state_f32(ctx, VIVS_VS_UNIFORMS(23), 20.0); /* u5.w */
        etna_set_state_f32(ctx, VIVS_VS_UNIFORMS(27), 0.0); /* u6.w */
        etna_set_state_f32(ctx, VIVS_VS_UNIFORMS(44), 1.0); /* u11.x */
        etna_set_state_f32(ctx, VIVS_VS_UNIFORMS(45), 0.5); /* u11.y */

        etna_set_state_f32(ctx, VIVS_PS_UNIFORMS(0), 1.0); /* u0.x */

        for(int idx=0; idx<5; ++idx)
        {
            ESMatrix modelview, modelviewprojection;
            esMatrixLoadIdentity(&modelview);

            esTranslate(&modelview, 0.0f, 0.0f, -7.0f);
            //esRotate(&modelview, 25.0f, 1.0f, 0.0f, 0.0f);
            esRotate(&modelview, 35.0f, 0.0f, 1.0f, 0.0f);
            esRotate(&modelview, frame*0.15f, 1.0f, 0.0f, 0.0f);
            esTranslate(&modelview, 0.0f, 0.0f, 0.3f * idx);

            esMatrixLoadIdentity(&modelviewprojection);
            esMatrixMultiply(&modelviewprojection, &modelview, &projection);
        
            etna_set_state_multi(ctx, VIVS_VS_UNIFORMS(0), 16, (uint32_t*)&modelviewprojection.m[0][0]);
            etna_set_state_multi(ctx, VIVS_VS_UNIFORMS(28), 16, (uint32_t*)&modelview.m[0][0]);
            etna_set_state_multi(ctx, VIVS_VS_UNIFORMS(48), 16, (uint32_t*)(float[]) /* material color */
                 {idx*0.25f, 0.3f, 1.0f - idx*0.25f, 0.5f}); /* u12 */
        
            pipe->draw_vbo(pipe, &(struct pipe_draw_info){
                    .indexed = 0,
                    .mode = PIPE_PRIM_TRIANGLE_STRIP,
                    .start = 0,
                    .count = 4
                    });
        }
        
        /* copy to screen */
        etna_bswap_wait_available(buffers);
        /*  this flush is really needed, otherwise some quads will have pieces undrawn */
        etna_set_state(ctx, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_COLOR | VIVS_GL_FLUSH_CACHE_DEPTH);
        /*  assumes TS is still set up correctly */
        etna_submit_rs_state(ctx, &copy_to_screen[buffers->backbuffer]);

        //etna_dump_cmd_buffer(ctx);
        //exit(0);
        etna_bswap_queue_swap(buffers);
    }
#ifdef DUMP
    bmp_dump32(fb.logical[1-backbuffer], width, height, false, "/mnt/sdcard/fb.bmp");
    printf("Dump complete\n");
#endif
    etna_bswap_free(buffers);
    etna_free(ctx);
    viv_close();
    return 0;
}

