#!/bin/bash
# start godot client

set -e

SCRIPT_DIR="$(dirname "${BASH_SOURCE[0]}")"
godot --path "$SCRIPT_DIR/godot_client"