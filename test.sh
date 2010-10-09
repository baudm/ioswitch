#!/bin/bash
#
#benchmark schedulers
#

if [ $EUID -ne 0 ]; then
	echo "run as sudo"
	exit 1
fi

if [ $# -ne 2 ]; then
	echo "Usage: $0 -ioswitch=<on/off> <workload>"
	exit 1
fi

dir="$(cd "$(dirname "$0")" && pwd)"
fio_file=($dir/benchmarks/backup/"$2".fio)

check=$(lsmod | grep throughput)
if [ ${#check} -ne 0 ]; then
	rmmod throughput
fi

check=$(lsmod | grep ioswitch)
if [ ${#check} -ne 0 ]; then
	rmmod ioswitch
fi

if [ ${1:10} == "on" ]; then
	insmod $dir/ioswitch/ioswitch.ko
	sleep 20
fi

line=$(wc /var/log/messages | awk -F' ' '{print $1}')
insmod $dir/throughput/throughput.ko
$dir/benchmarks/run.sh $fio_file
sleep 30
rmmod throughput

if [ ${1:10} == "on" ]; then
	rmmod ioswitch
fi

for n in {1..5} do
	check=$(ls | grep BW"$n")
	if [ ${#check} -eq 0 ]; then
		tail -n +$(($line+1)) /var/log/messages | grep TMM > $2_BW$n
		grep BW $2_BW$n | awk -F' ' '{print $8}' | cut -c 5- > $2_read$n
		grep BW $2_BW$n | awk -F' ' '{print $9}' | cut -c 5- > $2_write$n

		reboot
	fi
done

exit 0
