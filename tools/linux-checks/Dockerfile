# Ubuntu 24.04 as on CI, with LLVM 23 installed as CI's Linux jobs install it (install-llvm.sh),
# CMake, Ninja, Node and Valgrind: what scripts/check-linux.sh and scripts/engine-work.sh need
# where they do not run on Linux (on macOS, scripts/check.sh runs the first here). Built from
# tools/linux-checks. Valgrind is Ubuntu's own package, which cannot be pinned by hash.
# Pinned by digest, so the image cannot change under the tag; Dependabot proposes new digests.
FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac
# The oldest Node the build accepts (protocol/CMakeLists.txt), as in tools/mutation/Dockerfile.
# The Node checksums below are for this version.
ARG NODE=22.18.0
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        cmake ninja-build git curl ca-certificates xz-utils valgrind && \
    rm -rf /var/lib/apt/lists/*
COPY install-llvm.sh /tmp/
RUN /tmp/install-llvm.sh && rm -rf /tmp/install-llvm.sh /var/lib/apt/lists/*
# Checked against nodejs.org's SHASUMS256.txt for this version, as install-mull.sh checks Mull.
RUN case "$(dpkg --print-architecture)" in \
        amd64) nodeArch=x64 sum=c1bfeecf1d7404fa74728f9db72e697decbd8119ccc6f5a294d795756dfcfca7 ;; \
        arm64) nodeArch=arm64 sum=04fca1b9afecf375f26b41d65d52aa1703a621abea5a8948c7d1e351e85edade ;; \
    esac && \
    tarball="/tmp/node-v$NODE-linux-$nodeArch.tar.xz" && \
    curl -fsSL -o "$tarball" "https://nodejs.org/dist/v$NODE/node-v$NODE-linux-$nodeArch.tar.xz" && \
    echo "$sum  $tarball" | sha256sum -c - && \
    tar -xJf "$tarball" -C /opt && rm "$tarball" && \
    ln -s "/opt/node-v$NODE-linux-$nodeArch/bin/node" /usr/local/bin/node && \
    ln -s "/opt/node-v$NODE-linux-$nodeArch/bin/npm" /usr/local/bin/npm
RUN git config --system --add safe.directory '*'
