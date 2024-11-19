#!/bin/bash
# /etc/init.d/vcool

### BEGIN INIT INFO
# Provides:          vcool
# Required-Start:    $all
# Required-Stop:     $all
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Vivid Unit cooler panel daemon
# Description:       This survice runs vcool process in the background and control fan speed according to CPU/GPU temperature.
### END INIT INFO

case "$1" in
    start)
        echo "Running vcool..."
        /usr/bin/vcool >>/var/log/vcool.log 2>&1 &
        ;;
    stop)
        echo "Stopping vcool..."
		vcool_pid=$(cat /var/run/vcool.pid)
		kill -9 $vcool_pid
        ;;
	status)
		if pidof myservice > /dev/null; then
			echo "vcool is running"
        else
			echo "vcool is not running"
        fi
		;;
    *)
        echo "Usage: /etc/init.d/vcool start|stop|status"
        exit 1
        ;;
esac

exit 0