FROM python:3.12-slim
WORKDIR /app
COPY lynnai_hub_watch.py .
ENV TZ=Asia/Seoul HUB_DB=/data/hub.db
VOLUME /data
CMD ["python", "-u", "lynnai_hub_watch.py"]
