#!/bin/sh
# 허브 펌웨어를 빌드한다(ESP-IDF 환경에서 실행).
#
#   ./build_all.sh                   빌드
#   ./build_all.sh flash <포트>      빌드하고 굽기(부트로더·파티션 표 포함)
#   ./build_all.sh release <버전>    빌드하고 릴리즈 이미지 만들기(전체 한 파일 + 앱 + 파티션 표)
set -e
cd "$(dirname "$0")"
if [ "$1" = release ]; then
    # 릴리즈는 커밋된 코드로만: 펌웨어 버전은 빌드 때의 git describe 라, 고친 채 구우면 같은 "vX.Y" 표시에
    # 내용이 다른 이미지가 나온다(검증할 수 없음)
    if [ -n "$(git status --porcelain --untracked-files=no -- . ../../edge-driver ../../edge-driver-lan 2>/dev/null)" ]; then
        echo "커밋 안 된 변경이 있습니다 — 릴리즈는 깨끗한 트리에서만(git status 확인)" >&2
        exit 1
    fi
    idf.py reconfigure >/dev/null       # 버전 문자열(git describe)을 지금 커밋으로 새로 넣게
fi
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
    sha256sum "lynnai-hub-esp32c6-$VER-full.bin" "lynnai-hub-esp32c6-$VER-app.bin" "lynnai-hub-esp32c6-$VER-partitions.bin" \
        > "lynnai-hub-esp32c6-$VER-SHA256SUMS"
    echo "만든 파일: lynnai-hub-esp32c6-$VER-full.bin(0x0) · -app.bin(0x20000) · -partitions.bin(0x8000) · -SHA256SUMS"
    echo "릴리즈에 SHA256SUMS 도 같이 올리세요(받은 쪽이 sha256sum -c 로 확인)"
    ;;
esac
