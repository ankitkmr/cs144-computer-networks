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


	/* locks to synchronise access to shared linked list between threads */
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
typdef struct {
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

	/*Initializing outbound, inflight and received segments with an empty linked list*/
	state->outbound_segments_list = ll_create();
	state->inflight_segments_list = ll_create();		
	state->received_segments_list = ll_create();

	/* Initialise mutex locks */
	pthread_mutex_init(&(state->outbound_list_lock), NULL);
	pthread_mutex_init(&(state->inflight_list_lock), NULL);		
	pthread_mutex_init(&(state->received_list_lock), NULL);

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


/* Helper Function to copy n chars from from[] to to[] */
void array_copy(char *to, char *from, int n){
	int i;
	for(i=0;i<n;i++){
		to[i]=from[i];
	}
}


/* Helper Function to create a new segment with data. Returns a pointer to the new segment */
ctcp_segment_t *create_new_data_segment(ctcp_state_t *state, int bytes_read, char *buffer){

	/* allocate a new segment */
	ctcp_segment_t *new_segment = calloc(sizeof(ctcp_segment_t),1);

	/* size of data we send in segment will only be equal to the number of
	 bytes we have read and hence have to send */		
	char *data = malloc(bytes_read);
	array_copy(data, buffer, bytes_read);	

	new_segment->seqno = htonl(state->next_seq_no);
	state->next_seq_no += bytes_read;

	/* We are handling acks separately so whenever we ack we update this 
	  field in ctcp_state and while sending in a segment with data, 
	  we simply copy the current value of last_ack_sent from ctcp_state */
	new_segment->ackno = htonl(state->last_ack_sent); 

	new_segment->len = htons(sizeof(ctcp_segment_t)+bytes_read);
	new_segment->flags |= htonl(ACK);
	new_segment->window = htons(state->config->recv_window); 	

	new_segment->cksum = 0;
	new_segment->data = data;

	/* already returns in network byte order */
	new_segment->cksum = cksum(new_segment,ntohs(new_segment->len));	

	return new_segment;
}


/* Helper Function to create a new fin segment. Returns a pointer to the new segment */
ctcp_segment_t *create_new_fin_segment(ctcp_state_t *state){

	/* allocate a new segment */
	ctcp_segment_t *new_fin_segment = calloc(sizeof(ctcp_segment_t),1);	

	new_fin_segment->seqno = htonl(state->next_seq_no);
	/* no need to update next_seq_no field now as we dont intend to 
	   send any more segments*/

	/* We are handling acks separately so whenever we ack we update this 
	  field in ctcp_state and while sending in a segment with data, 
	  we simply copy the current value of last_ack_sent from ctcp_state */
	new_fin_segment->ackno = htonl(state->last_ack_sent); 

	new_fin_segment->len = htons(sizeof(ctcp_segment_t));

	/* both a FIN segment as well an ACK segment */
	new_fin_segment->flags |= htonl(ACK);
	new_fin_segment->flags |= htonl(FIN);

	new_fin_segment->window = htons(state->config->recv_window); 	
	new_fin_segment->cksum = 0u;	
	
	/* already returns in network byte order */
	new_fin_segment->cksum = cksum(new_fin_segment,ntohs(new_fin_segment->len));	

	return new_fin_segment;
}


/* this function is called by child thread forked by ctcp_read. It's job is to 
   send out the segments while maintaining window size */
void send_outbound_tail_segments(ctcp_state_t *state){

	short send_window_size = state->config->send_window;
	timestamped_segment_t *tail_timestamped_segment;

	if(send_window_size >  state->bytes_inflight){
		/* can send more packets so remove the last node from outbound and add to
		   inflight list and then send it*/

		pthread_mutex_lock(&(state->outbound_list_lock));

		if(state->outbound_segments_list->head != NULL){
			tail_timestamped_segment = state->outbound_segments_list->tail->object;
			ll_remove(state->outbound_segments_list, state->outbound_segments_list->tail);
		} 
		else {
			/*no segments to send, kill the forked thread*/
			thread_exit(0);
		}

		pthread_mutex_unlock(&(state->outbound_list_lock));


		pthread_mutex_lock(&(state->inflight_list_lock));
		pthread_mutex_unlock(&(state->inflight_list_lock));


	}
	/* In stop and wait, we send a segment and then wait for it's ack. 
	   While we're waiting, we keep the last sent segment, in case we need to retransmit it*/
	if(state->last_ack_received > 
		state->outbound_segments_list->tail->object->segment->seqno){
		/* means we can now */
		conn_send
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
 *
 */
void ctcp_read(ctcp_state_t *state) {

	int bytes_read;
	ctcp_segment_t *new_segment;
	timestamped_segment_t *timestamped_segment;

	/*Our main buffer into which we read in data (upto MAX_SEG_DATA_SIZE bytes at a time)
	  We then copy the data read from the buffer to an exact size array before adding it to segment*/
	char* buffer = malloc(MAX_SEG_DATA_SIZE);
	int have_we_forked = 0;		/* have we forked yet*/
	int am_i_parent = 1;		/* is the current thread the parent thread, start with non-zero value;
							   will contain fork return value after fork so non-zero for parent, zero for child*/

	while (am_i_parent>0 && (bytes_read = conn_input(state->conn, buffer, MAX_SEG_DATA_SIZE)) > 0){
		/* encapsulate the data we read into a ctcp segment and add it to head of outbound segment list*/
		new_segment = create_new_data_segment(state, bytes_read, buffer);

		timestamped_segment = calloc(sizeof(timestamped_segment_t),1);
		timestamped_segment->segment = new_segment;

		/* Acquire outbound list lock and add the new segment encapsulated 
		   within timestamp struct to the front of the list*/
		pthread_mutex_lock(&(state->outbound_list_lock));
		ll_add_front(state->outbound_segments_list, timestamped_segment);
		pthread_mutex_unlock(&(state->outbound_list_lock));

		if (!have_we_forked){
			/* fork a child thread to simultaneously start sending the segments
			   and maintain window size while the parent thread continues to 
			   generate more segments */
			am_i_parent = fork();	/* returns positive child_pid to parent and 0 to child */
			if (am_i_parent>=0){	/*successful fork*/
				have_we_forked=1;
			}
			else{
				am_i_parent=1;	/* reset and try again */
			}
		}
	}

	if(!am_i_parent){
		send_outbound_tail_segments(state);
	}
	else{ 
		/* if encountered EOF or error, terminate the connection */
		if (bytes_read == -1){	
			timestamped_segment = calloc(sizeof(timestamped_segment_t),1);
			timestamped_segment->segment = create_new_fin_segment(state);
			pthread_mutex_lock(&(state->outbound_list_lock));
			ll_add_front(state->outbound_segments_list,timestamped_segment);
			pthread_mutex_unlock(&(state->outbound_list_lock));
		}
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
