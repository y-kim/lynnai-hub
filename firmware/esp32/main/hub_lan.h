#pragma once
#include <stdint.h>

/* LAN 통로(SSDP 찾기 + TCP 8484 줄 단위). 형식은 hub_lan.c 맨 위. */
void hub_lan_start(const char *device_id);
void hub_lan_send(uint8_t cmd, const uint8_t *data, uint16_t n);
void hub_lan_pairing_open(int seconds);      /* 새 드라이버 등록 허용(버튼 짧게) */
