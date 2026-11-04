#!/bin/sh
CONSOLE_USER=`/usr/bin/stat -f%Su /dev/console`
/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall MoGRUConverter > /dev/null 2>&1 || true
/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall MoGRURenderer > /dev/null 2>&1 || true
/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall MoGRU > /dev/null 2>&1 || true

if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ] && [ "$COMMAND_LINE_INSTALL" != "1" ]; then
  /usr/bin/sudo -u "$CONSOLE_USER" "/Library/Input Methods/MoGRU.app/Contents/MacOS/MoGRU" --register_input_source > /dev/null 2>&1 || true
fi

/usr/bin/true
