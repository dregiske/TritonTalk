#include "host.h"
#include <assert.h>
#include "switch.h"


static void deliver_to_buffer(Host* host, int src, Frame* frame) {
    int bytes_to_copy = (frame->remaining_msg_bytes == 0)
                        ? (int)strlen(frame->data) + 1
                        : FRAME_PAYLOAD_SIZE;

    host->message_buffer[src] = realloc(host->message_buffer[src],
                                         host->message_buffer_len[src] + bytes_to_copy);
    memcpy(host->message_buffer[src] + host->message_buffer_len[src],
           frame->data, bytes_to_copy);
    host->message_buffer_len[src] += bytes_to_copy;

    if (frame->remaining_msg_bytes == 0) {
        printf("<RECV_%d>:[%s]\n", host->id, host->message_buffer[src]);
        free(host->message_buffer[src]);
        host->message_buffer[src] = NULL;
        host->message_buffer_len[src] = 0;
    }
}

// is_ack_type=1: cumulative (in-order delivery — covers all frames up to seq_num)
// is_ack_type=2: selective  (out-of-order buffer ACK — covers only this seq_num)
static void send_ack(Host* host, uint8_t dst_id, uint8_t seq_num, uint8_t is_ack_type) {
    Frame* ack = malloc(sizeof(Frame));
    assert(ack);
    ack->dst_id = dst_id;
    ack->src_id = host->id;
    ack->seq_num = seq_num;
    ack->is_ack = is_ack_type;
    ack->remaining_msg_bytes = 0;
    memset(ack->data, 0, FRAME_PAYLOAD_SIZE);
    ack->crc8 = 0;
    char* ack_buf = convert_frame_to_char(ack);
    ack->crc8 = compute_crc8(ack_buf);
    free(ack_buf);
    ll_append_node(&host->outgoing_frames_head, ack);
}

void handle_incoming_frames(Host* host) {
    int incoming_frames_length = ll_get_length(host->incoming_frames_head);
    while (incoming_frames_length > 0) {
        LLnode* ll_inmsg_node = ll_pop_node(&host->incoming_frames_head);
        incoming_frames_length = ll_get_length(host->incoming_frames_head);
        Frame* inframe = (Frame*)ll_inmsg_node->value;
        free(ll_inmsg_node);

        if (!check_crc8(inframe)) { free(inframe); continue; }
        if (inframe->is_ack != 0)  { free(inframe); continue; }

        int src = inframe->src_id;
        uint8_t ack_seq = inframe->seq_num;
        uint8_t expected = host->expected_seq_num[src];
        int diff = seq_num_diff(inframe->seq_num, expected);
        // diff = expected - seq_num:
        //   > 0  old/duplicate frame
        //   == 0 exactly what we want
        //   < 0  future frame (seq > expected)

        if (diff == 0) {
            deliver_to_buffer(host, src, inframe);
            free(inframe);
            host->expected_seq_num[src]++;

            // drain any contiguous buffered frames
            while (1) {
                uint8_t next = host->expected_seq_num[src];
                Frame* buffered = host->recv_ooo_buffer[src * 256 + next];
                if (buffered == NULL) break;
                deliver_to_buffer(host, src, buffered);
                host->recv_ooo_buffer[src * 256 + next] = NULL;
                free(buffered);
                host->expected_seq_num[src]++;
            }
            // is_ack=1: cumulative — covers all frames up to last delivered
            send_ack(host, src, (uint8_t)(host->expected_seq_num[src] - 1), 1);
        } else if (diff < 0) {
            // future frame: buffer if within window, drop if outside
            int abs_diff = -diff;
            if (abs_diff >= glb_sysconfig.window_size) {
                free(inframe);
                continue; // outside window, no ACK
            }
            if (host->recv_ooo_buffer[src * 256 + inframe->seq_num] == NULL) {
                host->recv_ooo_buffer[src * 256 + inframe->seq_num] = inframe;
            } else {
                free(inframe); // duplicate of an already-buffered frame
            }
            // is_ack=2: selective — only clears this specific slot at the sender
            send_ack(host, src, ack_seq, 2);
        } else {
            // old/duplicate: re-ACK as cumulative (harmless, slot already cleared)
            free(inframe);
            send_ack(host, src, ack_seq, 1);
        }
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