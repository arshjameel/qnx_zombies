#!/bin/bash
# start game server on qnx

set -e

ssh qnxpi "slay server_qnx" 2>/dev/null || true

source ~/qnx800/qnxsdp-env.sh
make deploy-server BUILD_PROFILE=release
make run-server BUILD_PROFILE=release