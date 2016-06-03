adb push set_alarm_timer /data/local/tmp/
adb shell chmod 0777 /data/local/tmp/set_alarm_timer
adb push run.sh /data/local/tmp/ 
adb shell chmod 0777 /data/local/tmp/run.sh
adb shell rm /data/local/tmp/test.log
