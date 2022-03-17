FROM magisk-builder-base:latest

RUN cd /app && \
    export NDK_CCACHE=/app/ccache && \
    export CCACHE_DIR=/app/.ccache && \
    set -a && \
    . /etc/environment && \
    set +a && \
    python3 build.py -v ndk
