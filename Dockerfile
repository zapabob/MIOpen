FROM ubuntu:22.04 as miopen
ARG DEBIAN_FRONTEND=noninteractive

# Support multiarch
RUN dpkg --add-architecture i386

# Install preliminary dependencies
RUN apt-get update && \
DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-unauthenticated \
    apt-utils \
    ca-certificates \
    curl \
    libnuma-dev \
    gnupg2 \
    wget

#Add gpg keys
ENV APT_KEY_DONT_WARN_ON_DANGEROUS_USAGE=DontWarn
RUN curl -fsSL https://repo.radeon.com/rocm/rocm.gpg.key | gpg --dearmor -o /etc/apt/trusted.gpg.d/rocm-keyring.gpg

RUN wget https://repo.radeon.com/amdgpu-install/6.0/ubuntu/jammy/amdgpu-install_6.0.60000-1_all.deb --no-check-certificate
RUN apt-get update && \
DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-unauthenticated \
    ./amdgpu-install_6.0.60000-1_all.deb

# Add rocm repository
RUN export ROCM_APT_VER=6.0;\
echo $ROCM_APT_VER &&\
sh -c 'echo deb [arch=amd64 signed-by=/etc/apt/trusted.gpg.d/rocm-keyring.gpg] https://repo.radeon.com/amdgpu/$ROCM_APT_VER/ubuntu jammy main > /etc/apt/sources.list.d/amdgpu.list' &&\
sh -c 'echo deb [arch=amd64 signed-by=/etc/apt/trusted.gpg.d/rocm-keyring.gpg] https://repo.radeon.com/rocm/apt/$ROCM_APT_VER jammy main > /etc/apt/sources.list.d/rocm.list'
RUN sh -c "echo deb http://mirrors.kernel.org/ubuntu jammy main universe | tee -a /etc/apt/sources.list"

RUN amdgpu-install -y --usecase=rocm --no-dkms

# Install dependencies
RUN apt-get update && \
DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-unauthenticated \
    build-essential \
    cmake \
    clang-format-12 \
    doxygen \
    gdb \
    git \
    git-lfs \
    lbzip2 \
    lcov \
    libncurses5-dev \
    pkg-config \
    python3-dev \
    python3-pip \
    python3-venv \
    rocblas \
    rocm-developer-tools \
    rpm \
    software-properties-common && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Setup ubsan environment to printstacktrace
ENV UBSAN_OPTIONS=print_stacktrace=1

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

# Install an init system
RUN wget https://github.com/Yelp/dumb-init/releases/download/v1.2.0/dumb-init_1.2.0_amd64.deb
RUN dpkg -i dumb-init_*.deb && rm dumb-init_*.deb
# Install cget
RUN pip3 install https://github.com/pfultz2/cget/archive/a426e4e5147d87ea421a3101e6a3beca541c8df8.tar.gz

# Install rbuild
RUN pip3 install https://github.com/RadeonOpenCompute/rbuild/archive/6d78a0553babdaea8d2da5de15cbda7e869594b8.tar.gz

# Add symlink to /opt/rocm
RUN [ -d /opt/rocm ] || ln -sd $(realpath /opt/rocm-*) /opt/rocm

# Make sure /opt/rcom is in the paths
ENV PATH="/opt/rocm:${PATH}"

# Add requirements files
ADD rbuild.ini /rbuild.ini
ADD requirements.txt /requirements.txt
ADD dev-requirements.txt /dev-requirements.txt
# Install dependencies
# TODO: Add --std=c++14
ARG GPU_ARCH=";"
ARG PREFIX=/usr/local
ARG USE_FIN="OFF"
ARG CCACHE_SECONDARY_STORAGE=""
ARG CCACHE_DIR="/tmp"
RUN env
# RUN cget -p $PREFIX install https://github.com/ccache/ccache/archive/7f1572ae9ca958fa923a66235f6a64a360b03523.tar.gz -DZSTD_FROM_INTERNET=ON -DHIREDIS_FROM_INTERNET=ON
ARG CCACHE_COMMIT=7f1572ae9ca958fa923a66235f6a64a360b03523
RUN rm -rf /tmp/ccache* && mkdir /tmp/ccache
ADD https://github.com/ccache/ccache/archive/${CCACHE_COMMIT}.tar.gz /tmp/ccache.tar.gz
RUN tar zxvf /tmp/ccache.tar.gz -C /tmp/ && mkdir /tmp/ccache-${CCACHE_COMMIT}/build && \
    cd /tmp/ccache-${CCACHE_COMMIT}/build && \
    cmake -DZSTD_FROM_INTERNET=ON -DHIREDIS_FROM_INTERNET=ON .. && make -j install && rm -rf /tmp/*
RUN ccache -s 

# purge existing composable kernel installed with ROCm
# hence cannot use autoremove since it will remove more components
RUN apt-get update && \
DEBIAN_FRONTEND=noninteractive apt-get purge -y --allow-unauthenticated \
    composablekernel-dev
ARG COMPILER_LAUNCHER=""
RUN if [ "$USE_FIN" = "ON" ]; then \
        rbuild prepare -s fin -d $PREFIX -DGPU_TARGETS=${GPU_ARCH} -DCMAKE_CXX_COMPILER_LAUNCHER="${COMPILER_LAUNCHER}"; \
    else \
        rbuild prepare -s develop -d $PREFIX -DGPU_TARGETS=${GPU_ARCH} -DCMAKE_CXX_COMPILER_LAUNCHER="${COMPILER_LAUNCHER}"; \
    fi

RUN ccache -s 
# Install doc requirements
ADD docs/sphinx/requirements.txt /doc-requirements.txt
RUN pip3 install -r /doc-requirements.txt

# Composable Kernel requires this version cmake
RUN pip3 install --upgrade cmake==3.27.5

RUN groupadd -f render
