# JoyTrace replay

Runs one port of a recorded `JoyTrace.txt` (from a local session) through `JoyBusLagModel` and
counts what a second machine in a room would have to wait for.

    clang++ -std=c++20 -O1 -I<dolphin>/Source/Core replay.cpp -o replay
    ./replay JoyTrace.txt 3 100        # port 3, 100 ms lag

Prints waited / lagged / predicted totals, those that differ from what the GBA really said, a
10-second breakdown, a histogram of the late waits, and a sample of the longest wait run.
