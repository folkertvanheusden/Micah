#! /bin/sh

cd /home/pi/cluster/Micah

/usr/games/polyglot -noini -ec "build/Micah -n p1:8001,p2:8002,p3:8003,p4:8004,p5:8005,p6:8006 -l ~/micahgb-micah.log -x `hostname` -T /home/pi/cluster/Micah/tune.dat"
