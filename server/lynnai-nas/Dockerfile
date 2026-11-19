# lynnai-nas — NAS 모드(ESP32 허브 없이). standalone/ 의 서버를 그대로 돌린다.
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends openssl iproute2 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
# 레포의 standalone 코드를 담는다(로직은 여기 있음, 새로 짠 것 아님)
COPY standalone/ /app/standalone/
# 필요한 파일만(운영 데이터 server/lynnai-nas/data/ 에는 개인키·토큰·부트스트랩 캐시가 있어 이미지에 넣으면 안 됨 — .dockerignore 도 막음)
COPY server/lynnai-nas/*.py server/lynnai-nas/entrypoint.sh server/lynnai-nas/cloud_ca.crt /app/lynnai-nas/
RUN cp /app/lynnai-nas/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh
ENV DATA_DIR=/data
VOLUME /data
ENTRYPOINT ["/app/entrypoint.sh"]
