/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * AS5600 magnetic rotary encoder, driven as a ZMK keymap sensor.
 *
 * ZMK's keymap-sensors subsystem (app/src/sensors.c) drives a sensor purely
 * through a SENSOR_TRIG_DATA_READY trigger: when the trigger fires it does
 * sensor_sample_fetch() + sensor_channel_get(SENSOR_CHAN_ROTATION) and feeds
 * the resulting value to the bound sensor-rotate behaviour. The AS5600 has no
 * interrupt line wired to the MCU on this board (its OUT pad is unused), so we
 * synthesise the data-ready trigger by polling the RAW ANGLE register on a
 * dedicated thread — kept off the system work queue so a blocked I2C read
 * (the OLED shares this bus) can never stall BLE/USB.
 *
 * Reporting model. We report a *rotation delta in degrees* (val1 = whole
 * degrees, val2 = micro-degrees) so ZMK's `triggers-per-rotation` controls how
 * many discrete tick events a full turn produces. ZMK's behaviour layer treats
 * a report whose val1 == 0 as a legacy "val2 is a raw tick count" message, so
 * we only ever report once at least one whole degree (~12 counts) has
 * accumulated, carrying the sub-degree remainder forward — that keeps val1 != 0
 * and loses no motion.
 */

#define DT_DRV_COMPAT nikolas_as5600

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/activity.h>

#include "as5600.h"

LOG_MODULE_REGISTER(AS5600, CONFIG_SENSOR_LOG_LEVEL);

#define AS5600_REG_RAW_ANGLE_H 0x0E
#define AS5600_PULSES_PER_REV  4096
#define AS5600_FULL_ANGLE      360
#define AS5600_ANGLE_MASK      0x0FFF
#define AS5600_MILLION         1000000
#define AS5600_FILT_SCALE      256 /* fixed-point scale for the low-pass state */

static int as5600_read_angle(const struct device *dev, uint16_t *angle) {
    const struct as5600_config *cfg = dev->config;
    uint8_t reg = AS5600_REG_RAW_ANGLE_H;
    uint8_t buf[2];

    int err = i2c_write_read_dt(&cfg->i2c, &reg, sizeof(reg), buf, sizeof(buf));
    if (err) {
        return err;
    }

    *angle = (((uint16_t)buf[0] << 8) | buf[1]) & AS5600_ANGLE_MASK;
    return 0;
}

/* Angle is sampled by the poll thread; ZMK's fetch() has nothing to do. */
static int as5600_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    ARG_UNUSED(dev);
    ARG_UNUSED(chan);
    return 0;
}

static int as5600_channel_get(const struct device *dev, enum sensor_channel chan,
                              struct sensor_value *val) {
    struct as5600_data *data = dev->data;

    if (chan != SENSOR_CHAN_ROTATION) {
        return -ENOTSUP;
    }

    unsigned int key = irq_lock();
    int32_t counts = data->pending_counts;
    data->pending_counts = 0;
    irq_unlock(key);

    int32_t scaled = counts * AS5600_FULL_ANGLE; /* degrees, in 1/4096ths */
    val->val1 = scaled / AS5600_PULSES_PER_REV;
    val->val2 = (int32_t)(((int64_t)(scaled % AS5600_PULSES_PER_REV) * AS5600_MILLION) /
                          AS5600_PULSES_PER_REV);
    return 0;
}

static int as5600_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
                              sensor_trigger_handler_t handler) {
    struct as5600_data *data = dev->data;

    if (trig->type != SENSOR_TRIG_DATA_READY) {
        return -ENOTSUP;
    }

    data->trigger = trig;
    data->handler = handler;
    return 0;
}

