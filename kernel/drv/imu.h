/* The Cardputer ADV's BMI270 motion sensor. Device-only.
 *
 * Six axes: acceleration (which way is down, and how hard it is being
 * shaken) and rotation. The original Cardputer has none; there imu_read
 * says so with -1, and an app asking explains itself.
 *
 * The sensor is started the first time it is asked for, not at boot: most
 * sessions never need it, and starting it means sending it 8 KB of
 * configuration over I2C (kernel/drv/bmi270_config.h, Bosch's).
 */
#ifndef CARDOS_IMU_H
#define CARDOS_IMU_H

#include <stdint.h>

typedef struct {
  int16_t ax, ay, az;    /* milli-g, the sensor's own axes */
  int16_t gx, gy, gz;    /* tenths of a degree a second */
} ImuSample;

/* 0 and a reading, or -1: no sensor, or it would not start. */
int imu_read(ImuSample *out);

/* Is there one? (Starts it, the first time.) */
int imu_present(void);

#endif /* CARDOS_IMU_H */
