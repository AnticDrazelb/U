/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include "ui/xemu-settings.h"
#include "renderer.h"
#include "hw/xbox/nv2a/xb_profile.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

#include <assert.h>
#include <glslang/Include/glslang_c_interface.h>
#include <stdio.h>

static const glslang_resource_t
    resource_limits = { .max_lights = 32,
                        .max_clip_planes = 6,
                        .max_texture_units = 32,
                        .max_texture_coords = 32,
                        .max_vertex_attribs = 64,
                        .max_vertex_uniform_components = 4096,
                        .max_varying_floats = 64,
                        .max_vertex_texture_image_units = 32,
                        .max_combined_texture_image_units = 80,
                        .max_texture_image_units = 32,
                        .max_fragment_uniform_components = 4096,
                        .max_draw_buffers = 32,
                        .max_vertex_uniform_vectors = 128,
                        .max_varying_vectors = 8,
                        .max_fragment_uniform_vectors = 16,
                        .max_vertex_output_vectors = 16,
                        .max_fragment_input_vectors = 15,
                        .min_program_texel_offset = -8,
                        .max_program_texel_offset = 7,
                        .max_clip_distances = 8,
                        .max_compute_work_group_count_x = 65535,
                        .max_compute_work_group_count_y = 65535,
                        .max_compute_work_group_count_z = 65535,
                        .max_compute_work_group_size_x = 1024,
                        .max_compute_work_group_size_y = 1024,
                        .max_compute_work_group_size_z = 64,
                        .max_compute_uniform_components = 1024,
                        .max_compute_texture_image_units = 16,
                        .max_compute_image_uniforms = 8,
                        .max_compute_atomic_counters = 8,
                        .max_compute_atomic_counter_buffers = 1,
                        .max_varying_components = 60,
                        .max_vertex_output_components = 64,
                        .max_geometry_input_components = 64,
                        .max_geometry_output_components = 128,
                        .max_fragment_input_components = 128,
                        .max_image_units = 8,
                        .max_combined_image_units_and_fragment_outputs = 8,
                        .max_combined_shader_output_resources = 8,
                        .max_image_samples = 0,
                        .max_vertex_image_uniforms = 0,
                        .max_tess_control_image_uniforms = 0,
                        .max_tess_evaluation_image_uniforms = 0,
                        .max_geometry_image_uniforms = 0,
                        .max_fragment_image_uniforms = 8,
                        .max_combined_image_uniforms = 8,
                        .max_geometry_texture_image_units = 16,
                        .max_geometry_output_vertices = 256,
                        .max_geometry_total_output_components = 1024,
                        .max_geometry_uniform_components = 1024,
                        .max_geometry_varying_components = 64,
                        .max_tess_control_input_components = 128,
                        .max_tess_control_output_components = 128,
                        .max_tess_control_texture_image_units = 16,
                        .max_tess_control_uniform_components = 1024,
                        .max_tess_control_total_output_components = 4096,
                        .max_tess_evaluation_input_components = 128,
                        .max_tess_evaluation_output_components = 128,
                        .max_tess_evaluation_texture_image_units = 16,
                        .max_tess_evaluation_uniform_components = 1024,
                        .max_tess_patch_components = 120,
                        .max_patch_vertices = 32,
                        .max_tess_gen_level = 64,
                        .max_viewports = 16,
                        .max_vertex_atomic_counters = 0,
                        .max_tess_control_atomic_counters = 0,
                        .max_tess_evaluation_atomic_counters = 0,
                        .max_geometry_atomic_counters = 0,
                        .max_fragment_atomic_counters = 8,
                        .max_combined_atomic_counters = 8,
                        .max_atomic_counter_bindings = 1,
                        .max_vertex_atomic_counter_buffers = 0,
                        .max_tess_control_atomic_counter_buffers = 0,
                        .max_tess_evaluation_atomic_counter_buffers = 0,
                        .max_geometry_atomic_counter_buffers = 0,
                        .max_fragment_atomic_counter_buffers = 1,
                        .max_combined_atomic_counter_buffers = 1,
                        .max_atomic_counter_buffer_size = 16384,
                        .max_transform_feedback_buffers = 4,
                        .max_transform_feedback_interleaved_components = 64,
                        .max_cull_distances = 8,
                        .max_combined_clip_and_cull_distances = 8,
                        .max_samples = 4,
                        .max_mesh_output_vertices_nv = 256,
                        .max_mesh_output_primitives_nv = 512,
                        .max_mesh_work_group_size_x_nv = 32,
                        .max_mesh_work_group_size_y_nv = 1,
                        .max_mesh_work_group_size_z_nv = 1,
                        .max_task_work_group_size_x_nv = 32,
                        .max_task_work_group_size_y_nv = 1,
                        .max_task_work_group_size_z_nv = 1,
                        .max_mesh_view_count_nv = 4,
                        .maxDualSourceDrawBuffersEXT = 1,
                        .limits = {
                            .non_inductive_for_loops = 1,
                            .while_loops = 1,
                            .do_while_loops = 1,
                            .general_uniform_indexing = 1,
                            .general_attribute_matrix_vector_indexing = 1,
                            .general_varying_indexing = 1,
                            .general_sampler_indexing = 1,
                            .general_variable_indexing = 1,
                            .general_constant_matrix_vector_indexing = 1,
                        } };

