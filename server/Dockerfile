FROM python:3.12-slim
WORKDIR /app
COPY matchmaker.py .
EXPOSE 7777/udp
CMD ["python", "-u", "matchmaker.py"]
