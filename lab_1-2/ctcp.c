/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include <pthread.h>

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"


#define INITIAL_SEQ_NO 1u /* We'll start with sequence no. 1 */
#define NO 0u
#define YES 1u


/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {

	struct ctcp_state *next;    /* Next in linked list of active ctcp states*/
	struct ctcp_state **prev;   /* Prev in linked list of active ctcp states*/

	conn_t *conn;				/* Connection object -- needed in order to figure
							  	   out destination when sending */

	ctcp_config_t *config;		/* Connection configuration object for this connection.
	                               It has the receive window size, send window size and 
	                               retransmission timeout for current TCP connection */

	int next_seq_no;		
	int last_ack_sent;
	int last_ack_received;

	pthread_mutex_t output_thread_mutex;	/* to synchronise access to io_thread and its state fields */

	pthread_t output_thread;
	short is_output_thread_running;

	/* locks to synchronise access to shared linked list between threads. 
	   Need separate locks because the lists they protect are used in 
	   separate cases and access to them are critical to performance */
	pthread_mutex_t outbound_list_lock;	
	pthread_mutex_t received_list_lock;
	pthread_mutex_t inflight_list_lock;
	
	linked_list_t *outbound_segments_list;	/* Linked list of segments to be sent to this connection */
	linked_list_t *inflight_segments_list;	/* Linked list of segment that are in flight i.e. sent but not received ack */
	linked_list_t *received_segments_list;	/* Linked list of segments received from other endpoint*/

	short bytes_inflight;

};


/* We'll use this structure to encapsulate the segment to send 
   and put this in ctcp_state outbound lists along with the time 
   when we send the segment
 */
typedef struct {
	long time_when_sent;
	ctcp_segment_t *segment;
} timestamped_segment_t;


/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;


ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
	/* Connection could not be established. */
	if (conn == NULL) {
		return NULL;
	}

	/* Established a connection. Create a new state and update the linked list
	 of connection states. */
	ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
	state->next = state_list;
	state->prev = &state_list;
	if (state_list)
		state_list->prev = &state->next;
	state_list = state;

	/* Initialize ctcp_state fields. */
	state->conn = conn;
	state->config = cfg;
	state->next_seq_no = INITIAL_SEQ_NO;	
	state->last_ack_sent = 0u;
	state->last_ack_received = 0u;
	state->bytes_inflight = 0u;

	
	/* Initialise mutex locks */
	pthread_mutex_init(&output_thread_mutex, NULL);

	pthread_mutex_init(&(state->outbound_list_lock), NULL);
	pthread_mutex_init(&(state->inflight_list_lock), NULL);		
	pthread_mutex_init(&(state->received_list_lock), NULL);

	state->is_output_thread_running = NO;

	/*Initializing outbound, inflight and received segments with an empty linked list*/
	state->outbound_segments_list = ll_create();
	state->inflight_segments_list = ll_create();		
	state->received_segments_list = ll_create();


	/* now add the state to the state_list*/
	if(state_list == NULL){
		state_list = ll_create();
	}
	ll_add(state_list, state);
	return state;
}


void ctcp_destroy(ctcp_state_t *state) {
	/* Update linked list. */
	if (state->next)
	state->next->prev = state->prev;

	*state->prev = state->next;
	conn_remove(state->conn);

	/* FIXME: Do any other cleanup here. */

	free(state);
	end_client();
}


/* Helper Function to create a new segment. Returns a pointer to the new segment */
ctcp_segment_t *create_new_segment(ctcp_state_t *state, int bytes_read, char *buffer){

	/* allocate a new segment */
	ctcp_segment_t *new_segment = calloc(sizeof(ctcp_segment_t),1);
	new_segment->seqno = htonl(state->next_seq_no);

	/* We are handling acks separately so whenever we ack we update this 
	  field in ctcp_state and while sending in a segment with data, 
	  we simply copy the current value of last_ack_sent from ctcp_state */
	new_segment->ackno = htonl(state->last_ack_sent);

	new_segment->flags |= htonl(ACK);
	new_segment->window = htons(state->config->recv_window); 	
	new_segment->cksum = 0u;


	if (bytes_read>0){
		/*create a new data segment. Also the size of data we send in 
		segment will only be equal to the number of bytes we have read 
		and hence have to send */		
		char *data = malloc(bytes_read);
		memcpy(data, buffer, bytes_read);

		state->next_seq_no += bytes_read;
		new_segment->len = htons(sizeof(ctcp_segment_t)+bytes_read);
		new_segment->data = data;
	}
	else if(bytes_read == -1){
		/*create a fin segment*/
		state->next_seq_no += 1;	/*for sending ack to a fin we'll receive*/
		new_segment->len = htons(sizeof(ctcp_segment_t));
		new_fin_segment->flags |= htonl(FIN); 	/* both a FIN segment as well an ACK segment */


	}
	else{
		fprintf(stderr, "Unintended Call to create_new_segment\n", );
	}	

	/* already returns in network byte order */
	new_segment->cksum = cksum(new_segment,ntohs(new_segment->len));	
	return new_segment;
}



