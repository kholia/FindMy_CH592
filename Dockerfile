FROM ubuntu:noble

ARG TARGETARCH
ARG XPACK_VERSION=15.2.0-1

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --yes --no-install-recommends \
        ca-certificates \
        curl \
        make \
    && case "${TARGETARCH}" in \
        amd64) XPACK_ARCH=x64; \
               XPACK_SHA256=aaaa8060c914851a3e5ee1ba82cc3d6f80972f90638a05c6e823a37557a33758 ;; \
        arm64) XPACK_ARCH=arm64; \
               XPACK_SHA256=4e60e2a54c16385e4e2476d08240f857495d5a61609d97e1ee49f72875a6ec1e ;; \
        *) echo "Unsupported architecture: ${TARGETARCH}" >&2; exit 1 ;; \
       esac \
    && XPACK_ARCHIVE="xpack-riscv-none-elf-gcc-${XPACK_VERSION}-linux-${XPACK_ARCH}.tar.gz" \
    && XPACK_URL="https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v${XPACK_VERSION}/${XPACK_ARCHIVE}" \
    && curl --fail --location --retry 3 \
        "${XPACK_URL}" --output /tmp/xpack-riscv-none-elf-gcc.tar.gz \
    && echo "${XPACK_SHA256}  /tmp/xpack-riscv-none-elf-gcc.tar.gz" | sha256sum --check --strict \
    && mkdir --parents /opt/xpack-riscv-none-elf-gcc \
    && tar --extract --gzip --file /tmp/xpack-riscv-none-elf-gcc.tar.gz \
        --directory /opt/xpack-riscv-none-elf-gcc --strip-components=1 \
    && rm --force /tmp/xpack-riscv-none-elf-gcc.tar.gz \
    && rm --recursive --force /var/lib/apt/lists/*

ENV PATH=/opt/xpack-riscv-none-elf-gcc/bin:${PATH}
WORKDIR /work

ENTRYPOINT ["make"]
