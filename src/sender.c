#include "host.h"
#include <assert.h>
#include "switch.h"

int count_frames_in_flight(Host* host, uint8_t destination) {
	int count = 0;
	for (int i = 0; i < glb_sysconfig.window_size; ++i){
		if(host->send_window[i].frame != NULL &&
		   host->send_window[i].frame->dst_id == destination){
			count++;
		}
	}
	return count;
}

struct timeval* host_get_next_expiring_timeval(Host* host) {
    // Returns the timeval when next timeout should occur
    // 1) Check your send_window for the timeouts of the frames. 
    // 2) Return the timeout of a single frame. 
	struct timeval* earliest = NULL;
	for(int i = 0; i < glb_sysconfig.window_size; ++i){
		struct timeval* timeout = host->send_window[i].timeout;
		if(host->send_window[i].frame != NULL && timeout != NULL){
			if(earliest == NULL || timeval_usecdiff(timeout, earliest) > 0){
				earliest = timeout;
			}
		}
	}
    return earliest;
}

void build_frame(Host* host, char* message, uint8_t dst_id){
	int msg_len = strlen(message) + 1;
	int offset = 0;

	while(offset < msg_len){
		int bytes_to_copy = min(msg_len - offset, FRAME_PAYLOAD_SIZE);

		Frame* frame = malloc(sizeof(Frame));
		assert(frame);

		frame->remaining_msg_bytes = msg_len - offset - bytes_to_copy;
		frame->src_id = host->id;
		frame->dst_id = dst_id;
		frame->seq_num = host->next_seq_num[dst_id];
		frame->is_ack = 0;

		memset(frame->data, 0, FRAME_PAYLOAD_SIZE);
		memcpy(frame->data, message + offset, bytes_to_copy);

		frame->crc8 = 0;
		char* buffer = convert_frame_to_char(frame);
		frame->crc8 = compute_crc8(buffer);
		free(buffer);

		ll_append_node(&host->buffered_outframes_head, frame);

		offset += bytes_to_copy;
		host->next_seq_num[dst_id]++;
	}
}

void fast_retransmit(Host* host, uint8_t recv_id) {
	int lowest_index = -1;
	uint8_t lowest_seq = 0;
	for (int i = 0; i < glb_sysconfig.window_size; ++i){
		if(host->send_window[i].frame != NULL &&
		   host->send_window[i].frame->dst_id == recv_id){
			if(lowest_index == -1 || seq_num_diff(host->send_window[i].frame->seq_num, lowest_seq) > 0) {
				lowest_seq = host->send_window[i].frame->seq_num;
				lowest_index = i;
			}
		}
	}
	if(lowest_index != -1) {
		if(host->send_window[lowest_index].timeout != NULL){
			free(host->send_window[lowest_index].timeout);
			host->send_window[lowest_index].timeout = NULL;
		}
	}
}