void pgraph_vk_init_glsl_compiler(void)
{
    glslang_initialize_process();
}

void pgraph_vk_finalize_glsl_compiler(void)
{
    glslang_finalize_process();
}

GByteArray *pgraph_vk_compile_glsl_to_spv(PGRAPHVkState *r,
                                          glslang_stage_t stage,
                                          const char *glsl_source)
{
    glslang_target_client_version_t client_version;
    glslang_target_language_version_t target_spv;

#ifdef VK_API_VERSION_1_4
    if (r->vk_api_version >= VK_API_VERSION_1_4) {
        client_version = GLSLANG_TARGET_VULKAN_1_4;
        target_spv = GLSLANG_TARGET_SPV_1_6;
    } else
#endif
#ifdef VK_API_VERSION_1_3
    if (r->vk_api_version >= VK_API_VERSION_1_3) {
        client_version = GLSLANG_TARGET_VULKAN_1_3;
        target_spv = GLSLANG_TARGET_SPV_1_6;
    } else
#endif
    if (r->vk_api_version >= VK_API_VERSION_1_2) {
        client_version = GLSLANG_TARGET_VULKAN_1_2;
        target_spv = GLSLANG_TARGET_SPV_1_5;
    } else {
        client_version = GLSLANG_TARGET_VULKAN_1_1;
        target_spv = GLSLANG_TARGET_SPV_1_3;
    }

    const glslang_input_t input = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = stage,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = client_version,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = target_spv,
        .code = glsl_source,
        .default_version = 460,
        .default_profile = GLSLANG_NO_PROFILE,
        .force_default_version_and_profile = false,
        .forward_compatible = false,
        .messages = GLSLANG_MSG_DEFAULT_BIT,
        .resource = &resource_limits,
    };

    glslang_shader_t *shader = glslang_shader_create(&input);

    if (!glslang_shader_preprocess(shader, &input)) {
        fprintf(stderr,
                "GLSL preprocessing failed\n"
                "[INFO]: %s\n"
                "[DEBUG]: %s\n"
                "%s\n",
                glslang_shader_get_info_log(shader),
                glslang_shader_get_info_debug_log(shader), input.code);
        assert(!"glslang preprocess failed");
        glslang_shader_delete(shader);
        return NULL;
    }

    if (!glslang_shader_parse(shader, &input)) {
        fprintf(stderr,
                "GLSL parsing failed\n"
                "[INFO]: %s\n"
                "[DEBUG]: %s\n"
                "%s\n",
                glslang_shader_get_info_log(shader),
                glslang_shader_get_info_debug_log(shader),
                glslang_shader_get_preprocessed_code(shader));
        assert(!"glslang parse failed");
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_program_t *program = glslang_program_create();
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT |
                                           GLSLANG_MSG_VULKAN_RULES_BIT)) {
        fprintf(stderr,
                "GLSL linking failed\n"
                "[INFO]: %s\n"
                "[DEBUG]: %s\n",
                glslang_program_get_info_log(program),
                glslang_program_get_info_debug_log(program));
        assert(!"glslang link failed");
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_spv_options_t spv_options = {
        .validate = true,
    };

    if (g_config.display.vulkan.debug_shaders) {
        spv_options.disable_optimizer = true;
        spv_options.generate_debug_info = true;
        spv_options.emit_nonsemantic_shader_debug_info = true;
        spv_options.emit_nonsemantic_shader_debug_source = true;

        // XXX: Note emit_nonsemantic_shader_debug_source actually does nothing
        // as of 2024.07.25. To actually get glsl source embedded in spv, we
        // must do the following...
        //
        // ref: https://github.com/KhronosGroup/glslang/issues/3252
        glslang_program_add_source_text(program, input.stage, input.code,
                                        strlen(input.code));
    }
    glslang_program_SPIRV_generate_with_options(program, stage, &spv_options);

    const char *spirv_messages = glslang_program_SPIRV_get_messages(program);
    if (spirv_messages) {
        printf("%s\b", spirv_messages);
    }

    size_t num_program_bytes =
        glslang_program_SPIRV_get_size(program) * sizeof(uint32_t);

    guint8 *data = g_malloc(num_program_bytes);
    glslang_program_SPIRV_get(program, (unsigned int *)data);

    glslang_program_delete(program);
    glslang_shader_delete(shader);

    return g_byte_array_new_take(data, num_program_bytes);
}

