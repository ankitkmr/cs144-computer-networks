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


#define NO 0u
#define YES 1u
#define INITIAL_SEQ_NUMBER 1u /* We'll start with sequence no. 1 */
#define INITIAL_ACK_NUMBER 0u
#define TRANSMISSION_LIMIT 6


/**
 *                       	DESIGN PHILOSOPHY: 
 * So the main server/client loop (do_loop() in ctcp_sys_internal.c) for handling 
 * output i.e. checking STDIN for data to send and sending it, handling timeouts and 
 * retransmissions is single thread sequential.
 *
 * In order to optimise the output handling, in this file, packet transmissions are
 * done by a new thread forked while the original thread can continue to generate 
 * packets and return back to other server functionalities
 *
 * This ensures that server/client isn't bottlenecked by aggregate network output 
 * traffic on it's machine. 
 */



/* struct to hold output state fields of ctcp connection, these fields 
   will be shared by both segment generating and segment transmission threads. */
typedef struct {
	long next_seq_no;		
	linked_list_t *outbound_segments_list;	/* Linked list of segments to be sent 
	                                           to this connection */
} ctcp_outbound_state_t;



/* struct to hold inflight state fields of ctcp connection. Need separate lock and
   access pattern for inflight_segments_list as it will be held while doing 
   network transmissions so latency is expected and separated from main thread */
typedef struct {
	long bytes_inflight;
	linked_list_t *inflight_segments_list;	/* Linked list of segment that are in 
	                                           flight i.e. sent but not received ack */
} ctcp_inflight_state_t;



/* struct to hold input state fields of ctcp connection */
typedef struct {
	long last_ackno_received;
	ll_node_t *last_eligible_output_node;
	linked_list_t *inbound_segments_list;	/* Linked list of segments received from 
	                                           other endpoint*/
} ctcp_inbound_state_t;



/* We'll use this structure to encapsulate the segment to send and put this in 
   ctcp_state outbound lists along with the time when we send the segment and 
   number of times it's been retransmitted */
typedef struct {
	long time_when_sent;
	ctcp_segment_t *segment;
	short transmission_count;	/* number of times the enclosed segment has been sent
	                               includes first time and subsequent retransmissions*/

} timestamped_segment_t;



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

	/* used by only one thread at a time, read into this in ctcp_read */
	char *output_buffer;		

	/* output, input and inflight state to be shared by threads */
	ctcp_outbound_state_t *outbound_state;		
	ctcp_inbound_state_t *inbound_state;
	ctcp_inflight_state_t *inflight_state;

	pthread_mutex_t outbound_state_mutex;		/* mutexes corresponding to each state */
	pthread_mutex_t inbound_state_mutex;		
	pthread_mutex_t inflight_state_mutex;	

	/* condition variable to check if we have new outbound segments or 
	   can send new segments to inflight list i.e. transmit them */
	pthread_cond_t new_outbound_segments_cv;	
	pthread_cond_t new_inflight_segments_cv;

	pthread_t transmission_thread;		/* forked thread for handling transmissions */

	short fin_sent;	
	short fin_received;
	short fin_acked;

	pthread_mutex_t ackno_sent_mutex;
	long _last_ackno_sent;	/* last ack sent by our end. Only access this field via
                               getter and setter methods defined below */
};


/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * retransmit segments and tear down connections.
 */
static ctcp_state_t *state_list = NULL;


/* Getter function to get the value of _next_ackno field of given state */
long get_last_ackno_sent(ctcp_state_t *state){
	pthread_mutex_lock(&state->ackno_sent_mutex);
	long last_ackno_sent = state->_last_ackno_sent;
	pthread_mutex_unlock(&state->ackno_sent_mutex);
	return last_ackno_sent;
}

/* Setter function to update the value of _next_ackno field of given state */
void set_last_ackno_sent(ctcp_state_t *state, long next_ack_to_send){
	pthread_mutex_lock(&state->ackno_sent_mutex);
	state->_last_ackno_sent = next_ack_to_send;
	pthread_mutex_unlock(&state->ackno_sent_mutex);
	return;
}


