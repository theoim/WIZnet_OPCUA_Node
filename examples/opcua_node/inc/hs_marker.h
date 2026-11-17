/*
 * Copyright (c) 2026 WIZnet Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file    hs_marker.h
 * @brief   Handshake timing markers (built with -DOPCUA_HS_MARKERS=ON).
 *
 * See src/hs_marker.c for the pins and the console format.
 */

#ifndef HS_MARKER_H
#define HS_MARKER_H

/** Configure the marker GPIOs and announce them in the boot log. */
void hs_marker_init(void);

#endif /* HS_MARKER_H */
