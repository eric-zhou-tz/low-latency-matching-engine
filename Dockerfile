# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        ninja-build \
        unzip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY examples ./examples

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build
RUN ctest --test-dir build --output-on-failure

FROM build AS dev

CMD ["/bin/bash"]

FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=build /workspace/build/matching_engine /usr/local/bin/matching_engine
COPY examples ./examples

ENTRYPOINT ["matching_engine"]
