#include "mpu6050.h"
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

static const char *TAG = "mpu6050";

#define MPU6050_TIMEOUT_MS          1000

#define MPU6050_SENSOR_ADDR         0x68        /*!< Address of the MPU6050 sensor */
#define MPU6050_WHO_AM_I_REG_ADDR   0x75        /*!< Register addresses of the "who am I" register */
#define MPU6050_PWR_MGMT_1_REG_ADDR 0x6B        /*!< Register addresses of the power management register */
#define MPU6050_RESET_BIT           7
#define MPU6050_ACCEL_XOUT          0x3B // accel registers read from 0x3B to 0x40, x to y to z, each one using 2 bytes
#define MPU6050_SMPLRT_DIV_REG      0x19 // sample rate divider
#define MPU6050_CONFIG_REG          0x1A // general config register, holds the DLPF setting
#define MPU6050_ACCEL_CONFIG_REG    0x1C // accelerometer full-scale range (AFS_SEL lives in bits 4:3)
#define MPU6050_ACCEL_CONFIG_AFS_SEL_SHIFT 3

struct mpu6050_dev_t {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    float accel_lsb_per_g;
};

static esp_err_t mpu6050_register_read(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_handle, &reg_addr, 1, data, len, MPU6050_TIMEOUT_MS);
}

static esp_err_t mpu6050_register_write_byte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(dev_handle, write_buf, sizeof(write_buf), MPU6050_TIMEOUT_MS);
}

/**
 * @brief LSB-per-g sensitivity for a given accelerometer full-scale range.
 * This is the single place that knows "AFS_SEL=1 means +/-4g means 8192 LSB/g" -
 * both the register write and the raw-to-g conversion pull from here.
 */
static float mpu6050_accel_fs_lsb_per_g(mpu6050_accel_fs_t fs)
{
    static const float lsb_per_g[] = {
        [MPU6050_ACCEL_FS_2G]  = 16384.0f,
        [MPU6050_ACCEL_FS_4G]  = 8192.0f,
        [MPU6050_ACCEL_FS_8G]  = 4096.0f,
        [MPU6050_ACCEL_FS_16G] = 2048.0f,
    };
    return lsb_per_g[fs];
}

esp_err_t mpu6050_init(const mpu6050_config_t *config, mpu6050_handle_t *out_handle)
{
    struct mpu6050_dev_t *dev = calloc(1, sizeof(*dev));
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

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU6050_SENSOR_ADDR,
        .scl_speed_hz = config->i2c_freq_hz,
    };
    err = i2c_master_bus_add_device(dev->bus_handle, &dev_config, &dev->dev_handle);
    if (err != ESP_OK) {
        i2c_del_master_bus(dev->bus_handle);
        free(dev);
        return err;
    }

    /* Read the MPU6050 WHO_AM_I register, on power up the register should have the value 0x68 */
    uint8_t who_am_i;
    err = mpu6050_register_read(dev->dev_handle, MPU6050_WHO_AM_I_REG_ADDR, &who_am_i, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WHO_AM_I - check wiring, power, and pull-ups");
        goto fail;
    }
    ESP_LOGI(TAG, "WHO_AM_I = 0x%02X", who_am_i);

    /* Reset the device, then clear the SLEEP bit it powers up (and comes out of
     * reset) with - SLEEP must be cleared explicitly before it produces data. */
    err = mpu6050_register_write_byte(dev->dev_handle, MPU6050_PWR_MGMT_1_REG_ADDR, 1 << MPU6050_RESET_BIT);
    if (err != ESP_OK) {
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    err = mpu6050_register_write_byte(dev->dev_handle, MPU6050_PWR_MGMT_1_REG_ADDR, 0);
    if (err != ESP_OK) {
        goto fail;
    }

    /* --- Vibration-monitoring configuration ---
     * Power-on defaults aren't tuned for reading conveyor/bearing vibration,
     * so set them explicitly. These three settings interact (DLPF picks the
     * internal rate that SMPLRT_DIV divides down), so they're grouped here. */

    /* DLPF_CFG = 1 selects the second DLPF setting (see datasheet table),
     * giving roughly a 184Hz bandwidth. That's wide enough to pass typical
     * bearing-fault harmonics without them being smoothed away, and it also
     * sets the sensor's internal sample rate to 1kHz (used just below). */
    err = mpu6050_register_write_byte(dev->dev_handle, MPU6050_CONFIG_REG, 1);
    if (err != ESP_OK) {
        goto fail;
    }

    /* Output sample rate = 1kHz / (1 + SMPLRT_DIV) while the DLPF above is
     * enabled. SMPLRT_DIV = 1 gives 1000 / (1 + 1) = 500Hz, which is well
     * above 2x the ~184Hz DLPF cutoff so nothing above that cutoff aliases
     * back down into the frequency range we're actually looking at. */
    err = mpu6050_register_write_byte(dev->dev_handle, MPU6050_SMPLRT_DIV_REG, 1);
    if (err != ESP_OK) {
        goto fail;
    }

    err = mpu6050_register_write_byte(dev->dev_handle, MPU6050_ACCEL_CONFIG_REG,
                                       config->accel_fs << MPU6050_ACCEL_CONFIG_AFS_SEL_SHIFT);
    if (err != ESP_OK) {
        goto fail;
    }

    dev->accel_lsb_per_g = mpu6050_accel_fs_lsb_per_g(config->accel_fs);

    *out_handle = dev;
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(dev->dev_handle);
    i2c_del_master_bus(dev->bus_handle);
    free(dev);
    return err;
}

esp_err_t mpu6050_read_accel(mpu6050_handle_t handle, mpu6050_measurements_t *out_measurements)
{
    uint8_t buffer[6];
    esp_err_t err = mpu6050_register_read(handle->dev_handle, MPU6050_ACCEL_XOUT, buffer, sizeof(buffer));
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
