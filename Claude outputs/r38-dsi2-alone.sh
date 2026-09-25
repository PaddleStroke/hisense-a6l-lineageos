cd /mnt/c/Users/Pierre/Desktop/A6L/.relay
./lap.sh 20 'adb -s 1e529013 reboot bootloader'
ok=0; for i in $(seq 1 30); do sleep 5; ./lap.sh 10 'adb devices' | grep -q HLTE730T-PROBE && { echo "RECOVERY_UP after $((i*5))s"; ok=1; break; }; done
[ $ok = 1 ] || { echo RECOVERY_NOT_SEEN; exit 1; }
sleep 5
echo "=== prep"; bash ./prep.sh 2>&1 | tail -n 2
P='export PATH=/tmp/bin:$PATH;'
pr='for i in 1 2 3 4 5 6 7 8; do /tmp/eink-draw/bin/a6l_dsi2dpi_init /dev/i2c-0 id | grep 0f; usleep 100000; done | sort | uniq -c'
./ph.sh 40 "$P echo A6L_T38 > /dev/kmsg; echo 1 > /sys/class/graphics/fb0/blank; sleep 2; echo blanked: vdcc=\$(cat /sys/class/regulator/regulator.1/state) active=\$(grep -c active=1 /sys/kernel/debug/dri/1/state)"
./ph.sh 60 "$P nohup sh -c \"sleep 120 | /tmp/modetest -M msm -D /dev/dri/card1 -s 38@70\" > /tmp/mt.log 2>&1 & sleep 8; echo mt.log:; cat /tmp/mt.log | head -n 8; echo; grep -E \"^crtc|active=|^connector|crtc=crtc\" /sys/kernel/debug/dri/1/state | tr \"\\n\" \" \"; echo; echo vdcc=\$(cat /sys/class/regulator/regulator.1/state); grep -E \"byte0_clk_src|byte1_clk_src\" /sys/kernel/debug/clk/clk_summary | cut -c1-60; dmesg | sed -n \"/A6L_T38/,\\\$p\" | grep -E \"bridge \(|rails\"; echo probe:; $pr"
./ph.sh 40 "$P sleep 5; echo probe2:; $pr; ps | grep -c modetest"
