#!/bin/sh
# Sandbox network probe, run INSIDE the box by sandbox_live_check / by hand.
# Every line is a claim the audit made about the network boundary.
echo "== proxy env"
env | grep -i '^\(http\|https\|all\)_proxy=' | sort
echo "== own netns? (a fresh one has only lo)"
cat /proc/net/dev 2>/dev/null | awk 'NR>2 {print $1}' | tr -d ' :' | sort | tr '\n' ' '
echo
echo "== direct to a host loopback service (must fail: not in this namespace)"
curl -sS --max-time 5 --noproxy '*' http://127.0.0.1:48231/ 2>&1 | head -2
echo "== through the proxy to that same host service (must be refused)"
curl -sS --max-time 5 -o /dev/null -w 'host-loopback=%{http_code}\n' -x http://127.0.0.1:3128 http://127.0.0.1:48231/ 2>&1 | head -2
echo "== through the proxy to a LAN address (must be refused)"
curl -sS --max-time 5 -o /dev/null -w 'lan=%{http_code}\n' -x http://127.0.0.1:3128 http://192.168.1.1/ 2>&1 | head -2
echo "== through the proxy to cloud metadata (must be refused)"
curl -sS --max-time 5 -o /dev/null -w 'metadata=%{http_code}\n' -x http://127.0.0.1:3128 http://169.254.169.254/latest/meta-data/ 2>&1 | head -2
echo "== through the proxy with SOCKS5 to the host service (must be refused)"
curl -sS --max-time 5 -o /dev/null -w 'socks-loopback=%{http_code}\n' -x socks5h://127.0.0.1:3128 http://127.0.0.1:48231/ 2>&1 | head -2
echo "== SOCKS5 to the real internet (must work)"
curl -sS --max-time 20 -o /dev/null -w 'socks-net=%{http_code}\n' -x socks5h://127.0.0.1:3128 https://example.com/ 2>&1 | head -2
echo "== real internet, https through the proxy (must work)"
curl -sS --max-time 20 https://example.com/ -o /dev/null -w 'status=%{http_code}\n' 2>&1 | head -2
echo "== real internet, plain http through the proxy (must work)"
curl -sS --max-time 20 http://example.com/ -o /dev/null -w 'status=%{http_code}\n' 2>&1 | head -2
