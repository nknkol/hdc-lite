FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Shanghai

WORKDIR /workspace

# 安装基础编译工具
RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    g++ \
    make \
    cmake \
    git \
    wget \
    pkg-config \
    vim \
    && rm -rf /var/lib/apt/lists/*

# 安装HDC需要的依赖
RUN apt-get update && apt-get install -y \
    libuv1-dev \
    libssl-dev \
    libusb-1.0-0-dev \
    liblz4-dev \
    && rm -rf /var/lib/apt/lists/*

# 编译安装 bounds_checking_function
RUN git clone --depth=1 https://gitee.com/openharmony/third_party_bounds_checking_function.git /tmp/bounds_checking && \
    cd /tmp/bounds_checking && \
    make CC=gcc && \
    mkdir -p /usr/local/include/securec && \
    cp include/*.h /usr/local/include/securec/ && \
    cp lib/libboundscheck.so /usr/local/lib/ && \
    chmod 755 /usr/local/lib/libboundscheck.so && \
    ldconfig && \
    rm -rf /tmp/bounds_checking

# 验证安装
RUN echo "=== Environment Check ===" && \
    gcc --version && \
    cmake --version && \
    pkg-config --modversion libuv || echo "libuv installed" && \
    ls -la /usr/local/lib/libboundscheck.so && \
    echo "=== Ready ==="

ENV LD_LIBRARY_PATH=/usr/local/lib

CMD ["/bin/bash"]