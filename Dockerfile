# Stage 1: Build environment using official Ubuntu 24.04 image
FROM ubuntu:24.04 AS builder

WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    make \
    && rm -rf /var/lib/apt/lists/*

COPY src/ src/
RUN mkdir -p build && g++ -O3 src/main.cpp -o build/iot_gateway_server -lpthread

# Stage 2: Minimal OCI runtime container
FROM ubuntu:24.04 AS runtime

WORKDIR /app
COPY --from=builder /app/build/iot_gateway_server /app/iot_gateway_server

EXPOSE 8080
CMD ["./iot_gateway_server"]
