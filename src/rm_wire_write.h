/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Write-only wire helpers for writer and editor modules. */

#ifndef RM_WIRE_WRITE_H
#define RM_WIRE_WRITE_H

#include "rm_model_internal.h"
#include "rm_wire.h"
#include "rmlines/writer.h"

/* Both draft pages and inserted strokes use the packed v2 point layout.
 * Include the actual identifier widths when checking that the enclosing
 * lengths fit. */
void rm_write_line_item(rm_buf* out, const rm_draft_stroke* stroke,
                        rm_wid parent, rm_wid id, rm_wid left, rm_wid right);
bool rmi_line_item_wire_fits(const rm_draft_stroke* stroke, rm_wid parent,
                             rm_wid id, rm_wid left, rm_wid right);

/* The stamped text setting nests the width one level deeper than the legacy
 * float. */
void rmi_write_text_settings_width(rm_buf* out, rm_wid stamp, float width);

#endif /* RM_WIRE_WRITE_H */
