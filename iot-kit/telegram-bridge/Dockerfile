FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY bridge.py .
# /data garde la liste des SOS déjà traités ; le volume Docker hérite de ce propriétaire.
RUN mkdir /data && chown 10001:10001 /data
USER 10001:10001
CMD ["python", "-u", "bridge.py"]
