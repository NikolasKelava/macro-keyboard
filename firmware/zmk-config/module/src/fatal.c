/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Reboot on fatal error instead of hanging.
 *
 * Zephyr's default k_sys_fatal_error_handler() (weak, in kernel/fatal.c) ends in
 * arch_system_halt() — it spins forever, so any unhandled CPU fault leaves the
 * board dead until it is power-cycled. Overriding it to sys_reboot() lets the
 * device recover on its own (USB re-enumerates, BLE reconnects). Paired with
 * CONFIG_HW_STACK_PROTECTION=y (MPU stack guard) and CONFIG_REBOOT=y, this is the
 * kept-for-good resilience net from the M6 USB-crash hunt.
 *
 * k_sys_fatal_error_handler is __weak in Zephyr and not defined by ZMK, so this
 * strong definition simply replaces it. It runs in a fault context, so it is
 * deliberately minimal — no logging (no backend is configured) — it just resets.
 */

#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(reason);
    ARG_UNUSED(esf);

    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}
