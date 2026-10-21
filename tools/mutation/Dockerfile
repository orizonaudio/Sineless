# Ubuntu 24.04 as on CI, with its Clang 18, Node (and npm, for the packages the editor's page is
# built with) and Mull built for that Clang: what scripts/mutation-cpp.sh needs where Mull is not
# installed (on macOS, whose Homebrew LLVM is newer than any Mull supports). Built from
# tools/mutation.
# Pinned by digest, so the image cannot change under the tag; Dependabot proposes new digests.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
# The oldest Node the build accepts (protocol/CMakeLists.txt): the first 22 to run .ts files
# without a flag. CI takes the newest of .nvmrc's major instead; both meet that minimum.
# The Node checksums below are for this version.
ARG NODE=22.18.0
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang cmake ninja-build git curl ca-certificates xz-utils && \
    rm -rf /var/lib/apt/lists/*
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
COPY install-mull.sh /tmp/
RUN /tmp/install-mull.sh && rm -rf /tmp/install-mull.sh /var/lib/apt/lists/*
RUN git config --system --add safe.directory '*'