VkShaderModule pgraph_vk_create_shader_module_from_spv(PGRAPHVkState *r, GByteArray *spv)
{
    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spv->len,
        .pCode = (uint32_t *)spv->data,
    };
    VkShaderModule module;
    VK_CHECK(
        vkCreateShaderModule(r->device, &create_info, NULL, &module));
    return module;
}

static void block_to_uniforms(const SpvReflectBlockVariable *block, ShaderUniformLayout *layout)
{
    assert(!layout->uniforms);

    layout->num_uniforms = block->member_count;
    layout->uniforms = g_malloc0_n(block->member_count, sizeof(ShaderUniform));
    layout->total_size = block->size;
    layout->allocation = g_malloc0(block->size);

    for (uint32_t k = 0; k < block->member_count; ++k) {
        const SpvReflectBlockVariable *member = &block->members[k];

        assert(member->array.dims_count < 2);

        int dim = 1;
        for (int i = 0; i < member->array.dims_count; i++) {
            dim *= member->array.dims[i];
        }
        int stride = MAX(member->array.stride, member->numeric.matrix.stride);
        if (member->numeric.matrix.column_count) {
            dim *= member->numeric.matrix.column_count;
            if (member->array.stride) {
                stride =
                    member->array.stride / member->numeric.matrix.column_count;
            }
        }
        layout->uniforms[k] = (ShaderUniform){
            .name = strdup(member->name),
            .offset = member->offset,
            .dim_v = MAX(1, member->numeric.vector.component_count),
            .dim_a = dim,
            .stride = stride,
        };

        // fprintf(stderr, "<%s offset=%zd dim_v=%zd dim_a=%zd stride=%zd>\n",
        //     layout->uniforms[k].name,
        //     layout->uniforms[k].offset,
        //     layout->uniforms[k].dim_v,
        //     layout->uniforms[k].dim_a,
        //     layout->uniforms[k].stride
        //     );
    }
    // fprintf(stderr, "--\n");
}

