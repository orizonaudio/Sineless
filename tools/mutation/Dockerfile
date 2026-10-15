# Ubuntu 24.04 as on CI, with its Clang 18, Node and Mull built for that Clang: what
# scripts/mutation-cpp.sh needs where Mull is not installed (on macOS, whose Homebrew LLVM is
# newer than any Mull supports). Built from tools/mutation.
# Pinned by digest, so the image cannot change under the tag; Dependabot proposes new digests.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
# The oldest Node the build accepts (protocol/CMakeLists.txt): the first 22 to run .ts files
# without a flag. CI takes the newest of .nvmrc's major instead; both meet that minimum.
ARG NODE=22.18.0
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang cmake ninja-build git curl ca-certificates xz-utils && \
    rm -rf /var/lib/apt/lists/*
RUN arch=$(dpkg --print-architecture) && \
    nodeArch=$([ "$arch" = arm64 ] && echo arm64 || echo x64) && \
    curl -fsSL "https://nodejs.org/dist/v$NODE/node-v$NODE-linux-$nodeArch.tar.xz" | tar -xJ -C /opt && \
    ln -s "/opt/node-v$NODE-linux-$nodeArch/bin/node" /usr/local/bin/node
COPY install-mull.sh /tmp/
RUN /tmp/install-mull.sh && rm -rf /tmp/install-mull.sh /var/lib/apt/lists/*
RUN git config --system --add safe.directory '*'
