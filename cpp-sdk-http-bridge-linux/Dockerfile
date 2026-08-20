# 固定 Debian 12：与 HCNetSDK V6.1.11.5 附带的 OpenSSL 3 库匹配，并隔离宿主发行版差异。
FROM debian:12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY CMakeLists.txt ./
COPY src ./src
COPY vendor/hcnetsdk ./vendor/hcnetsdk
RUN cmake -S . -B out -DCMAKE_BUILD_TYPE=Release && cmake --build out --parallel

FROM debian:12-slim
LABEL org.opencontainers.image.licenses="Apache-2.0"
RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg ca-certificates tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home --home-dir /nonexistent hikbridge
WORKDIR /opt/hik-bridge
COPY --from=build /build/out/hik-sdk-http-bridge /opt/hik-bridge/bin/hik-sdk-http-bridge
COPY vendor/hcnetsdk/sdk /opt/hik-bridge/sdk
COPY config/config.json /opt/hik-bridge/config/config.json
COPY vendor/licenses /opt/hik-bridge/licenses
RUN cp /usr/share/common-licenses/Apache-2.0 /opt/hik-bridge/licenses/PROJECT-APACHE-2.0.txt \
    && printf '%s\n' 'hik-hcnet-http-bridge' 'Copyright 2026 davideward5340' > /opt/hik-bridge/NOTICE \
    && chmod 0755 /opt/hik-bridge/bin/hik-sdk-http-bridge \
    && chown -R hikbridge:hikbridge /opt/hik-bridge
ENV LD_LIBRARY_PATH=/opt/hik-bridge/sdk:/opt/hik-bridge/sdk/HCNetSDKCom
USER hikbridge
EXPOSE 28080
ENTRYPOINT ["/opt/hik-bridge/bin/hik-sdk-http-bridge", "--config", "/opt/hik-bridge/config/config.json"]
