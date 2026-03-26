#!/bin/bash

TARGET=$1

while :
do
    cat /sys/fs/cgroup/htmm/memory.stat | grep -e anon_thp -e anon >> ${TARGET}/memory_stat.txt
    cat /sys/fs/cgroup/htmm/memory.hotness_stat >> ${TARGET}/hotness_stat.txt
    cat /proc/vmstat | grep pgmigrate_su >> ${TARGET}/pgmig.txt
    numastat -m >> ${TARGET}/numastat_periodic.txt 2>/dev/null
    cat /sys/fs/cgroup/htmm/memory.numa_stat >> ${TARGET}/cgroup_numa_stat.txt 2>/dev/null
    sleep 1
done
