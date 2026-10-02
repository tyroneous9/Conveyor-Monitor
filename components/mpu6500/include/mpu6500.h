#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Accelerometer full-scale range. Values match the sensor's AFS_SEL field
 * exactly, so a setting can be written to ACCEL_CONFIG with a plain shift. */
typedef enum {
    MPU6500_ACCEL_FS_2G  = 0,
    MPU6500_ACCEL_FS_4G  = 1,
    MPU6500_ACCEL_FS_8G  = 2,
    MPU6500_ACCEL_FS_16G = 3,
} mpu6500_accel_fs_t;

/* Internal sample clock with the DLPF enabled; the output rate is this
 * divided by (1 + SMPLRT_DIV), so sample_rate_hz must divide it evenly. */
#define MPU6500_BASE_RATE_HZ 1000

/* Returned by mpu6500_read_fifo_samples() when the FIFO overflowed. */
#define MPU6500_ERR_FIFO_OVERFLOW ESP_ERR_INVALID_STATE

typedef struct {
    float accel_x, accel_y, accel_z;
} mpu6500_measurements_t;

typedef struct {
    int sda_io_num;
    int scl_io_num;
    uint32_t i2c_freq_hz;
    mpu6500_accel_fs_t accel_fs;
    // Must evenly divide MPU6500_BASE_RATE_HZ (e.g. 1000, 500, 250, 200, 100)
    uint32_t sample_rate_hz;
} mpu6500_config_t;

typedef struct mpu6500_dev_t *mpu6500_handle_t;

/**
 * @brief Bring up the I2C bus, attach the MPU6500, and configure it for
 * vibration monitoring (DLPF + config->sample_rate_hz). Returns
 * ESP_ERR_INVALID_ARG if sample_rate_hz doesn't divide MPU6500_BASE_RATE_HZ.
 * An unexpected WHO_AM_I value is only logged as a warning.
 * On success *out_handle is ready to pass to mpu6500_read_accel().
 */
esp_err_t mpu6500_init(const mpu6500_config_t *config, mpu6500_handle_t *out_handle);

/**
 * @brief Read the acceleration XYZ measurement, in g.
 */
esp_err_t mpu6500_read_accel(mpu6500_handle_t handle, mpu6500_measurements_t *out_measurements);

/**
 * @brief Enable the sensor's DATA_RDY interrupt (fires once per internal
 * sample, at the rate mpu6500_init() configured via SMPLRT_DIV). The
 * device's INT pin should be wired to a GPIO configured for edge-triggered
 * interrupts by the caller -- this only turns on the interrupt source
 * inside the sensor itself.
 */
esp_err_t mpu6500_enable_data_ready_interrupt(mpu6500_handle_t handle);

/**
 * @brief Enable the sensor's onboard accelerometer FIFO (and reset it, so
 * the first mpu6500_read_fifo_samples() call only sees samples captured
 * after this point). With the FIFO enabled, a caller that wakes up late
 * (e.g. a DATA_RDY-driven task delayed by scheduling) can still recover
 * every sample that piled up in the meantime, instead of the accelerometer
 * registers having already been overwritten by the newest one.
 */
esp_err_t mpu6500_enable_fifo(mpu6500_handle_t handle);

/**
 * @brief Drain up to max_samples accelerometer samples currently buffered
 * in the sensor's FIFO into out_samples (oldest first), converted to g.
 * *out_n_read is set to how many were actually available (0 if the FIFO
 * was empty) -- always <= max_samples.
 * Returns MPU6500_ERR_FIFO_OVERFLOW if the FIFO filled up before it was
 * drained (512 bytes on the MPU6500): samples were
 * lost, so the FIFO has been reset and *out_n_read is 0. Samples read
 * before and after an overflow are not contiguous.
 */
esp_err_t mpu6500_read_fifo_samples(mpu6500_handle_t handle, mpu6500_measurements_t *out_samples,
                                     int max_samples, int *out_n_read);

#ifdef __cplusplus
}
#endif
