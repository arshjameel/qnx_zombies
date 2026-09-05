#!/bin/bash
# start qnx client

set -e

cd "$(dirname "${BASH_SOURCE[0]}")"

ssh qnxpi "rm -rf /data/home/qnxuser/game/qnx_client"
source ~/qnx800/qnxsdp-env.sh
cd qnx_client
make run