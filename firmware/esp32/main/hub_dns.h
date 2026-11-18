#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 허브 AP 쪽 DNS 응답기 시작. hub_ip_nbo = 허브 AP IP(네트워크 바이트 순서). */
void hub_dns_start(uint32_t hub_ip_nbo, bool captive);

/* 캡티브(모든 이름 → 허브) ↔ 클라우드 이름만 전환 */
void hub_dns_set_captive(bool captive);
