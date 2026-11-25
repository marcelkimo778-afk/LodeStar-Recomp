// Recorder-thread interface of totk-pc-recomp's vk_rec.c, reduced to direct calls:
// this port records every vkCmd* on the replaying thread (the original's
// TOTK_VK_RECORDER=0 path).  Same names, so the back end needs no edits.
#pragma once

static inline void rec_begin(VkCommandBuffer cb, uint64_t fence) { (void)cb; (void)fence; }
static inline void rec_end(void) {}
static inline int rec_end_cb(VkCommandBuffer cb, int crumb, uint64_t n) { (void)cb; (void)crumb; (void)n; return 0; }
static inline void rec_drain(void) {}
static inline void rec_stats_log(void) {}
enum { REC_IMAGE, REC_VIEW, REC_BUFFER, REC_KINDS };
static inline void rec_check_handle(int kind, uint64_t handle, int destroy, uint64_t tl, uint64_t gpu)
{ (void)kind; (void)handle; (void)destroy; (void)tl; (void)gpu; }
static inline void rec_check_log(void) {}

static inline void rec_push_tpl(PFN_vkCmdPushDescriptorSetWithTemplateKHR fn, VkCommandBuffer cb,
                                VkDescriptorUpdateTemplate tpl, VkPipelineLayout layout, uint32_t set,
                                const void *data, uint32_t bytes)
{ (void)bytes; fn(cb, tpl, layout, set, data); }
static inline void rec_push(PFN_vkCmdPushDescriptorSetKHR fn, VkCommandBuffer cb, VkPipelineBindPoint bp,
                            VkPipelineLayout layout, uint32_t set, uint32_t n, const VkWriteDescriptorSet *w)
{ fn(cb, bp, layout, set, n, w); }
static inline void rec_vertex_input(PFN_vkCmdSetVertexInputEXT fn, VkCommandBuffer cb, uint32_t nb,
                                    const VkVertexInputBindingDescription2EXT *b, uint32_t na,
                                    const VkVertexInputAttributeDescription2EXT *a)
{ fn(cb, nb, b, na, a); }
static inline void rec_bool(PFN_vkCmdSetDepthClampEnableEXT fn, VkCommandBuffer cb, VkBool32 v) { fn(cb, v); }
static inline void rec_polygon_mode(PFN_vkCmdSetPolygonModeEXT fn, VkCommandBuffer cb, VkPolygonMode m) { fn(cb, m); }
static inline void rec_blend_enable(PFN_vkCmdSetColorBlendEnableEXT fn, VkCommandBuffer cb, uint32_t first,
                                    uint32_t n, const VkBool32 *v) { fn(cb, first, n, v); }
static inline void rec_blend_equation(PFN_vkCmdSetColorBlendEquationEXT fn, VkCommandBuffer cb, uint32_t first,
                                      uint32_t n, const VkColorBlendEquationEXT *v) { fn(cb, first, n, v); }
static inline void rec_write_mask(PFN_vkCmdSetColorWriteMaskEXT fn, VkCommandBuffer cb, uint32_t first, uint32_t n,
                                  const VkColorComponentFlags *v) { fn(cb, first, n, v); }
static inline void rec_pass_mark(VkCommandBuffer cb, uint32_t id, int end) { (void)cb; (void)id; (void)end; }