void handle_incoming_acks(Host* host, struct timeval curr_timeval) {
    (void)curr_timeval;

    // Num of acks received from each receiver
    uint8_t num_acks_received[glb_num_hosts]; 
    memset(num_acks_received, 0, glb_num_hosts); 

    // Num of duplicate acks received from each receiver this rtt
    uint8_t num_dup_acks_for_this_rtt[glb_num_hosts];     //PA1b
    memset(num_dup_acks_for_this_rtt, 0, glb_num_hosts); 

	int ack_queue_length = ll_get_length(host->incoming_frames_head);
	while (ack_queue_length > 0) {
		LLnode* ack_node = ll_pop_node(&host->incoming_frames_head);
		ack_queue_length = ll_get_length(host->incoming_frames_head);
		Frame* ack_frame = (Frame*)ack_node->value;
		free(ack_node);

		if (!check_crc8(ack_frame)) { free(ack_frame); continue; }
		if (ack_frame->is_ack == 0) { free(ack_frame); continue; }

		int cleared_any = 0;
		int slots_cleared = 0;

		for (int i = 0; i < glb_sysconfig.window_size; i++) {
			if (host->send_window[i].frame != NULL &&
				host->send_window[i].frame->dst_id == ack_frame->src_id) {
				int diff = seq_num_diff(ack_frame->seq_num, host->send_window[i].frame->seq_num);
				// is_ack=1: cumulative — clear all slots whose seq_num <= ack seq_num
				// is_ack=2: selective — clear only the exact matching slot
				int should_clear = (ack_frame->is_ack == 1) ? (diff <= 0) : (diff == 0);
				if (should_clear) {
					cleared_any = 1;
					slots_cleared++;
					free(host->send_window[i].frame);
					host->send_window[i].frame = NULL;
					if (host->send_window[i].timeout != NULL) {
						free(host->send_window[i].timeout);
						host->send_window[i].timeout = NULL;
					}
				}
			}
		}
		uint8_t recv_id = ack_frame->src_id;
		CongestionControl* cc = &host->cc[recv_id];
		if (cleared_any) {
			num_acks_received[ack_frame->src_id]++;

			switch (cc->state) {
				case cc_SS:
					cc->dup_acks = 0;
					for(int i = 0; i < slots_cleared; ++i){
						cc->cwnd++;
					}
					if(cc->cwnd >= cc->ssthresh){
						cc->state = cc_AIMD;
					}
					break;
				case cc_AIMD:
					cc->dup_acks = 0;
					cc->cwnd += 1.0/cc->cwnd;
					break;
				case cc_FRFT:
					cc->cwnd = cc->ssthresh;
					cc->dup_acks = 0;
					cc->state = cc_AIMD;
					break;
			}
		} else {
			num_dup_acks_for_this_rtt[ack_frame->src_id]++;
			cc->dup_acks++;

			if(cc->dup_acks == 3){
				cc->ssthresh = max_double(cc->cwnd / 2, 2);
				cc->cwnd = cc->ssthresh + 3;
				cc->state = cc_FRFT;
				fast_retransmit(host, recv_id);
			}
			else if (cc->dup_acks > 3){
				cc->cwnd++;
			}
		}
		free(ack_frame);
	}

    if (host->id == glb_sysconfig.host_send_cc_id) {
        int recv_cc = glb_sysconfig.host_recv_cc_id;
        CongestionControl* cc = &host->cc[recv_cc];

        int frames_in_window = count_frames_in_flight(host, recv_cc);

        int timedout = 0;
        for (int i = 0; i < glb_sysconfig.window_size; i++) {
            if (host->send_window[i].frame != NULL &&
                host->send_window[i].frame->dst_id == recv_cc &&
                host->send_window[i].timeout == NULL) {
                timedout++;
            }
        }

        fprintf(cc_diagnostics, "%d,%d,%d,%s,%f,%f,%d,%d,%d,%d\n",
            host->round_trip_num,
            num_acks_received[recv_cc],
            num_dup_acks_for_this_rtt[recv_cc],
            cc_state_to_char(cc->state),
            cc->cwnd,
            cc->ssthresh,
            host->frames_sent_rtt,
            host->frames_dropped_rtt,
            frames_in_window,
            timedout);

        host->frames_sent_rtt = 0;
        host->frames_dropped_rtt = 0;
    }
}

void handle_input_cmds(Host* host, struct timeval curr_timeval) {
    (void)curr_timeval;
    //    1) Dequeue the Cmd from host->input_cmdlist_head
    //    2) Implement fragmentation if the message length is larger than FRAME_PAYLOAD_SIZE
    //    3) Set up the frame according to the protocol
    //    4) Append each frame to host->buffered_outframes_head

    int input_cmd_length = ll_get_length(host->input_cmdlist_head);

    while (input_cmd_length > 0) {
        // Pop a node off and update the input_cmd_length
        LLnode* ll_input_cmd_node = ll_pop_node(&host->input_cmdlist_head);
        input_cmd_length = ll_get_length(host->input_cmdlist_head);

        // Cast to Cmd type and free up the memory for the node
        Cmd* outgoing_cmd = (Cmd*) ll_input_cmd_node->value;
        free(ll_input_cmd_node);
        
		build_frame(host, outgoing_cmd->message, outgoing_cmd->dst_id);
		
		free(outgoing_cmd->message);
		free(outgoing_cmd);
    }
}

