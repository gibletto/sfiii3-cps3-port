/*
 * ctr_depth.h  3D depth for the attract screens (3DS)
 *
 * Depth 0 is the screen, positive is in front, negative behind. The renderer shifts each layer by its depth
 * (times the 3D slider) in opposite directions for each eye.
 */

#ifndef CTR_DEPTH_H
#define CTR_DEPTH_H

#include "types.h"

int ctr_depth_frame(void);                      /* once a frame: nonzero if this screen has depth */
s16 ctr_depth_sprite(u32 palette, int tilemap); /* a sprite's depth by palette, or a tilemap's */
s16 ctr_depth_text(s32 row, u32 palette);       /* a text layer tile's depth by row (0-27) and palette */

#endif
