#include "mpu6500.h"
#include <inttypes.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

static const char *TAG = "mpu6500";

#define MPU6500_TIMEOUT_MS          1000

#define MPU6500_SENSOR_ADDR         0x68        /*!< I2C address of the sensor (AD0 low) */
#define MPU6500_WHO_AM_I_REG_ADDR   0x75        /*!< Register addresses of the "who am I" register */
#define MPU6500_WHO_AM_I_VALUE      0x70
#define MPU6500_PWR_MGMT_1_REG_ADDR 0x6B        /*!< Register addresses of the power management register */
#define MPU6500_RESET_BIT           7
#define MPU6500_PWR_MGMT_1_CLKSEL_AUTO 1 // MPU6500: use the gyro PLL when ready (more accurate than the internal oscillator)
#define MPU6500_ACCEL_XOUT          0x3B // accel registers read from 0x3B to 0x40, x to y to z, each one using 2 bytes
#define MPU6500_SMPLRT_DIV_REG      0x19 // sample rate divider
#define MPU6500_CONFIG_REG          0x1A // general config register, holds digital low pass filter (DLPF) setting
#define MPU6500_ACCEL_CONFIG_REG    0x1C // accelerometer full-scale range (AFS_SEL lives in bits 4:3)
#define MPU6500_ACCEL_CONFIG_AFS_SEL_SHIFT 3
#define MPU6500_ACCEL_CONFIG2_REG   0x1D // MPU6500: accel DLPF (CONFIG's DLPF_CFG only filters the gyro)
#define MPU6500_ACCEL_DLPF_CFG_218HZ 1   // A_DLPF_CFG=1, ACCEL_FCHOICE_B=0: 218.1 Hz bandwidth, 1 kHz rate
#define MPU6500_INT_ENABLE_REG      0x38 // interrupt source enables
#define MPU6500_INT_ENABLE_DATA_RDY_BIT 0 // fires once per internal sample
#define MPU6500_USER_CTRL_REG       0x6A // FIFO enable/reset live here
#define MPU6500_USER_CTRL_FIFO_EN_BIT    6
#define MPU6500_USER_CTRL_FIFO_RESET_BIT 2
#define MPU6500_FIFO_EN_REG         0x23 // which data streams feed the FIFO
#define MPU6500_FIFO_EN_ACCEL_BIT  3
#define MPU6500_FIFO_COUNT_H_REG    0x72 // 16-bit big-endian byte count currently in the FIFO
#define MPU6500_FIFO_R_W_REG        0x74 // read this address repeatedly to drain the FIFO
#define MPU6500_FIFO_SAMPLE_BYTES  6 // one accel sample = 3 axes x 2 bytes
#define MPU6500_FIFO_SIZE_BYTES    512
#define MPU6500_FIFO_READ_CHUNK_SAMPLES 16 // bounds the on-stack scratch buffer below regardless of backlog size

struct mpu6500_dev_t {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    float accel_lsb_per_g;
};

static esp_err_t mpu6500_register_read(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_handle, &reg_addr, 1, data, len, MPU6500_TIMEOUT_MS);
}

static esp_err_t mpu6500_register_write_byte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(dev_handle, write_buf, sizeof(write_buf), MPU6500_TIMEOUT_MS);
}

/**
 * @brief LSB-per-g sensitivity for a given accelerometer full-scale range.
 * This is the single place that knows "AFS_SEL=1 means +/-4g means 8192 LSB/g" -
 * both the register write and the raw-to-g conversion pull from here.
 */
static float mpu6500_accel_fs_lsb_per_g(mpu6500_accel_fs_t fs)
{
    static const float lsb_per_g[] = {
        [MPU6500_ACCEL_FS_2G]  = 16384.0f,
        [MPU6500_ACCEL_FS_4G]  = 8192.0f,
        [MPU6500_ACCEL_FS_8G]  = 4096.0f,
        [MPU6500_ACCEL_FS_16G] = 2048.0f,
    };
    return lsb_per_g[fs];
}

