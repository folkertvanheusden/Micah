#! /bin/sh

cd /home/pi/cluster/Micah

mpirun --hostfile mpi.hosts build/Micah -c 4 -T tune.dat