static void init_layout_from_spv(ShaderModuleInfo *info)
{
    SpvReflectResult result = spvReflectCreateShaderModule(
        info->spirv->len, info->spirv->data, &info->reflect_module);
    assert(result == SPV_REFLECT_RESULT_SUCCESS &&
           "Failed to create SPIR-V shader module");

    uint32_t descriptor_set_count = 0;
    result = spvReflectEnumerateDescriptorSets(&info->reflect_module,
                                               &descriptor_set_count, NULL);
    assert(result == SPV_REFLECT_RESULT_SUCCESS &&
           "Failed to enumerate descriptor sets");

    info->descriptor_sets =
        g_malloc_n(descriptor_set_count, sizeof(SpvReflectDescriptorSet *));
    result = spvReflectEnumerateDescriptorSets(
        &info->reflect_module, &descriptor_set_count, info->descriptor_sets);
    assert(result == SPV_REFLECT_RESULT_SUCCESS &&
           "Failed to enumerate descriptor sets");

    info->uniforms.num_uniforms = 0;
    info->uniforms.uniforms = NULL;

    for (uint32_t i = 0; i < descriptor_set_count; ++i) {
        const SpvReflectDescriptorSet *descriptor_set =
            info->descriptor_sets[i];
        for (uint32_t j = 0; j < descriptor_set->binding_count; ++j) {
            const SpvReflectDescriptorBinding *binding =
                descriptor_set->bindings[j];
            if (binding->descriptor_type !=
                SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                continue;
            }

            const SpvReflectBlockVariable *block = &binding->block;
            block_to_uniforms(block, &info->uniforms);
        }
    }

    info->push_constants.num_uniforms = 0;
    info->push_constants.uniforms = NULL;
    assert(info->reflect_module.push_constant_block_count < 2);
    if (info->reflect_module.push_constant_block_count) {
        block_to_uniforms(&info->reflect_module.push_constant_blocks[0],
                          &info->push_constants);
    }
}

static glslang_stage_t vk_shader_stage_to_glslang_stage(VkShaderStageFlagBits stage)
{
    switch (stage) {
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return GLSLANG_STAGE_GEOMETRY;
    case VK_SHADER_STAGE_VERTEX_BIT:
        return GLSLANG_STAGE_VERTEX;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        return GLSLANG_STAGE_FRAGMENT;
    case VK_SHADER_STAGE_COMPUTE_BIT:
        return GLSLANG_STAGE_COMPUTE;
    default:
        assert(0);
    }
}

/* xbox-core: the shaders made, kept on the disk by their GLSL (and stage): a game's shaders are made
 * once, not each time it runs (glslang on the PS4's cores takes tens of milliseconds a shader). The
 * folder from xbox-core (nv2a.cpp); NULL: none. */
const char *xb_shader_cache_dir(void);

/* All of them in one file (shaders.pack: "XBSPV1\0\0", then records: the key's two halves, the stage,
 * the size, the SPIR-V), read once at the first lookup and appended to as shaders are made. The PS4's
 * drive is a hard disk: a file a shader cost a seek each (about 28 ms, a hitch whenever a scene first
 * showed an effect). Single .spv files from before are read once and moved into the pack. */
typedef struct XbSpvEntry {
    uint64_t high, low;
    int stage;
    GByteArray *spv;
    struct XbSpvEntry *next;
} XbSpvEntry;
static XbSpvEntry *xb_spv_table[4096];
static pthread_mutex_t xb_spv_lock = PTHREAD_MUTEX_INITIALIZER;
static bool xb_spv_loaded;
static FILE *xb_spv_pack;

static GByteArray *xb_spv_copy(const GByteArray *a)
{
    uint8_t *data = malloc(a->len);
    if (!data) {
        return NULL;
    }
    memcpy(data, a->data, a->len);
    return g_byte_array_new_take(data, a->len);
}

