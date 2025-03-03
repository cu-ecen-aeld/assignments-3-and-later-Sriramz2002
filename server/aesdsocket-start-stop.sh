#!/bin/sh

### BEGIN INIT INFO
# Provides:          aesdsocket
# Required-Start:    $remote_fs $syslog
# Required-Stop:     $remote_fs $syslog
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Start/stop aesdsocket daemon
### END INIT INFO

# ------------------- Configuration Variables --------------------
DAEMON_EXEC="aesdsocket"                    # Name of the executable
DAEMON_PATH="/usr/bin/$DAEMON_EXEC"         # Full path to the executable
DAEMON_ARGS="-d"                            # Arguments passed to the daemon
DAEMON_NAME="aesdsocket"                    # Name used by start-stop-daemon

# ---------------------- Helper Functions ------------------------

# Function to print error messages
error_exit() {
    echo "[ERROR] $1" >&2
    exit 1
}

# Function to check if the process is running
is_running() {
    pgrep -f "$DAEMON_PATH" > /dev/null 2>&1
}

# -------------------------- Main Script -------------------------

case "$1" in
  start)
    echo "Starting $DAEMON_NAME..."

    # Check if the daemon executable exists and is executable
    if [ ! -x "$DAEMON_PATH" ]; then
      error_exit "Daemon binary not found or is not executable at $DAEMON_PATH"
    fi

    # Check if the daemon is already running
    if is_running; then
      echo "$DAEMON_NAME is already running."
      exit 0
    fi

    # Start the daemon
    start-stop-daemon --start --background --exec "$DAEMON_PATH" -- $DAEMON_ARGS
    if [ $? -eq 0 ]; then
      echo "$DAEMON_NAME started successfully."
    else
      error_exit "Failed to start $DAEMON_NAME"
    fi
    ;;

  stop)
    echo "Stopping $DAEMON_NAME..."

    # Stop the daemon using its name
    if is_running; then
      pkill -f "$DAEMON_PATH"
      if [ $? -eq 0 ]; then
        echo "$DAEMON_NAME stopped successfully."
      else
        error_exit "Failed to stop $DAEMON_NAME"
      fi
    else
      echo "$DAEMON_NAME is not running."
    fi
    ;;

  status)
    if is_running; then
      echo "$DAEMON_NAME is running."
    else
      echo "$DAEMON_NAME is not running."
    fi
    ;;

  restart)
    $0 stop
    $0 start
    ;;

  *)
    echo "Usage: $0 {start|stop|restart|status}"
    exit 1
    ;;
esac

exit 0
