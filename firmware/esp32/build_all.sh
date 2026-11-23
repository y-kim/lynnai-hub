#!/bin/sh
# 허브 펌웨어를 빌드한다(ESP-IDF 환경에서 실행).
#
#   ./build_all.sh                   빌드
#   ./build_all.sh flash <포트>      빌드하고 굽기(부트로더·파티션 표 포함)
#   ./build_all.sh release <버전>    빌드하고 릴리즈 이미지 만들기(전체 한 파일 + 앱 + 파티션 표)
set -e
cd "$(dirname "$0")"
idf.py build
case "$1" in
flash)
    PORT=${2:?포트를 주세요(예: /dev/ttyUSB0)}
    idf.py -p "$PORT" flash
    ;;
release)
    VER=${2:?버전을 주세요}
    (cd build && python -m esptool --chip esp32c6 merge_bin -o "../lynnai-hub-esp32c6-$VER-full.bin" @flash_args)
    cp build/lynnai_hub.bin "lynnai-hub-esp32c6-$VER-app.bin"
    cp build/partition_table/partition-table.bin "lynnai-hub-esp32c6-$VER-partitions.bin"
    echo "만든 파일: lynnai-hub-esp32c6-$VER-full.bin(0x0) · -app.bin(0x20000) · -partitions.bin(0x8000)"
    ;;
esac
