#!/bin/bash
GATEWAY="192.168.4.1"

# Check if we can ping the gateway
ping -c 2 -W 3 "$GATEWAY" > /dev/null 2>&1

if [ $? -ne 0 ]; then
    echo "$(date): Gateway $GATEWAY is unreachable. Restarting wlan0..." >> /home/pi/piTrove/logs/wifi_keepalive.log
    nmcli device disconnect wlan0
    sleep 5
    nmcli device connect wlan0
else
    # Connection is healthy
    exit 0
fi
