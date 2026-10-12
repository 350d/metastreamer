# Build environment for the metastreamer SD card image (Buildroot). Used by tools/build-image.sh.
FROM debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential git wget ca-certificates cpio unzip rsync bc python3 file perl patch \
        xz-utils bzip2 gzip libncurses-dev which locales openssh-client \
    && rm -rf /var/lib/apt/lists/* \
    && sed -i 's/# en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen && locale-gen \
    && useradd -m -u 1000 b && mkdir -p /work && chown b /work
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
USER b
WORKDIR /work
