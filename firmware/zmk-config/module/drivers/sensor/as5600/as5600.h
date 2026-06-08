/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

struct as5600_config {
    struct i2c_dt_spec i2c;
    uint16_t poll_period_ms;
};

struct as5600_data {
    const struct device *dev;

    /* SENSOR_TRIG_DATA_READY handler registered by ZMK's keymap-sensors. */
    sensor_trigger_handler_t handler;
    const struct sensor_trigger *trigger;

    uint16_t last_angle;   /* reference 12-bit angle (0..4095), filtered      */
    int32_t pending_counts; /* delta counts accumulated since last channel_get */
    int32_t filt;          /* low-pass state: filtered angle * AS5600_FILT_SCALE */

    struct k_thread thread;
    K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_AS5600_THREAD_STACK_SIZE);
};
