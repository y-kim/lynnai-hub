#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * hub_room_calc -- 온습도 센서 값 계산(하드웨어 없이 PC 에서 시험할 수 있게 계산만 떼어 둔 것). 쓰는 곳은 hub_sensors.c.
 */

/* BME280/BMP280 보정값(칩마다 다름, 칩에서 읽는다). hum = 습도가 있는 칩(BME280) */
typedef struct {
    uint16_t T1;
    int16_t T2, T3;
    uint8_t H1, H3;
    int16_t H2, H4, H5;
    int8_t H6;
    bool hum;
} bme_cal_t;

/* 보정값 묶음 풀기: c = 0x88 부터 6바이트, h1 = 0xA1, h = 0xE1 부터 7바이트 */
static inline void bme280_unpack(bme_cal_t *k, const uint8_t *c, uint8_t h1, const uint8_t *h)
{
    k->T1 = (uint16_t)(c[0] | (c[1] << 8));
    k->T2 = (int16_t)(c[2] | (c[3] << 8));
    k->T3 = (int16_t)(c[4] | (c[5] << 8));
    k->H1 = h1;
    k->H2 = (int16_t)(h[0] | (h[1] << 8));
    k->H3 = h[2];
    k->H4 = (int16_t)((int8_t)h[3] * 16 + (h[4] & 0x0f));       /* 12비트, 부호 있음 */
    k->H5 = (int16_t)((int8_t)h[5] * 16 + (h[4] >> 4));
    k->H6 = (int8_t)h[6];
}

/* 원시값 → 섭씨·상대습도(%). 데이터시트(BST-BME280-DS002)의 부동소수점 식 그대로 */
static inline void bme280_comp(const bme_cal_t *k, int32_t adc_t, int32_t adc_h, float *t, float *h)
{
    double v1 = (adc_t / 16384.0 - k->T1 / 1024.0) * k->T2;
    double v2 = (adc_t / 131072.0 - k->T1 / 8192.0) * (adc_t / 131072.0 - k->T1 / 8192.0) * k->T3;
    double t_fine = v1 + v2;
    *t = (float)(t_fine / 5120.0);
    double x = t_fine - 76800.0;
    x = (adc_h - (k->H4 * 64.0 + k->H5 / 16384.0 * x)) *
        (k->H2 / 65536.0 * (1.0 + k->H6 / 67108864.0 * x * (1.0 + k->H3 / 67108864.0 * x)));
    x = x * (1.0 - k->H1 * x / 524288.0);
    *h = (float)(x > 100.0 ? 100.0 : x < 0.0 ? 0.0 : x);
}

/* MPL3115A2 온도: 위 바이트는 부호 있는 정수 도, 아래 바이트의 위 4비트가 1/16 도 */
static inline float mpl3115_temp(uint8_t msb, uint8_t lsb) { return (int8_t)msb + (lsb >> 4) / 16.0f; }

