FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY dashboard.py index.html ./
USER 10001:10001
EXPOSE 8080
CMD ["python", "-u", "dashboard.py"]
