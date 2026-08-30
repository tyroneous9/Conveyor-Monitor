#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Accelerometer full-scale range. Values match the sensor's AFS_SEL field
 * exactly, so a setting can be written to ACCEL_CONFIG with a plain shift. */
typedef enum {
    MPU6050_ACCEL_FS_2G  = 0,
    MPU6050_ACCEL_FS_4G  = 1,
    MPU6050_ACCEL_FS_8G  = 2,
    MPU6050_ACCEL_FS_16G = 3,
} mpu6050_accel_fs_t;

typedef struct {
    float accel_x, accel_y, accel_z;
} mpu6050_measurements_t;

typedef struct {
    int sda_io_num;
    int scl_io_num;
    uint32_t i2c_freq_hz;
    mpu6050_accel_fs_t accel_fs;
} mpu6050_config_t;

typedef struct mpu6050_dev_t *mpu6050_handle_t;

/**
 * @brief Bring up the I2C bus, attach the MPU6050, and configure it for
 * vibration monitoring (DLPF + sample rate tuned for bearing-fault frequencies).
 * On success *out_handle is ready to pass to mpu6050_read_accel().
 */
esp_err_t mpu6050_init(const mpu6050_config_t *config, mpu6050_handle_t *out_handle);

/**
 * @brief Read the acceleration XYZ measurement, in g.
 */
esp_err_t mpu6050_read_accel(mpu6050_handle_t handle, mpu6050_measurements_t *out_measurements);

#ifdef __cplusplus
}
#endif