void handle_timedout_frames(Host* host, struct timeval curr_timeval) {
    int cc_reset[glb_num_hosts];
    memset(cc_reset, 0, sizeof(int) * glb_num_hosts);

    for (int i = 0; i < glb_sysconfig.window_size; i++) {
        if (host->send_window[i].frame != NULL &&
            host->send_window[i].timeout != NULL) {
            if (timeval_usecdiff(&curr_timeval, host->send_window[i].timeout) <= 0) {
                uint8_t dst_id = host->send_window[i].frame->dst_id;
                host->frames_dropped_rtt++;
                if (!cc_reset[dst_id]) {
                    host->cc[dst_id].ssthresh = max_double(host->cc[dst_id].cwnd / 2.0, 2.0);
                    host->cc[dst_id].cwnd = 1.0;
                    host->cc[dst_id].dup_acks = 0;
                    host->cc[dst_id].state = cc_SS;
                    cc_reset[dst_id] = 1;
                }
            }
        }
    }

    for (int i = 0; i < glb_sysconfig.window_size; i++) {
        if (host->send_window[i].frame != NULL) {
            uint8_t dst_id = host->send_window[i].frame->dst_id;
            if (cc_reset[dst_id] && host->send_window[i].timeout != NULL) {
                free(host->send_window[i].timeout);
                host->send_window[i].timeout = NULL;
            }
        }
    }
}


void handle_outgoing_frames(Host* host, struct timeval curr_timeval) {

    long additional_ts = 0; 

    if (timeval_usecdiff(&curr_timeval, host->latest_timeout) > 0) {
        memcpy(&curr_timeval, host->latest_timeout, sizeof(struct timeval)); 
    }

    for (int i = 0; i < glb_sysconfig.window_size; i++) {
		if (host->send_window[i].frame != NULL &&
			host->send_window[i].timeout == NULL) {
			Frame* retransmit_copy = malloc(sizeof(Frame));
			assert(retransmit_copy);
			memcpy(retransmit_copy, host->send_window[i].frame, sizeof(Frame));
			ll_append_node(&host->outgoing_frames_head, retransmit_copy);

			struct timeval* next_timeout = malloc(sizeof(struct timeval));
			memcpy(next_timeout, &curr_timeval, sizeof(struct timeval));
			timeval_usecplus(next_timeout, TIMEOUT_INTERVAL_USEC + additional_ts);
			additional_ts += 10000;

			host->send_window[i].timeout = next_timeout;
		}
    }

    Frame* peeked = (Frame*)ll_peek_node(host->buffered_outframes_head);
    if (peeked == NULL) {
        memcpy(host->latest_timeout, &curr_timeval, sizeof(struct timeval));
        timeval_usecplus(host->latest_timeout, additional_ts);
        return;
    }
    uint8_t dst_id = peeked->dst_id;

    int count = count_frames_in_flight(host, dst_id);

    for (int i = 0; i < glb_sysconfig.window_size && ll_get_length(host->buffered_outframes_head) > 0; i++) {
        if (host->send_window[i].frame == NULL &&
            count < (int)host->cc[dst_id].cwnd) {
            LLnode* ll_outframe_node = ll_pop_node(&host->buffered_outframes_head);
            Frame* outgoing_frame = ll_outframe_node->value;

            Frame* send_copy = malloc(sizeof(Frame));
            assert(send_copy);
            memcpy(send_copy, outgoing_frame, sizeof(Frame));
            ll_append_node(&host->outgoing_frames_head, send_copy);

            struct timeval* next_timeout = malloc(sizeof(struct timeval));
            memcpy(next_timeout, &curr_timeval, sizeof(struct timeval));
            timeval_usecplus(next_timeout, TIMEOUT_INTERVAL_USEC + additional_ts);
            additional_ts += 10000; //ADD ADDITIONAL 10ms

            host->send_window[i].frame = outgoing_frame;
            host->send_window[i].timeout = next_timeout;

            free(ll_outframe_node);
            host->frames_sent_rtt++;
            count++;
        }
    }

    memcpy(host->latest_timeout, &curr_timeval, sizeof(struct timeval)); 
    timeval_usecplus(host->latest_timeout, additional_ts);
    
    //NOTE:
    // Don't worry about latest_timeout field for PA1a, but you need to understand what it does.
    // You may or may not use it in PA1b when you implement fast recovery & fast retransmit in handle_incoming_acks(). 
    // If you choose to retransmit a frame in handle_incoming_acks() in PA1b, all you need to do is:

    // ****************************************
    // long additional_ts = 0; 
    // if (timeval_usecdiff(&curr_timeval, host->latest_timeout) > 0) {
    //     memcpy(&curr_timeval, host->latest_timeout, sizeof(struct timeval)); 
    // }

    //  YOUR FRFT CODE FOES HERE

    // memcpy(host->latest_timeout, &curr_timeval, sizeof(struct timeval)); 
    // timeval_usecplus(host->latest_timeout, additional_ts);
    // ****************************************


    // It essentially fixes the following problem:
    
    // 1) You send out 8 frames from sender0. 
    // Frame 1: curr_time + 0.1 + additional_ts(0.01) 
    // Frame 2: curr_time + 0.1 + additional_ts(0.02) 
    // …

    // 2) Next time you send frames from sender0
    // Curr_time could be less than previous_curr_time + 0.1 + additional_ts. 
    // which means for example frame 9 will potentially timeout faster than frame 6 which shouldn’t happen. 

    // Latest timeout fixes that. 

}

