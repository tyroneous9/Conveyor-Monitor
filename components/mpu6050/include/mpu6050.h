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

/**
 * @brief Enable the sensor's DATA_RDY interrupt (fires once per internal
 * sample, at the rate mpu6050_init() configured via SMPLRT_DIV). The
 * device's INT pin should be wired to a GPIO configured for edge-triggered
 * interrupts by the caller -- this only turns on the interrupt source
 * inside the sensor itself.
 */
esp_err_t mpu6050_enable_data_ready_interrupt(mpu6050_handle_t handle);

/**
 * @brief Enable the sensor's onboard accelerometer FIFO (and reset it, so
 * the first mpu6050_read_fifo_samples() call only sees samples captured
 * after this point). With the FIFO enabled, a caller that wakes up late
 * (e.g. a DATA_RDY-driven task delayed by scheduling) can still recover
 * every sample that piled up in the meantime, instead of the accelerometer
 * registers having already been overwritten by the newest one.
 */
esp_err_t mpu6050_enable_fifo(mpu6050_handle_t handle);

/**
 * @brief Drain up to max_samples accelerometer samples currently buffered
 * in the sensor's FIFO into out_samples (oldest first), converted to g.
 * *out_n_read is set to how many were actually available (0 if the FIFO
 * was empty) -- always <= max_samples.
 */
esp_err_t mpu6050_read_fifo_samples(mpu6050_handle_t handle, mpu6050_measurements_t *out_samples,
                                     int max_samples, int *out_n_read);

#ifdef __cplusplus
}
#endif