static void xb_spv_insert(uint64_t high, uint64_t low, int stage, GByteArray *spv)
{
    XbSpvEntry *e = g_malloc(sizeof(*e));
    e->high = high, e->low = low, e->stage = stage, e->spv = spv;
    const unsigned slot = (unsigned)(low ^ (uint64_t)stage) & 4095;
    e->next = xb_spv_table[slot];
    xb_spv_table[slot] = e;
}

static XbSpvEntry *xb_spv_find(uint64_t high, uint64_t low, int stage)
{
    for (XbSpvEntry *e = xb_spv_table[(unsigned)(low ^ (uint64_t)stage) & 4095]; e; e = e->next) {
        if (e->high == high && e->low == low && e->stage == stage) {
            return e;
        }
    }
    return NULL;
}

static void xb_spv_load_pack(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/shaders.pack", dir);
    FILE *f = fopen(path, "rb");
    long good_end = 8;
    int count = 0;
    if (f) {
        char magic[8];
        if (fread(magic, 1, 8, f) == 8 && memcmp(magic, "XBSPV1\0\0", 8) == 0) {
            for (;;) {
                uint64_t key[2];
                int32_t stage;
                uint32_t size;
                if (fread(key, 8, 2, f) != 2 || fread(&stage, 4, 1, f) != 1 || fread(&size, 4, 1, f) != 1 ||
                    size < 20 || size > (64u << 20) || size % 4) {
                    break;
                }
                uint8_t *data = malloc(size);
                if (!data || fread(data, 1, size, f) != size) {
                    free(data);
                    break; /* a record cut short (the app ended while writing it): dropped */
                }
                xb_spv_insert(key[0], key[1], stage, g_byte_array_new_take(data, size));
                good_end = ftell(f);
                count++;
            }
        } else {
            good_end = 0;
        }
        fclose(f);
    } else {
        good_end = 0;
    }
    /* appended to from where the good records end (a cut-short one overwritten) */
    xb_spv_pack = fopen(path, good_end ? "r+b" : "wb");
    if (xb_spv_pack) {
        if (good_end) {
            fseek(xb_spv_pack, good_end, SEEK_SET);
        } else {
            fwrite("XBSPV1\0\0", 1, 8, xb_spv_pack);
            fflush(xb_spv_pack);
        }
    }
    fprintf(stderr, "xemu: shader cache: %d shaders from %s\n", count, path);
}

static void xb_spv_append(uint64_t high, uint64_t low, int stage, const GByteArray *spv)
{
    if (!xb_spv_pack || !spv) {
        return;
    }
    const uint64_t key[2] = {high, low};
    const int32_t st = stage;
    const uint32_t size = spv->len;
    fwrite(key, 8, 2, xb_spv_pack);
    fwrite(&st, 4, 1, xb_spv_pack);
    fwrite(&size, 4, 1, xb_spv_pack);
    fwrite(spv->data, 1, size, xb_spv_pack);
    fflush(xb_spv_pack);
}

