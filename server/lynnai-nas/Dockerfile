# lynnai-nas — NAS 모드(ESP32 허브 없이). standalone/ 의 서버를 그대로 돌린다.
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends openssl iproute2 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
# 레포의 standalone 코드를 담는다(로직은 여기 있음, 새로 짠 것 아님)
COPY standalone/ /app/standalone/
COPY server/lynnai-nas/ /app/lynnai-nas/
RUN cp /app/lynnai-nas/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh
ENV DATA_DIR=/data
VOLUME /data
ENTRYPOINT ["/app/entrypoint.sh"]
