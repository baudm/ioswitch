#!/bin/bash
#
#benchmark schedulers
#
if [ $EUID -ne 0 ]; then
	echo "run as sudo"
	exit 1
fi

dir="$(cd "$(dirname "$0")" && pwd)"
temp=($dir/benchmarks/backup/*.fio)

check=$(lsmod | grep throughput)
if [ ${#check} -ne 0 ]; then
	rmmod throughput
fi

check=$(lsmod | grep ioswitch)
if [ ${#check} -ne 0 ]; then
	rmmod ioswitch
fi

insmod $dir/ioswitch/ioswitch.ko
sleep 20

line=$(wc /var/log/messages | awk -F' ' '{print $1}')
insmod $dir/throughput/throughput.ko
t0=$(date +%s)
for n in {1..5}; do
	for fio_file in ${temp[*]}; do
		$dir/benchmarks/run.sh $fio_file
		t1=$(date +%s)
		echo $(($t1-St0)) >> timestamp
	done
done
sleep 30
rmmod throughput
rmmod ioswitch

tail -n +$(($line+1)) /var/log/messages | grep TMM > BW

exit 0
