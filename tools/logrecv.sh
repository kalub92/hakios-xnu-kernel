#!/bin/sh
# Live log of a stay image: boot it with logto=<this Mac's IP>:PORT; usage: tools/logrecv.sh [PORT] (default 9999)
cd "$(dirname "$0")/.." && mkdir -p logs && exec nc -ul "${1:-9999}" | tee logs/live.log