/* Helper function to allocate memory for output state variable and initialise output state fields */
ctcp_outbound_state_t *outbound_state_init(){
	ctcp_outbound_state_t *outbound_state = malloc(sizeof(ctcp_outbound_state_t));
	outbound_state->next_seq_no = INITIAL_SEQ_NUMBER;
	outbound_state->outbound_segments_list = ll_create();
	return outbound_state;
}


/* Helper function to allocate memory for input state variable and initialise input state fields */
ctcp_inbound_state_t *inbound_state_init(){
	ctcp_inbound_state_t *inbound_state = malloc(sizeof(ctcp_inbound_state_t));
	inbound_state->last_ackno_received = INITIAL_ACK_NUMBER;
	inbound_state->last_eligible_output_node = NULL;
	inbound_state->inbound_segments_list = ll_create();
	return inbound_state;
}


/* Helper function to allocate memory for inflight state variable and initialise inflight state fields */
ctcp_inflight_state_t *inflight_state_init(){
	ctcp_inflight_state_t *inflight_state = malloc(sizeof(ctcp_inflight_state_t));
	inflight_state->bytes_inflight = 0u;
	inflight_state->inflight_segments_list = ll_create();
	return inflight_state;
}



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

	/* Initialize ctcp_state connection fields. */
	state->conn = conn;
	state->config = cfg;
	state->fin_sent = NO;
	state->fin_received = NO;	/* if fin received it stores it's seqno */
	state->fin_acked = NO;

	/*
	  Our main buffer into which we read in data to send or copy in data received
	  (upto MAX_SEG_DATA_SIZE bytes at a time). We then copy the data read from the 
	  output_buffer to an exact size array before adding it to segment 
	*/
	state->output_buffer = malloc(MAX_SEG_DATA_SIZE);
	state->recv_buffer = malloc(MAX_SEG_DATA_SIZE);


	/* Initialize ctcp_state output, input and inflight fields. */
	state->outbound_state = outbound_state_init();
	state->inbound_state = inbound_state_init();
	state->inflight_state = inflight_state_init();


	/* Initialise corresponding mutex locks and condition variables */
	pthread_mutex_init(&state->outbound_state_mutex, NULL);
	pthread_mutex_init(&state->inbound_state_mutex, NULL);
	pthread_mutex_init(&state->inflight_state_mutex, NULL);

	pthread_cond_init(&state->new_inflight_segments_cv, NULL);
	pthread_cond_init(&state->new_outbound_segments_cv, NULL);

	/* Initialise next ackno and its mutex */
	pthread_mutex_init(&state->ackno_mutex, NULL);
	state->_next_ackno = INITIAL_ACK_NUMBER;

	short thread_creation_success = NO;
	while(!thread_creation_success){
	/**
	 * initialise the transmission thread and pass the state we just initialized 
	 * as argument for its procedure call, pthread_create returns 0 on success, 
	 * +ve otherwise, so we negate it
	 */
		 thread_creation_success = !pthread_create(&state->transmission_thread, 
			                                 NULL, send_outbound_tail_segments, state);
	}

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


/**
 * Helper function to get the next timestamped_segment to transmit from outbound 
 * segments list. Waits on new_outbound_segments_cv if outbound_segments_list is empty
 */
timestamped_segment_t *get_next_transmission_segment(ctcp_state_t *state){
	pthread_mutex_lock(&state->outbound_state_mutex);

	timestamped_segment_t *tail_timestamped_segment;
	while(state->outbound_state->outbound_segments_list->head == NULL){
		/* while there is no data to send i.e. no outbound segment, 
		   wait on new_outbound_segments_cv */
		pthread_cond_wait(&new_outbound_segments_cv, &state->outbound_state_mutex);
	}

	/* now outbound_segments_list is not empty */
	tail_timestamped_segment = 
			state->outbound_state->outbound_segments_list->tail->object;
	ll_remove(state->outbound_state->outbound_segments_list, 
			state->outbound_state->outbound_segments_list->tail);
		
	pthread_mutex_unlock(&state->outbound_state_mutex);
	return tail_timestamped_segment;
}



/**
 * Helper function to transmit segment and update segment timestamps. This procedure 
 * is called while holding inflight_state_mutex
 */
