#!/bin/sh
# 허브 펌웨어 두 벌을 빌드한다(ESP-IDF 환경에서 실행):
#   build/      내장 라디오용  → 앱 영역 ota_0 (0x20000)
#   build_rcp/  외부 라디오용  → 앱 영역 ota_1 (0x320000)   (UART 로 붙인 C6/H2 를 Zigbee 라디오로)
# 부팅할 때 외부 라디오가 있는지 보고 맞는 쪽으로 넘어간다(main/hub_radio.h).
#
#   ./build_all.sh                   둘 다 빌드
#   ./build_all.sh flash <포트>      빌드하고 둘 다 굽기(부트로더·파티션 표 포함)
#   ./build_all.sh release <버전>    빌드하고 릴리즈 이미지 만들기(전체 한 파일 + 앱 두 개)
set -e
cd "$(dirname "$0")"
idf.py build
idf.py -B build_rcp -DSDKCONFIG=build_rcp/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.rcp" build
case "$1" in
flash)
    PORT=${2:?포트를 주세요(예: /dev/ttyUSB0)}
    idf.py -p "$PORT" flash
    python -m esptool --chip esp32c6 -p "$PORT" write_flash 0x320000 build_rcp/lynnai_hub.bin
    ;;
release)
    VER=${2:?버전을 주세요}
    (cd build && python -m esptool --chip esp32c6 merge_bin -o "../lynnai-hub-esp32c6-$VER-full.bin" @flash_args \
        0x320000 ../build_rcp/lynnai_hub.bin)
    cp build/lynnai_hub.bin "lynnai-hub-esp32c6-$VER-app.bin"
    cp build_rcp/lynnai_hub.bin "lynnai-hub-esp32c6-$VER-app-rcp.bin"
    cp build/partition_table/partition-table.bin "lynnai-hub-esp32c6-$VER-partitions.bin"
    echo "만든 파일: lynnai-hub-esp32c6-$VER-full.bin(0x0) · -app.bin(0x20000) · -app-rcp.bin(0x320000) · -partitions.bin(0x8000)"
    ;;
esac
