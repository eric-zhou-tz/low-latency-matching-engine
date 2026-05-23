# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS base

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        git \
        ninja-build \
        python3 \
        unzip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

FROM base AS build

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY benchmarks ./benchmarks
COPY toy ./toy

RUN cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG"
RUN cmake --build build

FROM build AS validation

CMD ["./build/matching_engine"]

FROM build AS dev

CMD ["/bin/bash"]

FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=build /workspace/build/matching_engine /usr/local/bin/matching_engine
COPY tests/replay_cli.txt ./tests/replay_cli.txt

ENTRYPOINT ["matching_engine"]