void transmit_segment(ctcp_state_t *state, timestamped_segment_t *timestamped_segment){
	short segment_sent_status;
	uint16_t segment_len = ntohs(timestamped_segment->segment->len);

	segment_sent_status = NO;
	while(!segment_sent_status){
		timestamped_segment->time_when_sent = current_time();
		/* unless the number of bytes sent equals the number we intended, we can't 
		   be sure of the integrity of the transmission */
		segment_sent_status = conn_send(state->conn, 
					timestamped_segment->segment, segment_len) == segment_len;
	}

	/* segment successfully sent */
	timestamped_segment->transmission_count += 1;
	return;
}



/**
 * Helper function to send a segment while maintaining transmission window size. It 
 * checks if we can include a new segment into inflight list and if we have no space 
 * to send more bytes, we wait on new_inflight segments_cv
 */
void maintain_transmission_window(ctcp_state_t *state, timestamped_segment_t *timestamped_segment){
	pthread_mutex_lock(&state->inflight_state_mutex);

	uint16_t segment_len = ntohs(timestamped_segment->segment->len);
	uint16_t segment_data_len = segment_len - sizeof(ctcp_segment_t);

	/**
	 * For Lab 2:
	 * 		while(state->config->send_window < 
	 *				state->inflight_state->bytes_inflight + segment_data_len){
	 */
	while(segment_data_len!=0 && bytes_inflight!=0){
		/** 
		 * If it's a data segment and we already have a data segment in-flight then
		 * we can't send another packet and so we wait on condition variable until 
		 * we can and get signalled so. 
		 * 
		 * For segments with no data, we can transmit them irrespective of window size
		 */
		pthread_cond_wait(&new_inflight_segments_cv, &state->inflight_state_mutex);
	}

	/* now window size allows us to add the next segment to transmit to 
	   inflight_segments_list and send it */

	/* actually send the segment, calls conn_send() */
	transmit_segment(state, timestamped_segment);

	if(segment_data_len){
		/* only if it's a data segment do we add it to inflight_segments_list to 
		   monitor when we receive an ACK for it later and handle retransmissions */
		state->inflight_state->bytes_read += segment_data_len;
		ll_add_front(state->inflight_state->inflight_segments_list, timestamped_segment);
	}
	pthread_mutex_unlock(&state->inflight_state_mutex);
	return;
}



/**
 * Transmission thread procedure. It's job is to send out the segments while 
 * maintaining window size. Uses new_inflight_segments_cv to wait if window
 * size worth bytes are already inflight
 *
 * receives pthread_cond_signal() from ctcp_read() and ctcp_receive() 
 */
void send_outbound_tail_segments(void *ctcp_state){
	ctcp_state_t *state = ctcp_state;
	timestamped_segment_t *timestamped_segment;

	while(YES){
		timestamped_segment = get_next_transmission_segment(state);
		/* now we have our next segment we want to send, so we check if our 
		   window size allows us to add it to inflight_segments_list and send it */
		maintain_transmission_window(state, timestamped_segment);
	}
}



/**
 * Helper Function to create a new segment. Returns a pointer to the new segment.
 * This function is called while holding outbound_state_mutex
 */
