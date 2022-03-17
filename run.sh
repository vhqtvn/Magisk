#!/bin/bash

if [[ "$1" == "entry" ]]; then
    sudo() {
        "$@"
    }
    export -f sudo
    shift
    export NDK_CCACHE=/app/ccache
    export CCACHE_DIR=/app/.ccache
    set -a
    . /etc/environment
    set +a
    /bin/bash "$@"
    exit $?
fi


DOCKER_IMAGE="$1"

DOCKER_FILE_EXT=
case "$DOCKER_IMAGE" in
    builder-base)
        DOCKER_FILE_EXT=base
        ;;
    builder)
        DOCKER_FILE_EXT=builder
        ;;
    *)
        echo "Invalid image"
        exit 1
        ;;
esac
DOCKER_IMAGE="magisk-$DOCKER_IMAGE"
CONTAINER_NAME="$DOCKER_IMAGE--container"

case "$2" in
    build)
        docker build -f "Dockerfile.$DOCKER_FILE_EXT" . -t "$DOCKER_IMAGE"
        ;;
    kill)
        docker kill "$CONTAINER_NAME"
        ;;
    rm)
        docker rm "$CONTAINER_NAME"
        ;;
    run)
        docker start "$CONTAINER_NAME" || docker run -d --name "$CONTAINER_NAME" -v "$(pwd):/app" --workdir /app "$DOCKER_IMAGE"
        docker exec -it "$CONTAINER_NAME" /app/run.sh entry
        ;;
    *)
        echo "????"
        ;;
esac