/*
 * Copyright (c) 2026 WIZnet Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_STDIO_LINK_H
#define USB_STDIO_LINK_H

#include "opcua_settings.h"

void usb_stdio_link_init(const opcua_settings_t *cfg);
void usb_stdio_link_poll(void);
void usb_stdio_print_help(void);
void usb_stdio_print_state(void);

#endif /* USB_STDIO_LINK_H */
