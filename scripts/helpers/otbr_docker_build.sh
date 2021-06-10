#!/usr/bin/env bash

#
# Copyright (c) 2021 Project CHIP Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# otbr_docker_build.sh - utility for building (and optionally) tagging and pushing
#                        the otbr Docker image.
#

set -ex

# Default parameters
ORG=connectedhomeip
IMAGE=otbr
VERSION=latest
PLATFORMS=linux/amd64,linux/arm/v7,linux/arm64
OTBR_REV=main
OTBR_PATH=
ARGS=

die() {
    echo "$0: *** ERROR: $*"
    exit 1
}

usage() {
    echo "Usage: $0 --path <path_to_otbr> [--rev <otbr_revision> --org <organization> --image <image> --version <version> --push]" >&2
    exit 0
}

while (($#)); do
    case "$1" in
        --path)
            OTBR_PATH="$2"
            shift
            ;;
        --rev)
            OTBR_REV="$2"
            shift
            ;;
        --org)
            ORG="$2"
            shift
            ;;
        --image)
            IMAGE="$2"
            shift
            ;;
        --version)
            VERSION="$2"
            shift
            ;;
        --push) ARGS+=("--push") ;;
        --help) usage ;;
    esac
    shift
done

[[ -n "$OTBR_PATH" ]] || usage

# Checkout ot-br-posix to the correct revision
git -C "$OTBR_PATH" fetch || die "Failed to update repository: $OTBR_PATH"
git -C "$OTBR_PATH" checkout "$OTBR_REV" || die "Failed to switch to revision: $OTBR_REV"

# Build docker image
docker run --rm --privileged multiarch/qemu-user-static --reset -p yes
docker buildx create --use --name otbr_builder --node otbr_node --driver docker-container --platform "$PLATFORMS"
docker buildx build --no-cache -t "$ORG/$IMAGE:$VERSION" -f "$OTBR_PATH/etc/docker/Dockerfile" --platform "$PLATFORMS" "${ARGS[*]}" "$OTBR_PATH"

exit 0
