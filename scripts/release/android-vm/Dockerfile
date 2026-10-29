# Optional Linux ARM64 operator image. Chromium's pinned Linux x86-64 tools
# still require the VM's existing binfmt/QEMU support. No signing secrets here.
FROM ruby@sha256:b6d266e975e092f85955ac055dae86f07d57cf20c99859adaf09cd0bd13029dd

RUN sed -i 's|http://deb.debian.org|https://deb.debian.org|g' /etc/apt/sources.list.d/debian.sources \
    && dpkg --add-architecture amd64 \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates curl git python3 python3-venv python3-dev pkg-config xz-utils unzip zip bzip2 patch file \
    build-essential gperf bison ninja-build openjdk-17-jdk-headless libatomic1 libglib2.0-0 libnss3 \
    libx11-6 libdbus-1-3 libdrm2 libgbm1 libatk1.0-0 libatk-bridge2.0-0 libatspi2.0-0 \
    libxcomposite1 libxdamage1 libxfixes3 libxrandr2 libasound2 libfontconfig1 zlib1g-dev \
    libc6:amd64 libstdc++6:amd64 zlib1g:amd64 libtinfo6:amd64 \
    && test -x /lib64/ld-linux-x86-64.so.2 \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir /tmp/node-install && cd /tmp/node-install \
    && curl --fail --location --retry 2 --connect-timeout 30 https://nodejs.org/dist/v22.23.1/node-v22.23.1-linux-arm64.tar.xz -o node.tar.xz \
    && curl --fail --location --retry 2 --connect-timeout 30 https://nodejs.org/dist/v22.23.1/SHASUMS256.txt -o SHASUMS256.txt \
    && awk '$2 == "node-v22.23.1-linux-arm64.tar.xz" {print $1 "  node.tar.xz"}' SHASUMS256.txt > node.sha256 \
    && test -s node.sha256 && sha256sum -c node.sha256 \
    && tar -xJf node.tar.xz -C /usr/local --strip-components=1 \
    && cd / && rm -rf /tmp/node-install \
    && corepack enable && gem install bundler -v 2.5.22 --no-document

RUN useradd --uid 501 --gid 20 --create-home --shell /bin/bash aegisbuilder
USER 501:20
ENV USER=aegisbuilder LOGNAME=aegisbuilder
WORKDIR /home/aegisbuilder
