/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Tests use this header to reach the parser's scratch storage, the byte
 * reader and writer, and the edit helpers. The installed headers expose none
 * of them. */

#ifndef RM_INTERNAL_H
#define RM_INTERNAL_H

#include "rm_crdt.h"
#include "rm_edit_ops.h"
#include "rm_log_internal.h"
#include "rm_model_internal.h"
#include "rm_scene.h"
#include "rm_support.h"
#include "rm_wire.h"
#include "rm_wire_write.h"

#ifdef RMLINES_TESTING
/* Tests can damage a session revision in place to exercise verification and
 * recovery. */
uint8_t* rm_test_edit_bytes_mut(rm_edit_session* session, size_t* size);
#endif

#endif /* RM_INTERNAL_H */
