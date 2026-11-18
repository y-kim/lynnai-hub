# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(PREPEND SDKCONFIG_DEFAULTS "components/example_common/sdkconfig.mbedtls_minimal" "sdkconfig.defaults")

set(COMPONENTS main)

project(lynnai_hub)

# wolfSSL 설정은 레포의 wolfssl/user_settings.h 가 원본이다(매니지드 컴포넌트는 다시 받으면 기본값으로 돌아감).
# 바뀐 점: NO_OLD_TLS 끔(보일러 TLS1.1) · WOLFSSL_STATIC_RSA · SNI 강제 · RSA_LOW_MEM 끔(TLS 4.2→1.4초)
#          · ESP32 하드웨어 가속 전부 끔(SHA 가속은 RNG 자가검사를 깨고, RSA 가속은 클라우드 쪽 TLS 를 깨뜨림 — 실측)
set(LYNNAI_WOLFSSL_SETTINGS "${CMAKE_SOURCE_DIR}/wolfssl/user_settings.h")
set(LYNNAI_WOLFSSL_TARGET "${CMAKE_SOURCE_DIR}/managed_components/wolfssl__wolfssl/include/user_settings.h")
if(EXISTS "${LYNNAI_WOLFSSL_TARGET}")
    file(COPY_FILE "${LYNNAI_WOLFSSL_SETTINGS}" "${LYNNAI_WOLFSSL_TARGET}" ONLY_IF_DIFFERENT)
else()
    message(FATAL_ERROR "wolfssl 매니지드 컴포넌트가 없습니다 — idf.py reconfigure 를 한 번 더")
endif()
