#!/bin/sh
DIR=/tmp/fio
if [ ! $# -eq 1 ]; then
	echo "Usage: $0 <benchmark>.fio"
	exit 1
fi
mkdir -p $DIR
fio $1
rm -rf $DIR
