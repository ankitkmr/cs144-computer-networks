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
#define RETRANSMISSION_LIMIT 5


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



/**
 * struct to hold output state fields of ctcp connection, these fields 
 * will be shared by both segment generating and segment transmission threads. 
 */
typedef struct {
	int next_seq_no;		
	int last_ack_sent;

	linked_list_t *outbound_segments_list;	/* Linked list of segments to be sent 
	                                           to this connection */
} ctcp_output_state_t;



/**
 * struct to hold inflight state fields of ctcp connection. Need separate lock and
 * access pattern for inflight_segments_list as it will be held while doing 
 * network transmissions so latency is expected and separated from main thread */
typedef struct {
	long bytes_inflight;
	linked_list_t *inflight_segments_list;	/* Linked list of segment that are in 
	                                           flight i.e. sent but not received ack */
} ctcp_inflight_state_t;



/**
 * struct to hold input state fields of ctcp connection. 
 */
typedef struct {
	int last_ack_received;
	linked_list_t *received_segments_list;	/* Linked list of segments received from 
	                                           other endpoint*/
} ctcp_input_state_t;



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

	/* output, input and inflight state to be shared by threads */
	ctcp_output_state_t *output_state;		
	ctcp_input_state_t *input_state;
	ctcp_inflight_state_t *inflight_state;

	pthread_mutex_t output_state_mutex;		/* mutexes corresponding to each state */
	pthread_mutex_t recv_state_mutex;		
	pthread_mutex_t inflight_state_mutex;	

	/* used by only one thread at a time, read into this in ctcp_read */
	char *output_buffer;		
	char *recv_buffer;

	/* condition variable to check if we have new outbound segments or 
	   can send new segments to inflight list i.e. transmit them */
	pthread_cond_t new_outbound_segments_cv;	
	pthread_cond_t new_inflight_segments_cv;

	pthread_t output_thread;		/* forked thread for handling transmissions */
	int keep_output_thread_running;	/* we set this to NO to exit infinite loop of 
	                                   output_thread procedure before killing that
	                                   thread */
	short fin_sent;	
};


/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;


/* We'll use this structure to encapsulate the segment to send 
   and put this in ctcp_state outbound lists along with the time 
   when we send the segment and number of times it's been retransmitted
 */
typedef struct {
	short transmission_count;	/* number of times the enclosed segment has been sent
	                               includes first time and subsequent retransmissions*/
	long time_when_sent;
	ctcp_segment_t *segment;
} timestamped_segment_t;


/* Helper function to allocate memory for output state variable and
   initialise output state fields */
ctcp_output_state_t *output_state_init(){
	ctcp_output_state_t *output_state = malloc(sizeof(ctcp_output_state_t));
	output_state->next_seq_no = INITIAL_SEQ_NUMBER;
	output_state->last_ack_sent = 0u;
	output_state->outbound_segments_list = ll_create();
	return output_state;
}

/* Helper function to allocate memory for input state variable and
   initialise input state fields */
ctcp_input_state_t *input_state_init(){
	ctcp_input_state_t *input_state = malloc(sizeof(ctcp_input_state_t));
	input_state->last_ack_received = 0u;
	input_state->received_segments_list = ll_create();
	return input_state;
}

