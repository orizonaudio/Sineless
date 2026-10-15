# Ubuntu 24.04 as on CI, with LLVM 23 installed as CI's Linux jobs install it (install-llvm.sh),
# CMake, Ninja and Node: what scripts/check-linux.sh needs where it does not run on Linux (on
# macOS, scripts/check.sh runs it here). Built from tools/linux-checks.
# Pinned by digest, so the image cannot change under the tag; Dependabot proposes new digests.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
# The oldest Node the build accepts (protocol/CMakeLists.txt), as in tools/mutation/Dockerfile.
ARG NODE=22.18.0
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        cmake ninja-build git curl ca-certificates xz-utils && \
    rm -rf /var/lib/apt/lists/*
COPY install-llvm.sh /tmp/
RUN /tmp/install-llvm.sh && rm -rf /tmp/install-llvm.sh /var/lib/apt/lists/*
RUN arch=$(dpkg --print-architecture) && \
    nodeArch=$([ "$arch" = arm64 ] && echo arm64 || echo x64) && \
    curl -fsSL "https://nodejs.org/dist/v$NODE/node-v$NODE-linux-$nodeArch.tar.xz" | tar -xJ -C /opt && \
    ln -s "/opt/node-v$NODE-linux-$nodeArch/bin/node" /usr/local/bin/node && \
    ln -s "/opt/node-v$NODE-linux-$nodeArch/bin/npm" /usr/local/bin/npm
RUN git config --system --add safe.directory '*'