static void as5600_thread(void *p1, void *p2, void *p3) {
    const struct device *dev = p1;
    const struct as5600_config *cfg = dev->config;
    struct as5600_data *data = dev->data;
    bool primed = false;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* Let the shared I2C bus (OLED), BLE and USB finish coming up before this
     * thread starts hitting the bus — touching I2C during the OLED's own init
     * sequence was an intermittent boot-hang source. */
    k_msleep(CONFIG_AS5600_STARTUP_DELAY_MS);

    while (1) {
        /* Back off the poll rate once the board leaves ACTIVE (display blanked
         * after the idle timeout). At rest the deadband emits no events, so the
         * fast period would keep waking the CPU ~200x/s and block the SoC's
         * low-power idle (M6 power item). The first notch after idle still
         * raises a sensor event -> activity resets to ACTIVE -> we're back to
         * the fast period next loop. (In SLEEP the SoC is powered off and this
         * thread isn't running at all, so only IDLE is in play here.) */
        uint32_t period = cfg->poll_period_ms;
        if (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE &&
            CONFIG_AS5600_IDLE_POLL_PERIOD_MS > period) {
            period = CONFIG_AS5600_IDLE_POLL_PERIOD_MS;
        }
        k_msleep(period);

        uint16_t angle;
        if (as5600_read_angle(dev, &angle) != 0) {
            continue; /* transient bus error — keep last_angle, retry */
        }

        /* First good reading seeds the reference and the filter (no spurious
         * delta from the power-on default of 0). */
        if (!primed) {
            data->last_angle = angle;
            data->filt = (int32_t)angle * AS5600_FILT_SCALE;
            primed = true;
            continue;
        }

        /* Light wrap-aware low-pass (EMA) to take the edge off raw-angle
         * fluctuation before anything downstream sees it. alpha = 1/2^SHIFT;
         * SHIFT=0 disables it. Fixed-point so slow motion isn't lost to
         * truncation. */
        {
            int32_t filt_counts = data->filt / AS5600_FILT_SCALE;
            int16_t fd = (int16_t)angle - (int16_t)filt_counts;
            if (fd >= AS5600_PULSES_PER_REV / 2) {
                fd -= AS5600_PULSES_PER_REV;
            } else if (fd < -AS5600_PULSES_PER_REV / 2) {
                fd += AS5600_PULSES_PER_REV;
            }
            data->filt += ((int32_t)fd * AS5600_FILT_SCALE) >> CONFIG_AS5600_SMOOTHING_SHIFT;
            const int32_t mod = AS5600_PULSES_PER_REV * AS5600_FILT_SCALE;
            data->filt = ((data->filt % mod) + mod) % mod;
            angle = (uint16_t)((data->filt / AS5600_FILT_SCALE) & AS5600_ANGLE_MASK);
        }

        /* Wrap-safe 12-bit delta from the *reference* angle (last_angle).
         * Range [-2048, 2047]. */
        int16_t diff = (int16_t)angle - (int16_t)data->last_angle;
        if (diff >= AS5600_PULSES_PER_REV / 2) {
            diff -= AS5600_PULSES_PER_REV;
        } else if (diff < -AS5600_PULSES_PER_REV / 2) {
            diff += AS5600_PULSES_PER_REV;
        }

        /* Deadband. The RAW ANGLE register jitters by ~1-2 LSB even when the
         * magnet is still; without this the reading random-walks past the 1°
         * report threshold roughly every second, raising spurious sensor
         * events. Those events reset ZMK's activity timer (activity.c
         * subscribes to zmk_sensor_event), so the board never goes idle — the
         * display stops blanking and it never deep-sleeps. While |diff| stays
         * within the band we treat it as no motion and, crucially, do NOT
         * advance the reference, so jitter around a fixed point produces
         * nothing. (The AS5600's filtered ANGLE register 0x0C is an
         * alternative, but a software deadband keeps us on RAW per the plan.) */
        if (diff > -CONFIG_AS5600_DEADBAND_COUNTS && diff < CONFIG_AS5600_DEADBAND_COUNTS) {
            continue;
        }
        data->last_angle = angle; /* advance reference only on real movement */

        unsigned int key = irq_lock();
        data->pending_counts += diff;
        int32_t whole_degrees = (data->pending_counts * AS5600_FULL_ANGLE) / AS5600_PULSES_PER_REV;
        irq_unlock(key);

        /* Hold sub-degree motion until it forms a whole degree (keeps the
         * report's val1 != 0; the remainder stays in pending_counts). */
        if (whole_degrees == 0) {
            continue;
        }

        if (data->handler) {
            data->handler(dev, data->trigger);
        }
    }
}

static const struct sensor_driver_api as5600_driver_api = {
    .trigger_set = as5600_trigger_set,
    .sample_fetch = as5600_sample_fetch,
    .channel_get = as5600_channel_get,
};

static int as5600_init(const struct device *dev) {
    const struct as5600_config *cfg = dev->config;
    struct as5600_data *data = dev->data;

    data->dev = dev;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("I2C bus %s not ready", cfg->i2c.bus->name);
        return -ENODEV;
    }

    /* No I2C here on purpose: the bus is shared with the OLED, whose driver is
     * initialising around the same time. The poll thread does the first read
     * after a startup delay and primes last_angle itself. */
    k_thread_create(&data->thread, data->thread_stack, CONFIG_AS5600_THREAD_STACK_SIZE,
                    as5600_thread, (void *)dev, NULL, NULL, CONFIG_AS5600_THREAD_PRIORITY, 0,
                    K_NO_WAIT);
    k_thread_name_set(&data->thread, "as5600");

    return 0;
}

#define AS5600_INST(n)                                                                              \
    static struct as5600_data as5600_data_##n;                                                      \
    static const struct as5600_config as5600_cfg_##n = {                                            \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                                             \
        .poll_period_ms = DT_INST_PROP(n, poll_period_ms),                                          \
    };                                                                                              \
    DEVICE_DT_INST_DEFINE(n, as5600_init, NULL, &as5600_data_##n, &as5600_cfg_##n, POST_KERNEL,     \
                          CONFIG_SENSOR_INIT_PRIORITY, &as5600_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AS5600_INST)