/* Helper function to allocate memory for inflight state variable and
   initialise inflight state fields */
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


	/* Initialize ctcp_state output, input and inflight fields. */
	state->output_state = output_state_init();
	state->input_state = input_state_init();
	state->inflight_state = inflight_state_init();

	/*
	  Our main buffer into which we read in data to send or copy in data received
	  (upto MAX_SEG_DATA_SIZE bytes at a time). We then copy the data read from the 
	  output_buffer to an exact size array before adding it to segment 
	*/
	state->output_buffer = malloc(MAX_SEG_DATA_SIZE);
	state->recv_buffer = malloc(MAX_SEG_DATA_SIZE);

	/* Initialise corresponding mutex locks and condition variables */
	pthread_mutex_init(&(state->output_state_mutex), NULL);
	pthread_mutex_init(&(state->intput_state_mutex), NULL);
	pthread_mutex_init(&(state->inflight_state_mutex), NULL);

	pthread_cond_init(&(state->new_inflight_segments_cv), NULL);
	pthread_cond_init(&(state->new_outbound_segments_cv), NULL);


	state->keep_output_thread_running = NO;
	while(!(state->keep_output_thread_running)){
	/**
	 * initialise the output thread and pass the state we just initialized 
	 * as argument for its procedure call, pthread_create returns 0 on success, 
	 * +ve otherwise 
	 */
		state->keep_output_thread_running = !pthread_create(&(state->output_thread), 
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
	pthread_mutex_lock(&(state->output_state_mutex));

	timestamped_segment_t *tail_timestamped_segment;
	while(state->output_state->outbound_segments_list->head == NULL){
		/* while there is no data to send i.e. no outbound segment, 
		   wait on new_outbound_segments_cv */
		pthread_cond_wait(&new_outbound_segments_cv, &(state->output_state_mutex));
	}

	/* now outbound_segments_list is not empty */
	tail_timestamped_segment = 
			state->output_state->outbound_segments_list->tail->object;
	ll_remove(state->output_state->outbound_segments_list, 
			state->output_state->outbound_segments_list->tail);
		
	pthread_mutex_unlock(&(state->outbound_list_lock));
	return tail_timestamped_segment;
}


/**
 * Helper function to send a segment while maintaining transmission window size. It 
 * checks if we can include a new segment into inflight list and if we have no space 
 * to send more bytes, we wait on new_inflight segments_cv
 */
void maintain_transmission_window(ctcp_state_t *state, timestamped_segment_t *timestamped_segment){
	pthread_mutex_lock(&(state->inflight_list_lock));

	short segment_sent_status;
	uint16_t segment_len = ntohs(timestamped_segment->segment->len);
	uint16_t segment_payload_len = segment_len - sizeof(ctcp_segment_t);

	while(state->config->send_window < 
				state->inflight_state->bytes_inflight + segment_payload_len){
		/* We cant send another packet so wait on condition variable until 
		   we can and get signalled so */
		pthread_cond_wait(&new_inflight_segments_cv, &(state->inflight_state_mutex));
	}

	/* now window size allows us to add the next segment to transmit to 
	   inflight_segments_list and send it so we timestamp the segment before 
	   sending it */

	segment_sent_status = NO;
	while(!segment_sent_status){
		timestamped_segment->time_when_sent = current_time();
		/* unless the number of bytes sent equals the number we intended, we can't 
		   be sure of the integrity of the transmission */
		segment_sent_status = conn_send(state->conn, 
					timestamped_segment->segment, segment_len) == segment_len;
	}

	/* segment successfully sent */
	timestamped_segment->transmission_count = 1u;		
	ll_add(inflight_segments_list, timestamped_segment);

	pthread_mutex_unlock(&(state->inflight_list_lock));
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

	/* we set this to NO in ctcp_destroy, just before killing this thread */
	while(state->keep_output_thread_running == YES){

		timestamped_segment = get_next_transmission_segment(state);
		/* now we have our next segment we want to send, so we check if we can 
		   window size allows us to add it to inflight_segments_list and send it */
		maintain_transmission_window(state, timestamped_segment);
	}
}



/**
 * Helper Function to create a new segment. Returns a pointer to the new segment.
 * This function is called while holding output_state_mutex
 */
ctcp_segment_t *create_new_segment(ctcp_state_t *state, int bytes_read){
	/* allocate a new segment */
	ctcp_segment_t *new_segment = calloc(sizeof(ctcp_segment_t),1);
	new_segment->seqno = htonl(state->output_state->next_seq_no);

	/* We are handling acks separately so whenever we ack we update this 
	  field in ctcp_state and while sending in a segment with data, 
	  we simply copy the current value of last_ack_sent from ctcp_state */
	new_segment->ackno = htonl(state->output_state->last_ack_sent);

	new_segment->flags |= htonl(ACK);
	new_segment->window = htons(state->config->recv_window); 	
	new_segment->cksum = 0u;

	if (bytes_read>0){
		/*create a new data segment. Also the size of data we send in 
		segment will only be equal to the number of bytes we have read 
		and hence have to send */		
		char *data = malloc(bytes_read);
		memcpy(data, state->output_buffer, bytes_read);

		state->output_state->next_seq_no += bytes_read;
		new_segment->len = htons(sizeof(ctcp_segment_t) + bytes_read);
		new_segment->data = data;
	}
	else if(bytes_read == -1){
		/*create a fin segment*/
		state->output_state->next_seq_no += 1;	/*for sending ack to a fin we'll receive*/
		new_segment->len = htons(sizeof(ctcp_segment_t));
		new_fin_segment->flags |= htonl(FIN); 	/* both a FIN segment as well an ACK segment */
		state->fin_sent = YES;
	}
	else{
		fprintf(stderr, "Unintended call to create_new_segment() with 0 bytes_read\n", );
	}	

	/* already returns in network byte order */
	new_segment->cksum = cksum(new_segment,ntohs(new_segment->len));
	return new_segment;
}


/**
 * Helper function to encapsulate new ctcp_segment into timestamped segments, and 
 * then add the timestamped segment to front of the outbound segments list 
 *
 * This function is called while holding output_state_mutex
 */
void add_segment_to_outbound_list(ctcp_state_t *state, ctcp_segment_t *new_segment){
	timestamped_segment_t *timestamped_segment = calloc(sizeof(timestamped_segment_t),1);
	timestamped_segment->transmission_count = 0u;
	timestamped_segment->segment = new_segment;

	ll_add_front(state->output_state->outbound_segments_list, timestamped_segment);
	return;
}


/**
 * This function reads a chunk of the input, puts it in a new segment and appends 
 * it to the outbound segments list in the connection state object. While there is 
 * data to send, keep reading and encapsulating in packets. 
 *
 * If we encounter -1 from conn_input i.e. EOF or error, we send FIN segment 
 *
 * If there is data to send only then acquire the output_state_mutex. 
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
		pthread_mutex_lock(&(state->output_state_mutex));

		new_segment = create_new_segment(state, bytes_read);
		add_segment_to_outbound_list(state, new_segment);
		pthread_cond_signal(&(state->new_outbound_segments_cv));

		pthread_mutex_unlock(&(state->output_state_mutex));
	}
	return;
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
