FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

# Render (and most PaaS hosts) inject the port to bind via $PORT.
ENV PORT=8000
EXPOSE 8000

CMD ["sh", "-c", "python scripts/init_db.py; uvicorn backend.main:app --host 0.0.0.0 --port ${PORT}"]
