#!/bin/sh
DIR=/tmp/fio
mkdir $DIR
fio $1
rm -rf $DIR