/**
 * @brief Debug helper: probe every 7-bit I2C address and log which ones ACK.
 * The sensor should show up at 0x68 (AD0 low) or 0x69 (AD0 high)
 */
static void mpu6500_scan_bus(i2c_master_bus_handle_t bus_handle)
{
    int found = 0;
    int timeouts = 0;
    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint16_t addr = 0x08; addr < 0x78; addr++) {
        esp_err_t err = i2c_master_probe(bus_handle, addr, 50);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  device found at 0x%02X", addr);
            found++;
        } else if (err == ESP_ERR_TIMEOUT) {
            // The driver resets the bus after a timeout, so later addresses can still answer
            ESP_LOGW(TAG, "  bus timed out at 0x%02X", addr);
            timeouts++;
        }
    }
    if (timeouts > 0) {
        ESP_LOGW(TAG, "Scan done, %d device(s) found, %d address(es) timed out - check wiring and pull-ups",
                 found, timeouts);
    } else {
        ESP_LOGI(TAG, "Scan done, %d device(s) found", found);
    }
}

esp_err_t mpu6500_init(const mpu6500_config_t *config, mpu6500_handle_t *out_handle)
{
    // The sensor can only produce rates of MPU6500_BASE_RATE_HZ / (1 + SMPLRT_DIV), with SMPLRT_DIV in 0..255
    if (config->sample_rate_hz == 0 || config->sample_rate_hz > MPU6500_BASE_RATE_HZ ||
        MPU6500_BASE_RATE_HZ % config->sample_rate_hz != 0 ||
        MPU6500_BASE_RATE_HZ / config->sample_rate_hz - 1 > 255) {
        ESP_LOGE(TAG, "Sample rate %" PRIu32 " Hz must evenly divide %d Hz",
                 config->sample_rate_hz, MPU6500_BASE_RATE_HZ);
        return ESP_ERR_INVALID_ARG;
    }

    struct mpu6500_dev_t *dev = calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = config->sda_io_num,
        .scl_io_num = config->scl_io_num,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &dev->bus_handle);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }

    /* If the ESP32 was reset mid-transfer while the sensor stayed powered (e.g. after
     * flashing), the sensor can still be holding SDA low. Clock it free before first use. */
    err = i2c_master_bus_reset(dev->bus_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus reset failed: %s", esp_err_to_name(err));
    }

    mpu6500_scan_bus(dev->bus_handle);

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU6500_SENSOR_ADDR,
        .scl_speed_hz = config->i2c_freq_hz,
    };
    err = i2c_master_bus_add_device(dev->bus_handle, &dev_config, &dev->dev_handle);
    if (err != ESP_OK) {
        i2c_del_master_bus(dev->bus_handle);
        free(dev);
        return err;
    }

    /* Check if MPU6500 is connected by reading the WHO_AM_I register */
    uint8_t who_am_i;
    err = mpu6500_register_read(dev->dev_handle, MPU6500_WHO_AM_I_REG_ADDR, &who_am_i, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WHO_AM_I - check wiring, power, and pull-ups");
        goto fail;
    }
    if (who_am_i != MPU6500_WHO_AM_I_VALUE) {
        ESP_LOGW(TAG, "Unexpected WHO_AM_I = 0x%02X (expected 0x%02X), continuing anyway", who_am_i,
                 MPU6500_WHO_AM_I_VALUE);
    } else {
        ESP_LOGI(TAG, "WHO_AM_I = 0x%02X", who_am_i);
    }

    // Reset the device, then clear the SLEEP bit and select the PLL clock
    err = mpu6500_register_write_byte(dev->dev_handle, MPU6500_PWR_MGMT_1_REG_ADDR, 1 << MPU6500_RESET_BIT);
    if (err != ESP_OK) {
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    err = mpu6500_register_write_byte(dev->dev_handle, MPU6500_PWR_MGMT_1_REG_ADDR, MPU6500_PWR_MGMT_1_CLKSEL_AUTO);
    if (err != ESP_OK) {
        goto fail;
    }

    // Gyro DLPF setting 1 (184 Hz). This also sets the 1 kHz internal rate that SMPLRT_DIV divides
    err = mpu6500_register_write_byte(dev->dev_handle, MPU6500_CONFIG_REG, 1);
    if (err != ESP_OK) {
        goto fail;
    }

    // Accel DLPF is configured separately on the MPU6500; 218 Hz is its closest setting to 184 Hz
    err = mpu6500_register_write_byte(dev->dev_handle, MPU6500_ACCEL_CONFIG2_REG, MPU6500_ACCEL_DLPF_CFG_218HZ);
    if (err != ESP_OK) {
        goto fail;
    }

    /* Sample Rate = Internal Sample Rate / (1 + SMPLRT_DIV), where the internal rate is 1 kHz
     * while the gyro DLPF is enabled (DLPF_CFG 1-6, FCHOICE_B = 0). SMPLRT_DIV has no effect otherwise */
    const uint8_t smplrt_div = MPU6500_BASE_RATE_HZ / config->sample_rate_hz - 1;
    err = mpu6500_register_write_byte(dev->dev_handle, MPU6500_SMPLRT_DIV_REG, smplrt_div);
    if (err != ESP_OK) {
        goto fail;
    }

    err = mpu6500_register_write_byte(dev->dev_handle, MPU6500_ACCEL_CONFIG_REG,
                                       config->accel_fs << MPU6500_ACCEL_CONFIG_AFS_SEL_SHIFT);
    if (err != ESP_OK) {
        goto fail;
    }

    dev->accel_lsb_per_g = mpu6500_accel_fs_lsb_per_g(config->accel_fs);
    ESP_LOGI(TAG, "Sampling at %" PRIu32 " Hz (SMPLRT_DIV=%u)", config->sample_rate_hz, smplrt_div);

    *out_handle = dev;
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(dev->dev_handle);
    i2c_del_master_bus(dev->bus_handle);
    free(dev);
    return err;
}

