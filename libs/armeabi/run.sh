setenforce 0
echo "starting sleep" > ./test.log
sleep 10

### echo memtester_with_idle_pc > /sys/power/wake_unlock
echo $( cat /proc/driver/rtc | grep rtc_time ) >> ./test.log  
./set_alarm_timer 40 >> ./test.log  2>&1  & 

sync
### echo memtester_with_idle_pc > /sys/power/wake_lock

COUNT=0
PREV_TIME=$(date +%s)
while [ ${COUNT} -le 25 ]
do 
    sleep 2
    NEXT_TIME=$(date +%s)
    DIFF_TIME=$(( NEXT_TIME - PREV_TIME ))
    RTC_TIME=$( cat /proc/driver/rtc | grep rtc_time )
    echo $COUNT $RTC_TIME $DIFF_TIME 2>&1  >> ./test.log  
    sync
    PREV_TIME=$NEXT_TIME
    let COUNT=$COUNT+1 
done

