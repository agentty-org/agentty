#!/bin/sh
# Does the default cpu.weight slow a build when nothing else wants the CPU?
#
# It must not: a weight only decides who loses under CONTENTION. 12 jobs on
# 12 cores, timed. Compare against the same script run outside a box.
start=$(date +%s%N)
i=0
while [ $i -lt 12 ]; do
    python3 -c 'sum(range(12000000))' &
    i=$((i + 1))
done
wait
end=$(date +%s%N)
echo "elapsed_ms=$(( (end - start) / 1000000 ))"
