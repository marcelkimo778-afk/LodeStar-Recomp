/* NVN procs that Pokémon Legends: Arceus asks for and the ported front end
 * (written for Tears of the Kingdom) does not implement.  Same conventions as
 * nvn_api_*.c: objects live in place in the game's NVN object memory
 * (nvn_obj.h layouts), arguments are read with arg_x / arg_p. */
#include <stdint.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"

/* size_t nvnBufferBuilderGetSize(const NVNbufferBuilder *b) */
NVN_API(nvnBufferBuilderGetSize) { ret_x(c, ((const buf_builder_t *)arg_p(c, 0))->size); }

/* int nvnTexturePoolGetSize(const NVNtexturePool *p) / nvnSamplerPoolGetSize */
NVN_API(nvnTexturePoolGetSize) { ret_x(c, (uint64_t)(int64_t)((const dpool_t *)arg_p(c, 0))->n); }
NVN_API(nvnSamplerPoolGetSize) { ret_x(c, (uint64_t)(int64_t)((const dpool_t *)arg_p(c, 0))->n); }

/* void nvnTexturePoolRegisterImage(const NVNtexturePool *p, int id, const NVNtexture *t, const NVNtextureView *v):
 * image descriptors share the texture pool; the back end keeps one table for both */
NVN_API(nvnTexturePoolRegisterImage) { gfx_backend()->texture_register((int)arg_x(c, 1), arg_p(c, 2), arg_p(c, 3)); }

/* Multisample state fields the replay does not use, stored so the getters read them back */
#define MS(c) ((ms_st *)arg_p(c, 0))
NVN_API(nvnMultisampleStateSetCoverageModulationMode) { MS(c)->cov_mod = (uint8_t)arg_x(c, 1); }
NVN_API(nvnMultisampleStateGetCoverageModulationMode) { ret_x(c, MS(c)->cov_mod); }
NVN_API(nvnMultisampleStateSetCoverageToColorEnable) { MS(c)->cov2color_en = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnMultisampleStateGetCoverageToColorEnable) { ret_x(c, MS(c)->cov2color_en); }
NVN_API(nvnMultisampleStateSetCoverageToColorOutput) { MS(c)->cov2color_out = (uint8_t)arg_x(c, 1); }
NVN_API(nvnMultisampleStateGetCoverageToColorOutput) { ret_x(c, MS(c)->cov2color_out); }
NVN_API(nvnMultisampleStateSetSampleLocationsGridEnable) { MS(c)->sl_grid = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnMultisampleStateGetSampleLocationsGridEnable) { ret_x(c, MS(c)->sl_grid); }
NVN_API(nvnMultisampleStateGetAlphaToCoverageDither) { ret_x(c, MS(c)->a2c_dither); }

/* void nvnQueueBuilderSetQueuePriority(NVNqueueBuilder *b, NVNqueuePriority p): one queue here */
NVN_API(nvnQueueBuilderSetQueuePriority) { (void)c; }
