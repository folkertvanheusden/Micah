#! /bin/sh

mpirun --npersocket 1 --hostfile /home/pi/mpi.hosts build/Micah -c 4 -T /home/pi/cluster/Micah/tune.dat
