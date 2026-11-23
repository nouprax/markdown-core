#include "flank.h"

static inline bool S_left(markdown_core_flank flank) {
    return !flank.space_after && (!flank.punct_after || flank.space_before || flank.punct_before);
}

static inline bool S_right(markdown_core_flank flank) {
    return !flank.space_before && (!flank.punct_before || flank.space_after || flank.punct_after);
}

unsigned markdown_core_flank_free(markdown_core_flank flank) {
    return (S_left(flank) ? MARKDOWN_CORE_FLANK_OPENS : 0u) | (S_right(flank) ? MARKDOWN_CORE_FLANK_CLOSES : 0u);
}

unsigned markdown_core_flank_bound(markdown_core_flank flank) {
    const bool left = S_left(flank), right = S_right(flank);
    return (left && (!right || flank.punct_before) ? MARKDOWN_CORE_FLANK_OPENS : 0u) |
           (right && (!left || flank.punct_after) ? MARKDOWN_CORE_FLANK_CLOSES : 0u);
}
