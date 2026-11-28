FROM python:3.12-slim
WORKDIR /app
# PostgreSQL 에 쓸 때(HUB_DB_URL)만 쓰는 드라이버 — 안 주면 SQLite 파일(/data/hub.db)
RUN pip install --no-cache-dir "psycopg[binary]==3.2.3"
COPY lynnai_hub_watch.py .
ENV TZ=Asia/Seoul HUB_DB=/data/hub.db
VOLUME /data
CMD ["python", "-u", "lynnai_hub_watch.py"]
