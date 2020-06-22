# --- builder: compiles the C decoder and Go gateway; `docker compose run tests` runs everything
FROM golang:1.24-bookworm AS builder
RUN apt-get update && apt-get install -y --no-install-recommends make python3 python3-venv \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make all
CMD ["make", "test"]

# --- gateway runtime
FROM debian:bookworm-slim AS gateway
COPY --from=builder /src/build/gateway /src/build/bw_decode /usr/local/bin/
COPY data /data
ENV BW_DECODER=/usr/local/bin/bw_decode BW_LOG=/data/sample.log
ENTRYPOINT ["/usr/local/bin/gateway"]

# --- demo HTTP receiver (the Python test double, stdlib only)
FROM python:3.12-slim AS receiver
WORKDIR /app
COPY harness/buswhisperer_hil ./buswhisperer_hil
EXPOSE 8080
CMD ["python", "-m", "buswhisperer_hil.receiver"]
