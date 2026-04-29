# Fabify Link

Fabify Link is a plug-and-play device that connects the CNC machines with the internet. This version uses ESP32 S3 for its USB Host cabailities that connects with 3d printers. Later version would have RS232 for communicating with general CNC machines, along with the USB Hubs support.

## Core Components

1. Message Router - routes queued messages to the subscribers/listeners of path based channels
2. Command Processor - processes cli-like commands based on the registered commands
3. HTTP Server - provides a web interface to the user to interact with the device
4. USB VC - provides a virtual serial host port to the user to interact with the device



## IDF initialization

1. Try export to use the idf
```bash
. "$HOME/esp/esp-idf/export.sh" > /dev/null
```

2. If thrown error of idf not installed then
```bash
# install idf
$HOME/esp/esp-idf/install.sh
# export idf
. $HOME/esp/esp-idf/export.sh
```

3. otherwise just find the idf path and restart from there.
4. if path not found then you may need to download idf from https://github.com/espressif/esp-idf, install, and export it.
