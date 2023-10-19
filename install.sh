#!/bin/bash
sudo mmcli -G DEBUG
sudo systemctl stop ModemManager || sudo systemctl disable ModemManager || sudo apt remove modemmanager
sudo journalctl -f|grep "ModemManager.*\[forbidden\]"
if [ $? -ne 0 ]; then
fi
sudo mmcli -G ERR 
sudo snap install arduino
sudo snap install arduino-cli
sudo apt-get install ccache