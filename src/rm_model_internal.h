/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Shared internal value-model predicates and column width resolver.
 * Depends only on the public `rmlines/types.h`. */

#ifndef RM_MODEL_INTERNAL_H
#define RM_MODEL_INTERNAL_H

#include "rmlines/types.h"

/* These checks apply to newly written pen data. A reader can still encounter
 * tool and colour numbers the writer cannot produce. */
bool rmi_pen_tool_valid(rm_pen_tool tool);
bool rmi_pen_color_valid(rm_pen_color color);
bool rmi_pen_thickness_valid(rm_pen_tool tool, double thickness);
bool rmi_stroke_color_valid(rm_pen_tool tool, rm_pen_color color,
                            bool has_argb);

/* Resolve an automatic column against this sheet before writing the legacy
 * width in page units. */
float rmi_resolved_column_width(uint64_t setting, uint32_t screen_width,
                                uint32_t screen_height);
bool rmi_uuid_is_nil(const uint8_t uuid[16]);

#endif /* RM_MODEL_INTERNAL_H */