ctcp_segment_t *create_new_segment(ctcp_state_t *state, int bytes_read){
	/* allocate a new segment */
	ctcp_segment_t *new_segment = calloc(sizeof(ctcp_segment_t),1);
	new_segment->seqno = htonl(state->outbound_state->next_seq_no);

	/* 
	  We are handling acks separately so whenever we ack we update this 
	  field in ctcp_state and while sending in a segment with data, 
	  we simply get the current value of _next_ackno from ctcp_state using
	  it's getter function 
	
	  This allows us to send ACK immediately in response to a data segment
	 */
	new_segment->ackno = htonl(get_last_ackno_sent(state));

	new_segment->flags |= htonl(ACK);
	new_segment->window = htons(state->config->recv_window); 	
	new_segment->cksum = 0u;

	if (bytes_read>0){
		/*create a new data segment. Also the size of data we send in 
		segment will only be equal to the number of bytes we have read 
		and hence have to send */		
		char *data = malloc(bytes_read);
		memcpy(data, state->output_buffer, bytes_read);

		state->outbound_state->next_seq_no += bytes_read;
		new_segment->len = htons(sizeof(ctcp_segment_t) + bytes_read);
		new_segment->data = data;
	}
	else if(bytes_read == -1){
		/*create a fin segment*/
		state->outbound_state->next_seq_no += 1;	/* SYN and FIN packets consume a byte
		                                               in sequence number so that they
		                                               can be separately acknowledged */
		new_segment->len = htons(sizeof(ctcp_segment_t));
		new_fin_segment->flags |= htonl(FIN); 	/* both a FIN segment as well an ACK segment */
		state->fin_sent = YES;
	}
	else{
		/* prepare an ACK segment in response to a a received data segment, no need to
		  increment the sequence number */
		new_segment->len = htons(sizeof(ctcp_segment_t));
	}	


	/* already returns in network byte order */
	new_segment->cksum = cksum(new_segment,ntohs(new_segment->len));
	return new_segment;
}


/**
 * Helper function to encapsulate new ctcp_segment into timestamped segments, and 
 * then add the timestamped segment to front of the outbound segments list 
 *
 * This function is called while holding outbound_state_mutex
 */
void add_segment_to_outbound_list(ctcp_state_t *state, ctcp_segment_t *new_segment){
	timestamped_segment_t *timestamped_segment = calloc(sizeof(timestamped_segment_t),1);
	timestamped_segment->transmission_count = 0u;
	timestamped_segment->segment = new_segment;

	ll_add_front(state->outbound_state->outbound_segments_list, timestamped_segment);
	return;
}


/**
 * This function reads a chunk of the input, puts it in a new segment and appends 
 * it to the outbound segments list in the connection state object. While there is 
 * data to send, keep reading and encapsulating in packets. 
 *
 * If we encounter -1 from conn_input i.e. EOF or error, we send FIN segment 
 *
 * If there is data to send only then acquire the outbound_state_mutex. 
 * 
 * Encapsulate the data we read into a ctcp_segment, encapsulate that into 
 * timestamped_segment_t and add it to front of outbound_segments_list
 * 
 * Here we signal the outbound segments condition variable to wake the transmission
 * thread because now there is data to send available
 */
void ctcp_read(ctcp_state_t *state) {

	int bytes_read; 
	ctcp_segment_t *new_segment;

	while ((bytes_read = conn_input(state->conn, state->output_buffer, 
							MAX_SEG_DATA_SIZE)) != 0 && !(state->fin_sent)){
		/* we acquire and release mutex in loop to separate conn_input() IO-latency. */
		pthread_mutex_lock(&state->outbound_state_mutex);

		new_segment = create_new_segment(state, bytes_read);
		add_segment_to_outbound_list(state, new_segment);
		pthread_cond_signal(&state->new_outbound_segments_cv);

		pthread_mutex_unlock(&state->outbound_state_mutex);
	}
	return;
}


/**
 * Helper function to free ctcp_segment memory, and the memory allocated for 
 * enclosed data
 */
void free_ctcp_segment(ctcp_segment_t *segment){
	free(segment->data);
	free(segment);
	return;
}


/**
 * Helper function to free timestamped segment memory, and the memory allocated for 
 * enclosed segment and its data
 */
void free_timestamped_segment(timestamped_segment_t *timestamped_segment){
	free_ctcp_segment(timestamped_segment->segment);
	free(timestamped_segment);
	return;
}

/**
 * Helper function to free memory allocated for transmission node of inflight 
 * segments list, timestamped_segment and the enclosing ctcp_segment. This function 
 * is to be called while holding inflight_state_mutex
 */
void free_transmission_node(ctcp_state_t *state, ll_node_t *transmission_node){
	free_timestamped_segment(transmission_node->object);
	ll_remove(state->inflight_state->inflight_segments_list, transmission_node);
	return;
}


/**
 * Helper function to update inflight state on receiving an advancing ACK
 * This function is called while holding inbound_state_mutex. In addition it
 * acquires inflight_state_mutex 
 */