/* this function is called by child thread forked by ctcp_read. It's job is to 
   send out the segments while maintaining window size */
void send_outbound_tail_segments(void *state){

	timestamped_segment_t *tail_timestamped_segment;
	ctcp_state_t ctcp_state = (ctcp_state_t *)state;

	while( state->config->send_window >= state->bytes_inflight + MAX_SEG_DATA_SIZE){
		/* can send more packets so remove the last node from outbound and add to
		   inflight list and then send it*/

		pthread_mutex_lock(&(state->outbound_list_lock));

		if(state->outbound_segments_list->head != NULL){
			tail_timestamped_segment = state->outbound_segments_list->tail->object;
			ll_remove(state->outbound_segments_list, state->outbound_segments_list->tail);
			pthread_mutex_unlock(&(state->outbound_list_lock));

			pthread_mutex_lock(&(state->inflight_list_lock));
			tail_timestamped_segment->time_when_sent = current_time();
			ll_add_front(inflight_segments_list,tail_timestamped_segment);

			conn_send(state->conn, tail_timestamped_segment->segment,
						tail_timestamped_segment->segment->len);		
			pthread_mutex_unlock(&(state->inflight_list_lock));
		} 
		else {
			/* sent all segments, no more segments to send, kill the forked thread
			  but also make sure no other ctcp_read thread is assuming the state 
			  of this thread as still running */

			/* important to unlock the outbound list mutex after output thread mutex
			  to ensure that no ctcp_thread can add a segment while we exit the output
			  thread on assumption that we finished the outbound segments. That thread
			  then will have to fork a new output thread */
			pthread_mutex_lock(&output_thread_mutex);
			state->is_output_thread_running = NO;
			pthread_mutex_unlock(&output_thread_mutex);

			pthread_mutex_unlock(&(state->outbound_list_lock));
			thread_exit(0);
		}
	}
}


/* 
 * This function reads the input, breaks it into segments and appends it to 
 * the outbound segments list in the connection state object 
 *
 * On adding each new segment, we check if a new segment from the tail of the 
 * outbound_segments_list in the connection state can be sent and then send it
 *
 * REMEMBER: 
 * In this function, Memory allocated for: segments and timestamped segments and is
 * to be freed when sent and acked by other end
 */
void ctcp_read(ctcp_state_t *state) {

	int bytes_read;
	ctcp_segment_t *new_segment;
	timestamped_segment_t *timestamped_segment;

	/*Our main buffer into which we read in data (upto MAX_SEG_DATA_SIZE bytes at a time)
	  We then copy the data read from the buffer to an exact size array before adding it to segment*/
	char* buffer = malloc(MAX_SEG_DATA_SIZE);
	short need_to_fork_thread = YES;
	
	while ((bytes_read = conn_input(state->conn, buffer, MAX_SEG_DATA_SIZE)) > 0){
		/* encapsulate the data we read into a ctcp segment and add it to head of outbound segment list*/
		new_segment = create_new_segment(state, bytes_read, buffer);

		timestamped_segment = calloc(sizeof(timestamped_segment_t),1);
		timestamped_segment->segment = new_segment;

		/* Acquire outbound list lock and add the new segment encapsulated 
		   within timestamp struct to the front of the list*/
		pthread_mutex_lock(&(state->outbound_list_lock));
		ll_add_front(state->outbound_segments_list, timestamped_segment);
		pthread_mutex_unlock(&(state->outbound_list_lock));

		if (need_to_fork_thread){
			/* fork a child thread to simultaneously start sending the segments
			   and maintain window size while the parent thread continues to 
			   generate more segments 

			   See if output thread is already running from earlier call, or if we
			   need to fork new thread and update thread status accordingly 
			*/
			pthread_mutex_lock(&output_thread_mutex);

			if (!state->is_output_thread_running){
				need_to_fork_thread = 
					pthread_create(&(state->output_thread), NULL, 
							send_outbound_tail_segments, (void *)state);
				if(!need_to_fork_thread){
					state->is_output_thread_running = YES;
				}
			}
			else{
				/* io has more latency than computing, so it may take longer for output
				    thread to send segments and in that time the library may call ctcp_read again */
				need_to_fork_thread = NO;
			}
			pthread_mutex_unlock(&output_thread_mutex);
		}
	}

	/* if encountered EOF or error, terminate the connection */
	if (bytes_read == -1){	
		timestamped_segment = calloc(sizeof(timestamped_segment_t),1);
		timestamped_segment->segment = create_new_segment(state, bytes_read, NULL);
		pthread_mutex_lock(&(state->outbound_list_lock));
		ll_add_front(state->outbound_segments_list,timestamped_segment);
		pthread_mutex_unlock(&(state->outbound_list_lock));
	}
}



void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
}

void ctcp_timer() {
  /* FIXME */
}
