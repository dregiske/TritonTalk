#!/bin/bash
make clean
make 

# This function runs the executable with the following args:
# $1 -> test_case_id
# $2 -> path to the config file
# $3+ -> extra flags forwarded to ./tritontalk (e.g. -s 0 -r 1)
# It checks the exact order of stdout and exp_out text files
function run_ordered_test() {
    test_id=$1
    config=$2
    shift 2
    test_input=./test_suite/test$test_id/test_input.txt
    stdout=./test_suite/test$test_id/stdout.txt
    stderr=./test_suite/test$test_id/stderr.txt
    exp_out=./test_suite/test$test_id/exp_out.txt
    config_path=./test_suite/$config

	./tritontalk -t $test_id -p $config_path "$@" < $test_input 2>$stderr  1>$stdout

    out_cmp=$(<$stdout)
    exp_out_cmp=$(<$exp_out)

    if [ "$out_cmp" == "$exp_out_cmp" ]; then
        echo "Test $test_id OK!"
    else
        echo "FAILED TEST $test_id"
        cat $stderr
    fi
}

# This function runs the executable with the following args:
# $1 -> test_case_id
# $2 -> path to the config file
# $3+ -> extra flags forwarded to ./tritontalk (e.g. -s 0 -r 1)
# It does not check the exact order of stdout and exp_out text files
function run_unordered_test() {
    test_id=$1
    config=$2
    shift 2
    test_input=./test_suite/test$test_id/test_input.txt
    stdout=./test_suite/test$test_id/stdout.txt
    stderr=./test_suite/test$test_id/stderr.txt
    exp_out=./test_suite/test$test_id/exp_out.txt
    config_path=./test_suite/$config

	./tritontalk -t $test_id -p $config_path "$@" < $test_input 2>$stderr  1>$stdout

    sorted_stdout=$(sort $stdout)
    sorted_exp_out=$(sort $exp_out)

    if [ "$sorted_stdout" == "$sorted_exp_out" ]; then
        echo "Test $test_id OK!"
    else
        echo "FAILED TEST $test_id"
        cat $stderr
    fi
}

#Test 1
run_ordered_test 1 basic.cfg

#Test 2
#Advanced Testing: Checking corruption
run_ordered_test 2 basic.cfg

#Test 3
run_unordered_test 3 sw4.cfg

#Test 4: Multi-frame message (fragmentation + reassembly)
run_ordered_test 4 basic.cfg

#Test 5: Bidirectional simultaneous messaging
run_unordered_test 5 basic.cfg

#Test 6: Corruption + retransmission (30% corrupt)
run_ordered_test 6 corrupt.cfg

#Test 7: Multi-frame message + corruption (fragmentation + retransmission combined)
run_ordered_test 7 corrupt.cfg

#Test 8: Wraparound — 20K message, window_size=8 (mirrors autograder test 13)
run_ordered_test 8 wrap8.cfg

#Test 9: Wraparound — 20K message, window_size=5 (mirrors autograder test 26)
run_ordered_test 9 wrap5.cfg

#Test 10: 1K message, window=8, 30% corruption (mirrors autograder test 19)
run_ordered_test 10 longcorrupt.cfg

#Test 11: 128-char bidirectional + 30% corruption, 3 hosts (mirrors autograder test 18)
run_unordered_test 11 corrupt.cfg

#Test 12: recv_accept_rate=8, window=8, 3 senders → host 0, no corruption (mirrors autograder test 22)
run_unordered_test 12 sw8.cfg

# PA1b CC Tests
#Test 13: window=16 (>8), 18 frames — SS grows cwnd past 8, exercises large window
run_ordered_test 13 cc_window16.cfg -s 0 -r 1

#Test 14: window=20, 27 frames — SS grows cwnd past ssthresh=20, transitions to AIMD
run_ordered_test 14 cc_window20.cfg -s 0 -r 1

#Test 15: window=8, recv_accept_rate=4 — CC with rate-limited drops (FRFT/timeout recovery)
run_ordered_test 15 cc_basic.cfg -s 0 -r 2

#Test 16: window=16, simultaneous bidirectional — independent CC state per sender/receiver pair
run_unordered_test 16 cc_window16.cfg -s 0 -r 1

#Test 17: window=16, long bidirectional messages — CC in both directions concurrently
run_unordered_test 17 cc_window16.cfg -s 0 -r 1

#Test 18: window=16, two senders → same receiver — each sender tracks separate cwnd
run_unordered_test 18 cc_window16.cfg -s 0 -r 2

#Test 19: sequential messages same pair — CC state (cwnd/ssthresh) persists between messages
run_ordered_test 19 cc_window16.cfg -s 0 -r 1

#Test 20: one sender → two different receivers — independent cwnd per destination
run_unordered_test 20 cc_window16.cfg -s 0 -r 1

#Test 21: seq num wraparound + CC — 264 frames with window=16, seq wraps at 256
run_ordered_test 21 cc_window16.cfg -s 0 -r 1


# Just in case you want to get even more fancy with your testing :)
# str="a"
# freq=10
# printf "%0.s${str}" $(seq 1 $freq) >> ./test_suite/test_input.txt
# printf "%s\n" >> ./test_suite/test_input.txt
# printf "%s" "exit" >> ./test_suite/test_input.txt