void update_inflight_state(ctcp_state_t *state){
	pthread_mutex_lock(&state->inflight_state_mutex);

	long last_ackno_received = state->inbound_state->last_ackno_received;
	ll_node_t *inflight_transmission_node = state->inflight_state->inflight_segments_list->tail;

	/* inflight list is ordered in the decreasing sequence no. order going left-right*/
	while(inflight_transmission_node != NULL){

		timestamped_segment_t *timestamped_segment = inflight_transmission_node->object;
		long segment_seqno = ntohl(timestamped_segment->segment->seqno);
		short segment_data_len = ntohs(timestamped_segment->segment->len) - sizeof(ctcp_segment_t);
		long total_bytes_sent = segment_seqno + segment_data_len - 1;

		/* despite having already updated last_ackno_received we only remove a segment 
		   from inflight_list after we're sure all of it has been received else 
		   we'll let the timer retransmit it */
		if (total_bytes_sent < last_ackno_received){
			/* this segment has been received by the other end, remove it from 
			   list and free it*/
			inflight_transmission_node = inflight_transmission_node->prev;
			free_transmission_node(state, inflight_transmission_node->next);

			/*now update the bytes_inflight and signal transmission thread that it 
			  can send new segments now */
			state->inflight_state->bytes_inflight -= segment_data_len;
			pthread_cond_signal(&state->new_inflight_segments_cv);
		}
		else{
			/* this segment hasn't been acknowledged and segments to the left of this
			   have higher seqno so break loop*/
			break;
		}
	}

	pthread_mutex_unlock(&state->inflight_state_mutex);
	return;
}


/**
 * Helper Function to process ACK bit and/or FIN bits in segments and corresponding 
 * actions.
 */
short process_ctcp_flags(ctcp_state_t *state, ctcp_segment_t *segment){
	pthread_mutex_lock(&state->inbound_state_mutex);

	uint32_t segment_ackno;
	uint32_t segment_seqno;
	uint32_t segment_flags = ntohl(segment->flags);
	short can_we_free_segment = YES;

	if (segment_flags & ACK){	/* ACK bit set */
		segment_ackno = ntohl(segment->ackno);

		/**
		 * since it's possible that an ack might get delayed so we only update the 
		 * last_ackno_received if ack received advances it. 
		 *
		 * Then update the inflight_state for segments still in-flight accordingly 
		 */
		if(state->inbound_state->last_ackno_received < segment_ackno){
			state->inbound_state->last_ackno_received = segment_ackno;
			update_inflight_state(state);
		}
	}

	if (segment_flags & FIN){ /* FIN bit set*/
		/**
		 * We'll treat fin segments like data segments and output EOF after we receive 
		 * all data segments. But since it doesnt have data it wont be processed by 
		 * ctcp_receive(). So we add it to inbound_segments_list here
 		 *
		 * We'll let the timer or some other function see if all
		 * conditions are met and its time to teardown the connection 
		 */
		segment_seqno = ntohl(segment->seqno);
		long last_ackno_sent = get_last_ackno_sent(state);

		if (!state->fin_received){
			state->fin_received = segment_seqno;

			if (last_ackno_sent == segment_seqno){
				/* we have received and acked all data there was so we expect 
				no more data so right away print out EOF, send ack to this fin 
				and update last_ackno_sent */
				conn_output(state->conn, NULL, 0);
				send_response_ack(state, last_ackno_sent + 1);
				state->fin_acked = YES;
			} 
			else {
				/* we add it in inboud segments list to be handled by update_inbound_state
				   after we receive all the data segments. Also now we cant free the
				   segment right away */
				ll_add(state->inbound_state->inbound_segments_list, segment);
				can_we_free_segment = NO;
			}
		}
	}
	pthread_mutex_unlock(&state->inbound_state_mutex);
	return can_we_free_segment;
}



/**
 * Helper function to insert segment into inbound_segments_list. This
 * function acquires the inbound_state_mutex. This function is caled 
 * only when we are sure that the segment is to be kept. 
 *
 * It inserts the segment if its a new segment and returns NO for 
 * duplicacy, else in case of a duplicate segment it returns YES and 
 * doesn't insert the segment
 */
