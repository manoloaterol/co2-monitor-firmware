/*
 * Copyright (c) 2018, Sensirion AG
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Sensirion AG nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensirion_i2c_hal.h"
#include "sensirion_common.h"
#include "sensirion_config.h"

#define I2C_MASTER_SCL_IO           12
#define I2C_MASTER_SDA_IO           11
#define I2C_MASTER_NUM              I2C_NUM_0

/*
 * 50 kHz: the SCD30 accepts at most 100 kHz and Sensirion recommends 50 kHz
 * or less. At 400 kHz the bus runs out of specification, which shows up as
 * intermittent timeouts and CRC errors rather than as a clean failure.
 */
#define I2C_MASTER_FREQ_HZ          50000

/*
 * The SCD30 stretches the clock, typically up to 30 ms and up to 150 ms
 * during its internal calibration. The driver's default on the ESP32-S2 is
 * 2 ms, far too short for that. 200 ms is close to the most the peripheral
 * can count, 2^24 cycles of the 80 MHz APB clock, about 210 ms.
 */
#define I2C_MASTER_SCL_WAIT_US      200000

/*
 * Software limit per transaction, above the longest stretch plus the transfer
 * itself. A finite value matters: a glitched bus must come back as an error
 * the application can report, not block the display loop.
 */
#define I2C_MASTER_TIMEOUT_MS       300

static const char *SCD30_TAG = "SCD30";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t                 s_dev_addr;

/*
 * The Sensirion driver passes whatever this layer returns up as its error, and
 * its own codes are small positive numbers (CRC_ERROR 1, BYTE_NUM_ERROR 4), so
 * the bus errors are negative codes that cannot collide.
 *
 * The master driver reports a NACK as ESP_ERR_INVALID_RESPONSE and other
 * failures as ESP_ERR_INVALID_STATE, but a NACK alone does not say whether the
 * sensor is merely busy or gone. An address probe right after the failure
 * tells the two apart, which is what
 * points at a loose wire rather than at a sensor that is merely busy. It only
 * runs after a failure, so it costs nothing in normal operation.
 */
static int8_t hal_err(esp_err_t err, uint8_t address)
{
    if (err == ESP_OK) {
        return 0;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return SENSIRION_HAL_ERR_TIMEOUT;   // the bus lock or the transfer timed out
    }

    switch (i2c_master_probe(s_bus, address, I2C_MASTER_TIMEOUT_MS)) {
    case ESP_ERR_NOT_FOUND: return SENSIRION_HAL_ERR_NACK;      // no ACK from the sensor
    case ESP_ERR_TIMEOUT:   return SENSIRION_HAL_ERR_TIMEOUT;
    default:                return SENSIRION_HAL_ERR_OTHER;     // answers again now
    }
}

/*
 * One device handle, for the address in use. The SCD30 is alone on its bus
 * and always at 0x61, so this only does work on the first transfer.
 */
static i2c_master_dev_handle_t device(uint8_t address)
{
    if (s_bus == NULL) {
        return NULL;
    }
    if (s_dev != NULL && s_dev_addr == address) {
        return s_dev;
    }
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = address,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
        .scl_wait_us     = I2C_MASTER_SCL_WAIT_US,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(SCD30_TAG, "I2C device 0x%02x: %s", address, esp_err_to_name(err));
        s_dev = NULL;
        return NULL;
    }
    s_dev_addr = address;
    return s_dev;
}

/**
 * Select the current i2c bus by index. Not needed: the SCD30 is alone on its
 * bus.
 */
int16_t sensirion_i2c_hal_select_bus(uint8_t bus_idx) {
    (void)bus_idx;
    return NOT_IMPLEMENTED_ERROR;
}

/**
 * Initialize all hard- and software components that are needed for the I2C
 * communication.
 */
void sensirion_i2c_hal_init(void) {
    // The internal pull-ups stay off: at 30-50 kOhm they are far too weak for
    // I2C, and the board provides its own.
    const i2c_master_bus_config_t conf = {
        .i2c_port          = I2C_MASTER_NUM,
        .sda_io_num        = I2C_MASTER_SDA_IO,
        .scl_io_num        = I2C_MASTER_SCL_IO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };

    esp_err_t err = i2c_new_master_bus(&conf, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(SCD30_TAG, "I2C init failed: %s", esp_err_to_name(err));
        s_bus = NULL;
        return;
    }

    // The driver logs every failed transfer at error level, several lines
    // each, and a sensor that comes loose fails on every poll. The failure is
    // still reported, once per episode and with its cause, by the application.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    ESP_LOGI(SCD30_TAG, "I2C up at %d kHz", I2C_MASTER_FREQ_HZ / 1000);
}

/**
 * Release all resources initialized by sensirion_i2c_hal_init(). Nothing to
 * do: the bus lives as long as the device.
 */
void sensirion_i2c_hal_free(void) {
}

/**
 * Execute one read transaction on the I2C bus, reading a given number of bytes.
 * If the device does not acknowledge the read command, an error shall be
 * returned.
 *
 * @param address 7-bit I2C address to read from
 * @param data    pointer to the buffer where the data is to be stored
 * @param count   number of bytes to read from I2C and store in the buffer
 * @returns 0 on success, a SENSIRION_HAL_ERR_* code otherwise
 */
int8_t sensirion_i2c_hal_read(uint8_t address, uint8_t* data, uint16_t count) {
    i2c_master_dev_handle_t dev = device(address);
    if (dev == NULL) {
        return SENSIRION_HAL_ERR_OTHER;
    }
    return hal_err(i2c_master_receive(dev, data, count, I2C_MASTER_TIMEOUT_MS), address);
}

/**
 * Execute one write transaction on the I2C bus, sending a given number of
 * bytes. The bytes in the supplied buffer must be sent to the given address. If
 * the slave device does not acknowledge any of the bytes, an error shall be
 * returned.
 *
 * @param address 7-bit I2C address to write to
 * @param data    pointer to the buffer containing the data to write
 * @param count   number of bytes to read from the buffer and send over I2C
 * @returns 0 on success, a SENSIRION_HAL_ERR_* code otherwise
 */
int8_t sensirion_i2c_hal_write(uint8_t address, const uint8_t* data,
                               uint16_t count) {
    i2c_master_dev_handle_t dev = device(address);
    if (dev == NULL) {
        return SENSIRION_HAL_ERR_OTHER;
    }
    return hal_err(i2c_master_transmit(dev, data, count, I2C_MASTER_TIMEOUT_MS), address);
}

/**
 * Sleep for a given number of microseconds. The function should delay the
 * execution for at least the given time, but may also sleep longer.
 *
 * @param useconds the sleep time in microseconds
 */
void sensirion_i2c_hal_sleep_usec(uint32_t useconds) {
    // Rounded up, plus one tick: vTaskDelay(n) can return as early as the next
    // tick boundary, so n ticks guarantee only n - 1 full periods. Truncating
    // instead turned the driver's 10 ms waits into anything from 0 to 10 ms at
    // the 100 Hz tick, and the sensor was read before it was ready.
    vTaskDelay(pdMS_TO_TICKS((useconds + 999) / 1000) + 1);
}