/* the SPIR-V for this GLSL and stage, or NULL (*key set for xb_spv_to_cache) */
static GByteArray *xb_spv_from_cache(int stage, const char *glsl, uint64_t key[2])
{
    const char *dir = xb_shader_cache_dir();
    key[0] = key[1] = 0;
    if (!dir) {
        return NULL;
    }
    const XXH128_hash_t h = XXH3_128bits(glsl, strlen(glsl));
    key[0] = h.high64, key[1] = h.low64;
    pthread_mutex_lock(&xb_spv_lock);
    if (!xb_spv_loaded) {
        xb_spv_loaded = true;
        xb_spv_load_pack(dir);
    }
    XbSpvEntry *e = xb_spv_find(h.high64, h.low64, stage);
    GByteArray *spv = NULL;
    if (e) {
        spv = xb_spv_copy(e->spv);
    }
    pthread_mutex_unlock(&xb_spv_lock);
    if (spv) {
        return spv;
    }
    /* a single file from before the pack: read once, moved into it */
    char path[512];
    snprintf(path, sizeof(path), "%s/%016llx%016llx-%d.spv", dir, (unsigned long long)h.high64,
             (unsigned long long)h.low64, stage);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        const long size = ftell(f);
        if (size >= 20 && size % 4 == 0 && fseek(f, 0, SEEK_SET) == 0) {
            uint8_t *data = malloc((size_t)size);
            uint32_t magic = 0;
            if (data && fread(data, 1, (size_t)size, f) == (size_t)size &&
                (memcpy(&magic, data, 4), magic == 0x07230203u)) {
                spv = g_byte_array_new_take(data, (gsize)size);
            } else {
                free(data);
            }
        }
    }
    fclose(f);
    if (spv) {
        pthread_mutex_lock(&xb_spv_lock);
        GByteArray *copy = xb_spv_copy(spv);
        if (copy) {
            xb_spv_insert(h.high64, h.low64, stage, copy);
        }
        xb_spv_append(h.high64, h.low64, stage, spv);
        pthread_mutex_unlock(&xb_spv_lock);
        remove(path);
    }
    return spv;
}

static void xb_spv_to_cache(const uint64_t key[2], int stage, const GByteArray *spv)
{
    if ((!key[0] && !key[1]) || !spv) {
        return;
    }
    pthread_mutex_lock(&xb_spv_lock);
    if (!xb_spv_find(key[0], key[1], stage)) {
        GByteArray *copy = xb_spv_copy(spv);
        if (copy) {
            xb_spv_insert(key[0], key[1], stage, copy);
        }
        xb_spv_append(key[0], key[1], stage, spv);
    }
    pthread_mutex_unlock(&xb_spv_lock);
}

ShaderModuleInfo *pgraph_vk_create_shader_module_from_glsl(
    PGRAPHVkState *r, VkShaderStageFlagBits stage, const char *glsl)
{
    ShaderModuleInfo *info = g_malloc0(sizeof(*info));
    info->refcnt = 0;
    info->glsl = strdup(glsl);
    const uint64_t xb_t0 = xb_prof_now();
    uint64_t cache_key[2];
    info->spirv = xb_spv_from_cache((int)stage, glsl, cache_key);
    if (info->spirv) {
        xb_prof_add(XB_PROF_SPV_CACHED, xb_t0);
    } else {
        info->spirv = pgraph_vk_compile_glsl_to_spv(
            r, vk_shader_stage_to_glslang_stage(stage), glsl);
        xb_prof_add(XB_PROF_GLSL, xb_t0);
        xb_spv_to_cache(cache_key, (int)stage, info->spirv);
    }
    info->module = pgraph_vk_create_shader_module_from_spv(r, info->spirv);
    init_layout_from_spv(info);
    return info;
}

static void finalize_uniform_layout(ShaderUniformLayout *layout)
{
    for (int i = 0; i < layout->num_uniforms; i++) {
        free((void*)layout->uniforms[i].name);
    }
    if (layout->uniforms) {
        g_free(layout->uniforms);
    }
}

void pgraph_vk_ref_shader_module(ShaderModuleInfo *info)
{
    info->refcnt++;
}

void pgraph_vk_unref_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info)
{
    assert(info->refcnt >= 1);

    info->refcnt--;
    if (info->refcnt == 0) {
        pgraph_vk_destroy_shader_module(r, info);
    }
}

void pgraph_vk_destroy_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info)
{
    assert(info->refcnt == 0);
    if (info->glsl) {
        free(info->glsl);
    }
    finalize_uniform_layout(&info->uniforms);
    finalize_uniform_layout(&info->push_constants);
    free(info->descriptor_sets);
    spvReflectDestroyShaderModule(&info->reflect_module);
    vkDestroyShaderModule(r->device, info->module, NULL);
    g_byte_array_unref(info->spirv);
    g_free(info);
}
