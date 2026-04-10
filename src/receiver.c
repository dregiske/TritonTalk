#include "host.h"
#include <assert.h>
#include "switch.h"
#include <stdbool.h>

bool check_crc8(Frame* frame){
	char* buffer = convert_frame_to_char(frame);
	int crc8_result = compute_crc8(buffer);
	free(buffer);

	if(crc8_result != 0){
		return false;
	}
	return true;
}

void handle_incoming_frames(Host* host) {
    //    1) Dequeue the Frame from the host->incoming_frames_head
    //    2) Compute CRC of incoming frame to know whether it is corrupted
    //    3) If frame is corrupted, drop it and move on.
    //    4) Implement logic to check if the expected frame has come
    //    5) Implement logic to combine payload received from all frames belonging to a message
    //       and print the final message when all frames belonging to a message have been received.
    //    6) Implement the cumulative acknowledgement part of the sliding window protocol
    //    7) Append acknowledgement frames to the outgoing_frames_head queue
    int incoming_frames_length = ll_get_length(host->incoming_frames_head);
    while (incoming_frames_length > 0) {
        // Pop a node off the front of the link list and update the count
        LLnode* ll_inmsg_node = ll_pop_node(&host->incoming_frames_head);
        incoming_frames_length = ll_get_length(host->incoming_frames_head);

        Frame* inframe = (Frame*)ll_inmsg_node->value;
		
		if (!check_crc8(inframe)){ // check for corruption
			free(inframe);
			free(ll_inmsg_node);
			continue;
		}

		if(inframe->is_ack == 1){ // check for acknowledgement frame
			free(inframe);
			free(ll_inmsg_node);
			continue;
		}

		int src = inframe->src_id;
		int bytes_to_copy = (inframe->remaining_msg_bytes == 0)
							? strlen(inframe->data) + 1
							: strlen(inframe->data);

		uint8_t expected = host->expected_seq_num[inframe->src_id];
		if(inframe->seq_num == expected){
			host->expected_seq_num[inframe->src_id]++;


			// realloc buffer size
			host->message_buffer[src] = realloc(host->message_buffer[src],
												host->message_buffer_len[src] + bytes_to_copy);

			// copy message into message buffer
			memcpy(host->message_buffer[src] + host->message_buffer_len[src],
												inframe->data, bytes_to_copy);
			
			// update length
			host->message_buffer_len[inframe->src_id] += bytes_to_copy;
		}
		else{
			free(inframe);
			free(ll_inmsg_node);
			continue;
		}
		

		if(inframe->remaining_msg_bytes == 0) {
			printf("<RECV_%d>:[%s]\n", host->id, host->message_buffer[src]);
			free(host->message_buffer[src]);
			host->message_buffer[src] = NULL;
			host->message_buffer_len[src] = 0;
		}

		// Build and send ACK frame to host
		Frame* ack = malloc(sizeof(Frame));
		assert(ack);
		ack->dst_id = inframe->src_id;
		ack->src_id = host->id;
		ack->seq_num = inframe->seq_num;
		ack->is_ack = 1;
		ack->remaining_msg_bytes = 0;
		memset(ack->data, 0, FRAME_PAYLOAD_SIZE);
		ack->crc8 = 0;
		char* ack_buf = convert_frame_to_char(ack);
		ack->crc8 = compute_crc8(ack_buf);
		free(ack_buf);

		ll_append_node(&host->outgoing_frames_head, ack);

        free(inframe);
        free(ll_inmsg_node);
    }
}

void run_receivers() {
    int recv_order[glb_num_hosts]; 
    get_rand_seq(glb_num_hosts, recv_order); 

    for (int i = 0; i < glb_num_hosts; i++) {
        int recv_id = recv_order[i]; 
        handle_incoming_frames(&glb_hosts_array[recv_id]); 
    }
}