short insert_segment_to_inbound_list(ctcp_state_t *state, ctcp_segment_t *segment){
	pthread_mutex_lock(&state->inbound_state_mutex);

	ctcp_segment_t *node_segment;
	long node_segment_seqno;
	
	short is_segment_duplicate = YES;	/* set by default*/
	long segment_seqno = ntohl(segment->seqno);

	linked_list_t *inbound_segments_list = state->inbound_state->inbound_segments_list;
	ll_node_t *inbound_node = inbound_segments_list->tail;
	ll_node_t *last_eligible_output_node = state->inbound_state->last_eligible_output_node;

	if (inbound_node == NULL){ 
		/* inbound_segments_list is empty */
		ll_add(inbound_segments_list, segment);
		is_segment_duplicate = NO;
	} 
	else{
		/*
		  if inbound segments list is not empty then we have to inspect against
		  segments in the list until we find the right position to insert the
		  received segment. We keep the received segments in the order of increasing
		  sequence numbers left to right. We start with inbound node at tail of list 
		 */
		while (inbound_node != NULL && inbound_node != last_eligible_output_node){
			node_segment = inbound_node->object;
			node_segment_seqno = ntohl(node_segment->seqno);

			if (node_segment_seqno == segment_seqno){
				break; /* it's a duplicate */
			}
			else if (node_segment_seqno < segment_seqno ){
				/* if we're here then all the segments to right of current node are 
				   of larger sequence numbers */
				ll_add_after(inbound_segments_list, inbound_node, segment);
				is_segment_duplicate = NO;
				break;	/* sucessfully inserted */
			}
			else{
				inbound_node = inbound_node->prev;
			}
		}

		if(inbound_node == NULL){
			/*in case a segment comes after delay but is to be kept
			  and has less seqno than all others in the list*/
			ll_add_front(inbound_segments_list, segment);
			is_segment_duplicate = NO;
		}
	}
	pthread_mutex_unlock(&state->inbound_state_mutex);
	return is_segment_duplicate;
}



/**
 * Helper function to update the inbound state upon receiving a data segment.  
 */
void update_inbound_state(ctcp_state_t *state, ctcp_segment_t *segment){
	short is_segment_duplicate = YES;
	long segment_seqno = ntohl(segment->seqno);
	long next_seqno_expected = get_last_ackno_sent(state);

	/* the received segment is to be kept as we havent received bytes upto it 
	   completely. For these segments the response ack will be sent by ctcp_output() 
	   after its data has been printed out */
	if (segment_seqno >= next_seqno_expected){
		is_segment_duplicate = insert_segment_to_inbound_list(state, segment);

		/*by now, if the segment wasnt a duplicate, we've placed the segment, so 
		  call ctcp_output to check if as a result of its insertion we can print 
		  more data out and update last_ackno_sent accordingly */
		if(!is_segment_duplicate){ 
			ctcp_output(state);
		}
	}
	else{
		/* Else it's a duplicate and we dont need it. We have already acked it but 
		   may be that ACK got lost so need to send an ack for it again */
		send_response_ack(state, next_seqno_expected);
	}

	if(is_segment_duplicate){
		free_ctcp_segment(segment);
	}
	return;
}



/**
 * Helper function to check for integrity of the received segment. Returns 0 if 
 * integrity is compromised, non-negative if segment is acceptable
 */
int check_segment_integrity(ctcp_state_t *state, ctcp_segment_t *segment, size_t len){
	uint16_t segment_len = ntohs(segment->len);
	/* segment has been truncated or corrupted */
	return !(segment_len > len || cksum(segment, segment_len) != htons(0xFFFF))
}



/** 
 * First check segments integrity. If the segment has been truncated or corrupted, 
 * we ignore the segment and free it. 
 * 
 * If a segment is acceptable then process its ctcp_flags if any. Also check in case its 
 * a fin segment if can we free it. Rest all segments unless they have data can be freed 
 * right away
 *
 * If a segment with data is received, then check if it's a new segment, place it
 * in sequence number order i.e. update inbound state accordingly. 
 *
 * If data is ready to be outputted, output it by calling ctcp_output() 
 * and do flow control by timing sending an ACK with the updated inbound 
 * state if no buffer space left in conn_bufspace()
 *
 * For data segments we free them after they have been outputted in ctcp_output()
 */