// WE HIGHLY RECOMMEND TO NOT MODIFY THIS FUNCTION
void run_senders() {
    int sender_order[glb_num_hosts]; 
    get_rand_seq(glb_num_hosts, sender_order); 

    for (int i = 0; i < glb_num_hosts; i++) {
        int sender_id = sender_order[i]; 
        struct timeval curr_timeval;

        gettimeofday(&curr_timeval, NULL);

        Host* host = &glb_hosts_array[sender_id]; 

        // Check whether anything has arrived
        int input_cmd_length = ll_get_length(host->input_cmdlist_head);
        int inframe_queue_length = ll_get_length(host->incoming_frames_head);
        struct timeval* next_timeout = host_get_next_expiring_timeval(host); 
        
        // Conditions to "wake up" the host:
        //    1) Acknowledgement or new command
        //    2) Timeout      
        int incoming_frames_cmds = (input_cmd_length != 0) | (inframe_queue_length != 0); 
        long reached_timeout = (next_timeout != NULL) && (timeval_usecdiff(&curr_timeval, next_timeout) <= 0);

        host->awaiting_ack = 0; 
        host->active = 0; 
        host->csv_out = 0; 

        if (incoming_frames_cmds || reached_timeout) {
            host->round_trip_num += 1; 
            host->csv_out = 1; 
            
            // Implement this
            handle_input_cmds(host, curr_timeval); 
            // Implement this
            handle_incoming_acks(host, curr_timeval);
            // Implement this
            handle_timedout_frames(host, curr_timeval);
            // Implement this
            handle_outgoing_frames(host, curr_timeval); 
        }

        //Check if we are waiting for acks
        for (int j = 0; j < glb_sysconfig.window_size; j++) {
            if (host->send_window[j].frame != NULL) {
                host->awaiting_ack = 1; 
                break; 
            }
        }

        //Condition to indicate that the host is active 
        if (host->awaiting_ack || ll_get_length(host->buffered_outframes_head) > 0) {
            host->active = 1; 
        }
    }
}