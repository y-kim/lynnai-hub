#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * hub_radio -- Zigbee(802.15.4) 라디오 고르기: 칩에 내장된 라디오 / UART 로 붙인 외부 라디오(RCP: ESP32-C6·H2 에 ot_rcp).
 *
 * Zigbee 라이브러리는 두 방식을 빌드할 때 하나만 넣을 수 있다(같은 이름의 함수를 각각 갖고 있어 한 펌웨어에 같이 못 넣는다).
 * 그래서 펌웨어를 두 벌 빌드해 앱 영역 두 칸에 하나씩 넣고(ota_0 = 내장용, ota_1 = 외부용), 부팅 때 외부 라디오가
 * 답하는지 보고 맞는 칸으로 넘어간다:
 *   - UART(HUB_RCP_UART_*)로 Spinel "프로토콜 버전" 을 물어 0.75초 안에 답이 오면 '있음'
 *   - 지금 펌웨어가 그것과 안 맞으면 다른 칸의 펌웨어 종류를 확인하고(이미지 안의 표시) 부팅 칸을 바꿔 재시작
 *   - 다른 칸에 맞는 펌웨어가 없으면 그대로 간다(외부용 펌웨어인데 라디오가 없으면 Zigbee 를 쓰지 않는다)
 * 설정과 Zigbee 가입 정보는 두 펌웨어가 같은 저장소를 쓴다. Zigbee 주소(EUI)는 호스트 칩 것으로 고정해
 * (hub_main) 라디오를 붙였다 떼도 SmartThings 에는 같은 기기로 남는다.
 */

/* 외부 라디오(RCP) UART. 두 보드를 TX↔RX 로 엇갈려 잇고 GND 를 같이 묶는다. RCP 쪽은 ot_rcp(UART, 같은 속도). */
#define HUB_RCP_UART_PORT  1
#define HUB_RCP_UART_BAUD  460800
#if CONFIG_IDF_TARGET_ESP32C6      /* C6 호스트: 16·17 은 콘솔 UART, 4·5·8·9·15 는 부팅 설정 핀이라 피한다 */
#define HUB_RCP_UART_TX    2       /* 호스트 TX → RCP RX */
#define HUB_RCP_UART_RX    3       /* 호스트 RX ← RCP TX */
#else                              /* ESP Thread Border Router 보드(S3 + H2) 기준 — 회로도로 확인할 것 */
#define HUB_RCP_UART_TX    17
#define HUB_RCP_UART_RX    18
#endif

void hub_radio_boot(void);        /* 저장소를 연 직후: 탐지 → 필요하면 다른 펌웨어로 넘어가 재시작(그 경우 돌아오지 않는다) */
bool hub_radio_usable(void);      /* 이 펌웨어의 라디오를 쓸 수 있나(외부용 펌웨어인데 라디오 칩이 없으면 false) */
bool hub_radio_external(void);    /* 이 펌웨어가 외부 라디오용인가 */
void hub_radio_print(void);       /* USB diag */

/* 시험용 고정(USB 명령 radio auto|native|ext): 탐지 결과와 상관없이 한쪽 라디오만 쓴다. 저장되며 다음 부팅부터 적용.
 * ext 로 고정했는데 외부 라디오가 답하지 않으면 내장으로 돌아가지 않고 Zigbee 를 쓰지 않는다(측정이 섞이지 않게). */
#define HUB_RADIO_AUTO    0
#define HUB_RADIO_NATIVE  1
#define HUB_RADIO_EXT     2
bool hub_radio_force(uint8_t f);
/* 배선 시험(USB 명령 radio test, 약 9초): 허브가 양쪽 핀에서 라디오 신호를 들어 보고 어느 선이 문제인지 알린다.
 * 그동안 라디오 보드를 리셋해 주어야 한다(리셋하면 시작 신호를 낸다). */
void hub_radio_wiretest(void);
