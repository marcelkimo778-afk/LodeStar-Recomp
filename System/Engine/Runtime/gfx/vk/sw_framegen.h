// Frame generation (sw_framegen.c, Arceus Recomp): the back end's present (vk_backend.c, [arceus] hunks) uses it.
#pragma once
#include <stdint.h>
#include "vk.h"

typedef struct {
    int n;                  // pictures to show for this game picture: n - 1 made ones, then the real one
    int prev, cur, fin;     // history slots of the pair, the motion field
    uint64_t start, step;   // when the first goes out (0: at once), the time between them, ns
} SwFgJob;

int sw_fg_capture(VkImage src, VkFormat fmt, int w, int h, const int *crop, uint64_t at_ns, SwFgJob *job);
void sw_fg_present(const SwFgJob *job);
int sw_fg_level(void);
void sw_fg_set(int n);
int sw_fg_effective(void);
