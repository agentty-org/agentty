#!/bin/sh
# Does the sandbox proxy serve fetches CONCURRENTLY?
#
# The first version handled one connection at a time, so N fetches took N
# times as long -- which is what `npm install` does, a dozen sockets at once.
# Eight parallel requests against a serial proxy take 8x one request; against
# a concurrent one they take about 1x.
N=8
start=$(date +%s%N)
i=0
while [ $i -lt $N ]; do
    curl -sS --max-time 25 -o /dev/null -w "%{http_code} " https://example.com/ &
    i=$((i + 1))
done
wait
end=$(date +%s%N)
echo
echo "parallel_ms=$(( (end - start) / 1000000 ))"
start=$(date +%s%N)
curl -sS --max-time 25 -o /dev/null -w "one=%{http_code}\n" https://example.com/
end=$(date +%s%N)
echo "single_ms=$(( (end - start) / 1000000 ))"