esp_err_t mpu6500_read_accel(mpu6500_handle_t handle, mpu6500_measurements_t *out_measurements)
{
    uint8_t buffer[6];
    esp_err_t err = mpu6500_register_read(handle->dev_handle, MPU6500_ACCEL_XOUT, buffer, sizeof(buffer));
    if (err != ESP_OK) {
        return err;
    }

    int16_t raw;
    raw = (int16_t) ((buffer[0] << 8) | buffer[1]);
    out_measurements->accel_x = raw / handle->accel_lsb_per_g;
    raw = (int16_t) ((buffer[2] << 8) | buffer[3]);
    out_measurements->accel_y = raw / handle->accel_lsb_per_g;
    raw = (int16_t) ((buffer[4] << 8) | buffer[5]);
    out_measurements->accel_z = raw / handle->accel_lsb_per_g;

    return ESP_OK;
}

esp_err_t mpu6500_enable_data_ready_interrupt(mpu6500_handle_t handle)
{
    // Enables INT pin to pull high every time a new sample is ready
    return mpu6500_register_write_byte(handle->dev_handle, MPU6500_INT_ENABLE_REG,
                                        1 << MPU6500_INT_ENABLE_DATA_RDY_BIT);
}

/**
 * @brief Empty the FIFO and start filling it again.
 * FIFO_RESET only takes effect while FIFO_EN is 0, so the FIFO is disabled first.
 */
static esp_err_t mpu6500_fifo_restart(mpu6500_handle_t handle)
{
    esp_err_t err = mpu6500_register_write_byte(handle->dev_handle, MPU6500_USER_CTRL_REG, 0);
    if (err != ESP_OK) {
        return err;
    }

    err = mpu6500_register_write_byte(handle->dev_handle, MPU6500_USER_CTRL_REG,
                                       1 << MPU6500_USER_CTRL_FIFO_RESET_BIT);
    if (err != ESP_OK) {
        return err;
    }

    return mpu6500_register_write_byte(handle->dev_handle, MPU6500_USER_CTRL_REG,
                                        1 << MPU6500_USER_CTRL_FIFO_EN_BIT);
}

