// Frame generation shaders (sw_framegen.c). Every image is rgba16f in GENERAL layout.
layout(local_size_x = 8, local_size_y = 8) in;
layout(push_constant) uniform P {
    ivec2 size;      // output size
    ivec2 full;      // full picture size
    float t;         // interp: time of the generated frame, 0 = previous frame, 1 = current
    float radius;    // search: search radius in this level's pixels; down: 1 = input is colour
    float prior;     // search: 1 = a coarser flow in s2
    float pad;
} pc;
layout(binding = 0) uniform sampler2D s0;
layout(binding = 1) uniform sampler2D s1;
layout(binding = 2) uniform sampler2D s2;
layout(binding = 3, rgba16f) uniform writeonly image2D dst;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
