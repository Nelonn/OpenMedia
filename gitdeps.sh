#!/usr/bin/env bash

#
# This file is a part of gitdeps utility
# licensed under MIT license
# https://github.com/Nelonn/gitdeps/blob/main/LICENSE
#

set -e

cd "`dirname "$0"`"

VERSION="0.2.2"
RELEASES_URL="https://github.com/Nelonn/gitdeps/releases/download"

OS_ARCH=$(echo `uname -m | sed s/aarch64/arm64/ | sed s/x86_64/amd64/`)

if [ "$(uname)" = "Darwin" ]; then
  BASE_NAME=gitdeps_${VERSION}_darwin_${OS_ARCH}
  FILE_NAME=${BASE_NAME}.zip
  echo Downloading ${RELEASES_URL}/v${VERSION}/${FILE_NAME}
  curl --create-dirs -LO --output-dir .gitdeps ${RELEASES_URL}/v${VERSION}/${FILE_NAME}
  unzip .gitdeps/${FILE_NAME} -d .gitdeps
else
  BASE_NAME=gitdeps_${VERSION}_linux_${OS_ARCH}
  FILE_NAME=${BASE_NAME}.tar.gz
  echo Downloading ${RELEASES_URL}/v${VERSION}/${FILE_NAME}
  curl --create-dirs -LO --output-dir .gitdeps ${RELEASES_URL}/v${VERSION}/${FILE_NAME}
  tar -xf .gitdeps/${FILE_NAME} -C .gitdeps
fi

.gitdeps/${BASE_NAME}/gitdeps "$@"