esp_err_t mpu6500_enable_fifo(mpu6500_handle_t handle)
{
    // Enable FIFO: samples can be stored in sensor's onboard queue

    // Choose which data stream is read into FIFO. In this case, only accel data is needed
    esp_err_t err = mpu6500_register_write_byte(handle->dev_handle, MPU6500_FIFO_EN_REG,
                                                 1 << MPU6500_FIFO_EN_ACCEL_BIT);
    if (err != ESP_OK) {
        return err;
    }

    // Clear any old samples and start storing new ones automatically
    return mpu6500_fifo_restart(handle);
}

static esp_err_t mpu6500_read_fifo_count(mpu6500_handle_t handle, uint16_t *out_count)
{
    uint8_t buffer[2];
    esp_err_t err = mpu6500_register_read(handle->dev_handle, MPU6500_FIFO_COUNT_H_REG, buffer, sizeof(buffer));
    if (err != ESP_OK) {
        return err;
    }
    *out_count = ((uint16_t)buffer[0] << 8) | buffer[1];
    return ESP_OK;
}

esp_err_t mpu6500_read_fifo_samples(mpu6500_handle_t handle, mpu6500_measurements_t *out_samples,
                                     int max_samples, int *out_n_read)
{
    uint16_t fifo_bytes;
    esp_err_t err = mpu6500_read_fifo_count(handle, &fifo_bytes);
    if (err != ESP_OK) {
        return err;
    }

    /* The FIFO size isn't a multiple of the 6-byte sample, so a count at capacity means a
     * sample was partially overwritten and every read from here on would be misaligned
     * (axes shifted). Throw the contents away and start clean. */
    if (fifo_bytes >= MPU6500_FIFO_SIZE_BYTES) {
        *out_n_read = 0;
        err = mpu6500_fifo_restart(handle);
        return err != ESP_OK ? err : MPU6500_ERR_FIFO_OVERFLOW;
    }

    int available = fifo_bytes / MPU6500_FIFO_SAMPLE_BYTES;
    int remaining = available < max_samples ? available : max_samples;
    int n_read = 0;

    /* Chunked so the scratch buffer stays small on the stack no matter how
     * large a backlog (max_samples) the caller asks us to drain. */
    while (remaining > 0) {
        int chunk = remaining < MPU6500_FIFO_READ_CHUNK_SAMPLES ? remaining : MPU6500_FIFO_READ_CHUNK_SAMPLES;
        uint8_t buffer[MPU6500_FIFO_READ_CHUNK_SAMPLES * MPU6500_FIFO_SAMPLE_BYTES];
        err = mpu6500_register_read(handle->dev_handle, MPU6500_FIFO_R_W_REG, buffer,
                                     (size_t)chunk * MPU6500_FIFO_SAMPLE_BYTES);
        if (err != ESP_OK) {
            return err;
        }

        for (int i = 0; i < chunk; i++) {
            const uint8_t *sample = &buffer[i * MPU6500_FIFO_SAMPLE_BYTES];
            int16_t raw;
            raw = (int16_t)((sample[0] << 8) | sample[1]);
            out_samples[n_read].accel_x = raw / handle->accel_lsb_per_g;
            raw = (int16_t)((sample[2] << 8) | sample[3]);
            out_samples[n_read].accel_y = raw / handle->accel_lsb_per_g;
            raw = (int16_t)((sample[4] << 8) | sample[5]);
            out_samples[n_read].accel_z = raw / handle->accel_lsb_per_g;
            n_read++;
        }

        remaining -= chunk;
    }

    *out_n_read = n_read;
    return ESP_OK;
}
