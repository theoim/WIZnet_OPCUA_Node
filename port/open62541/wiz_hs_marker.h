/*
 * Copyright (c) 2026 WIZnet Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file    wiz_hs_marker.h
 * @brief   Hooks that mark an asymmetric OPN handshake for timing measurement.
 *
 * The event loop calls these around the delivery of an OPN that needs RSA or
 * ECC work (SecurityPolicy other than None). Delivery is synchronous: the
 * server decrypts, verifies, signs and sends the response inside the callback,
 * so begin..end is exactly the stretch during which the poll loop serves no
 * other socket. An OPN split over several recv() calls is tracked to its last
 * byte, so the window ends when the call that completed it returns.
 *
 * The event loop provides weak no-op definitions. An image that wants the
 * markers links a strong definition (examples/opcua_node/src/hs_marker.c,
 * built with -DOPCUA_HS_MARKERS=ON); every other image pays nothing.
 */

#ifndef WIZ_HS_MARKER_H
#define WIZ_HS_MARKER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** First byte of an asymmetric OPN has been handed to the server.
 *  @param renew true when the connection already completed one OPN, i.e. this
 *               is a SecureChannel renewal rather than a new client. */
void wiz_hs_opn_begin(uint8_t sock, bool renew);

/** The call that delivered the last byte of that OPN has returned.
 *  @param aborted true when the connection closed before the OPN completed. */
void wiz_hs_opn_end(uint8_t sock, bool aborted);

#ifdef __cplusplus
}
#endif

#endif /* WIZ_HS_MARKER_H */