void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
	short can_we_free_segment = YES;
	
	if (check_segment_integrity(state, segment, len)){ 	
		can_we_free_segment = process_ctcp_flags(state, segment);	

		if(segment->data != NULL){
			update_inbound_state(state, segment);
			can_we_free_segment = NO;
		}
	}

	if(can_we_free_segment){
		free_ctcp_segment(segment);
	}
	return;
}



/**
 * This is the only function that updates the last_ackno_sent in inbound state. This 
 * function acquires the inbound state mutex 
 */
void ctcp_output(ctcp_state_t *state) {
	pthread_mutex_lock(&state->inbound_state_mutex);

	ctcp_segment_t *node_segment;
	long node_segment_seqno;
	short node_segment_data_len;
	short output_result;
	ll_node_t *inbound_node;

	linked_list_t *inbound_segments_list = state->inbound_state->inbound_segments_list;	
	long next_seqno_expected = get_last_ackno_sent(state);				

	while (inbound_node = inbound_segments_list->head){
		node_segment = inbound_node->object;
		node_segment_seqno = ntohl(node_segment->seqno);
		node_segment_data_len = ntohs(node_segment->len) - sizeof(ctcp_segment_t);

		if(node_segment_seqno == next_seqno_expected){
			/* means this node can be printed */
			if (conn_bufspace(state->conn) >= node_segment_data_len){
				output_result = conn_output(state->conn, 
					                 node_segment->data, node_segment_data_len);

				if(output_result == -1){
					ctcp_destroy(state);
				}
				else{
					/*successfully printed so update ackno and send a response
					for this segment with the new ackno and then free the segment 
					and free the node */
					next_seqno_expected += node_segment_data_len;
					send_response_ack(state, next_seqno_expected);
					set_last_ackno_sent(state, next_seqno_expected);

					free_ctcp_segment(node_segment);
					ll_remove(inbound_segments_list, inbound_node);
				}
			}
			else{
				/* We cant output so simply break without updating last_seqno_outputted */
				break;
			}
		}
		else{
			/* this segment is not in order yet */
			break;
		}
	}
	pthread_mutex_unlock(&state->inbound_state_mutex);
	return;
}


/**
 * Helper function to inspect inflight segments of given state for retransmissions
 * or an unresponseive connection in which case is tears down the connection by calling
 * ctcp_destroy()
 */
void inspect_state_for_retransmissions(ctcp_state_t *state){
	pthread_mutex_lock(&state->inflight_state_mutex);

	timestamped_segment_t *timestamped_segment;

	int rt_timeout = state->config->rt_timeout;
	linked_list_t *inflight_segments_list = state->inflight_state->inflight_segments_list;
	ll_node_t *inflight_transmission_node = inflight_segments_list->tail;

	/* Here we have to traverse the entire list because the tail might be*/
	while(inflight_transmission_node != NULL){
		timestamped_segment = (timestamped_segment_t *)inflight_transmission_node->object;
		if (current_time() - timestamped_segment->time_when_sent > rt_timeout){
			if(timestamped_segment->transmission_count < TRANSMISSION_LIMIT){
				transmit_segment(state, timestamped_segment);
			}
			else{
				/* unresponsive connection so teardown connection */
				ctcp_destroy(state);
			}
		}
		else{
			if (timestamped_segment->transmission_count ==1){
				/*means segments to the left of this segment and this segment are 
				 freshly sent so break loop and move to next state */
				break;
			}
		}
		inflight_transmission_node = inflight_transmission_node->prev;
	}

	pthread_mutex_unlock(&state->inflight_state_mutex);
	return;
}


void ctcp_timer() {
	ll_node_t *state_node;
	ctcp_state_t *state;

	if (state_list != NULL){
		state_node = state_list->head;

		while (state_node != NULL){
			state = (ctcp_state_t *)state_node->object;
			inspect_state_for_retransmissions(state);
			state_node = state_node->next;
		}
	}
	return;
}

