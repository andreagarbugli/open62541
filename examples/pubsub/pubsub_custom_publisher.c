#if !defined(_XOPEN_SOURCE) && !defined(_WRS_KERNEL)
# define _XOPEN_SOURCE 600
#endif

#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef SO_EE_ORIGIN_TXTIME
#define SO_EE_ORIGIN_TXTIME		6
#define SO_EE_CODE_TXTIME_INVALID_PARAM	1
#define SO_EE_CODE_TXTIME_MISSED	2
#endif

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <asm/types.h>
#include <linux/errqueue.h>
#include <poll.h>
#include <unistd.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/plugin/pubsub_udp_tsn.h>
#include <open62541/server_pubsub.h>
#include <open62541/types.h>
#include <open62541/plugin/pubsub.h>
#include <ua_pubsub.h>
#include <open62541/plugin/pubsub_ethernet_tsn.h>

#define ONE_SEC 1000000000

#define DEFAULT_INTERVAL 1000

#define DEFAULT_DELAY		500000

#define DEFAULT_PRIORITY	3


UA_WriterGroup *writerGroupRT;
UA_PubSubConnection *pubSubConnection;

UA_Boolean running = UA_TRUE;

UA_NodeId connectionIdent;
UA_NodeId publishedDataSetIdent;
UA_NodeId writerGroupIdent;
UA_NodeId counterNodeId;

static uint64_t pubInterval = DEFAULT_INTERVAL;

UA_UInt16 deadlineMode = 0;
UA_UInt16 receiveErrors = 0;
UA_UInt64 maxMessageNumber = 1000000; 

static uint64_t waketx_delay = DEFAULT_DELAY;
static uint64_t base_time = 0;

FILE* fp;
char* fileName = "tmptrace.csv";

size_t cpuNumber = 0;

typedef struct MessageTrace {
	uint64_t sendTime;
	uint64_t txTime;
	uint64_t wakeupTime;
	uint8_t missDeadline;
	uint64_t msgNumber;
} MessageTrace;

MessageTrace* messageTraces;

/**
 * **DataSetField handling**
 *
 * The DataSetField (DSF) is part of the PDS and describes exactly one published
 * field. */
static void
addDataSetField(UA_Server *server);

/**
 * **DataSetWriter handling**
 *
 * A DataSetWriter (DSW) is the glue between the WG and the PDS. The DSW is
 * linked to exactly one PDS and contains additional informations for the
 * message generation. */
static void
addDataSetWriter(UA_Server *server);

/**
 * **PublishedDataSet handling**
 *
 * The PublishedDataSet (PDS) and PubSubConnection are the toplevel entities and
 * can exist alone. The PDS contains the collection of the published fields. All
 * other PubSub elements are directly or indirectly linked with the PDS or
 * connection. */
static void
addPublishedDataSet(UA_Server *server);

static void
addPubSubConnection(UA_Server *server, UA_String *transportProfile,
                    UA_NetworkAddressUrlDataType *networkAddressUrl);

static void
addVariableScalar(UA_Server *server, void *value, int ua_types_id, char *name);

/**
 * **WriterGroup handling**
 *
 * The WriterGroup (WG) is part of the connection and contains the primary
 * configuration parameters for the message creation. */
static void
addWriterGroup(UA_Server *server);

void
normalizeTimeSpec(struct timespec *timespec);

void
*publisherTBS(void *server);

static int
run(UA_String *transportProfile, UA_NetworkAddressUrlDataType *networkAddressUrl);

static void stopHandler(int sign) {
    printf("received ctrl-c");
    running = UA_FALSE;
}

int main(int argc, char *argv[]) {
    UA_String transportProfile =
            UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-udp-uadp");
    UA_NetworkAddressUrlDataType networkAddressUrl =
            {UA_STRING_NULL, UA_STRING("opc.udp://224.0.0.1:4840/")};

    int c;
    while (EOF != (c = getopt(argc, argv, "u:d:DEp:i:b:m:w:c:"))) 
    {
	switch (c) 
	{
        case 'u':
            if(strncmp(optarg, "opc.udp://", 10) == 0)
            {
                networkAddressUrl.url = UA_STRING(optarg);
                networkAddressUrl.networkInterface = UA_STRING("127.0.0.1");
            }
            else if (strncmp(optarg, "opc.eth://", 10) == 0)
            {
                transportProfile =
                    UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-eth-uadp");
                networkAddressUrl.url = UA_STRING(optarg);
            }
            break;
	case 'c':
	    cpuNumber = strtoul(optarg, NULL, 10);
            break;
	case 'D':
	    deadlineMode = SOF_TXTIME_DEADLINE_MODE;
	    break;
        case 'E':
	    receiveErrors = SOF_TXTIME_REPORT_ERRORS;
	    break;
	case 'd':
	    waketx_delay = strtoul(optarg, NULL, 10);
	    break;
	case 'i':
	    networkAddressUrl.networkInterface = UA_STRING(optarg);
	    break;
	case 'p':
	    pubInterval = strtoul(optarg, NULL, 10);
	    break;
	case 'b':
	    base_time = strtoul(optarg, NULL, 10);
	    break;
	case 'm':
	    maxMessageNumber = strtoul(optarg, NULL, 10);
	    break;
	case 'w':
	    fileName = optarg;
	    break;
	case '?':
	    return -1;
	}
    }

    return run(&transportProfile, &networkAddressUrl);
}

static void
addVariableScalar(UA_Server *server, void *value, int ua_types_id, char *name) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Variant_setScalar(&attr.value, value, &UA_TYPES[ua_types_id]);

    attr.description = UA_LOCALIZEDTEXT("en-US", name);
    attr.displayName = UA_LOCALIZEDTEXT("en-US", name);
    attr.dataType = UA_TYPES[ua_types_id].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

    counterNodeId = UA_NODEID_STRING(1, name);
    UA_QualifiedName integerName = UA_QUALIFIEDNAME(1, name);

    UA_NodeId parentNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentReferenceNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);

    UA_Server_addVariableNode(server, counterNodeId, parentNodeId,
                              parentReferenceNodeId, integerName,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                              attr, NULL, NULL);
}

static void
addDataSetField(UA_Server *server) {
    UA_NodeId dataSetFieldIdent;
    UA_DataSetFieldConfig dataSetFieldConfig;
    memset(&dataSetFieldConfig, 0, sizeof(UA_DataSetFieldConfig));
    dataSetFieldConfig.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
    dataSetFieldConfig.field.variable.fieldNameAlias = UA_STRING("Message Counter");
    dataSetFieldConfig.field.variable.promotedField = UA_FALSE;
    dataSetFieldConfig.field.variable.publishParameters.publishedVariable = UA_NODEID_STRING(1, "counter");
    dataSetFieldConfig.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;

    UA_Server_addDataSetField(server, publishedDataSetIdent, &dataSetFieldConfig, &dataSetFieldIdent);
}

static void
addDataSetWriter(UA_Server *server) {
    UA_NodeId dataSetWriterIdent;
    UA_DataSetWriterConfig dataSetWriterConfig;
    memset(&dataSetWriterConfig, 0, sizeof(UA_DataSetWriterConfig));
    dataSetWriterConfig.name = UA_STRING("Demo DataSetWriter");
    dataSetWriterConfig.dataSetWriterId = 62541;
    dataSetWriterConfig.keyFrameCount = 10;

    UA_Server_addDataSetWriter(server, writerGroupIdent, publishedDataSetIdent, &dataSetWriterConfig,
                               &dataSetWriterIdent);
}

static void
addPublishedDataSet(UA_Server *server) {
    UA_PublishedDataSetConfig publishedDataSetConfig;
    memset(&publishedDataSetConfig, 0, sizeof(UA_PublishedDataSetConfig));
    publishedDataSetConfig.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    publishedDataSetConfig.name = UA_STRING("Demo PDS");

    UA_Server_addPublishedDataSet(server, &publishedDataSetConfig, &publishedDataSetIdent);
}

static void
addPubSubConnection(UA_Server *server, UA_String *transportProfile,
                    UA_NetworkAddressUrlDataType *networkAddressUrl) {
    UA_PubSubConnectionConfig connectionConfig;
    memset(&connectionConfig, 0, sizeof(connectionConfig));
    connectionConfig.name = UA_STRING("UADP Connection 1");
    connectionConfig.transportProfileUri = *transportProfile;
    connectionConfig.enabled = UA_TRUE;

    // custom properties
    connectionConfig.soTxTimeEnable = UA_TRUE;
    connectionConfig.soTxTimeEnbableDeadlineMode = deadlineMode;
    connectionConfig.soTxTimeReceiveErrors = receiveErrors;
    connectionConfig.soTxTimePriority = DEFAULT_PRIORITY;
    connectionConfig.soTxTimeClockId = CLOCK_TAI;

    UA_Variant_setScalar(&connectionConfig.address, networkAddressUrl,
                         &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    connectionConfig.publisherId.numeric = UA_UInt32_random();

    UA_Server_addPubSubConnection(server, &connectionConfig, &connectionIdent);
}

static void
addWriterGroup(UA_Server *server) {
    UA_WriterGroupConfig writerGroupConfig;
    memset(&writerGroupConfig, 0, sizeof(UA_WriterGroupConfig));
    writerGroupConfig.name = UA_STRING("Demo WriterGroup");
    writerGroupConfig.publishingInterval = (UA_Duration) pubInterval;
    writerGroupConfig.enabled = UA_FALSE;
    writerGroupConfig.writerGroupId = 100;
    writerGroupConfig.encodingMimeType = UA_PUBSUB_ENCODING_UADP;

    // custom
    writerGroupConfig.enableRealTime = UA_TRUE;

     UA_UadpWriterGroupMessageDataType *wgm = UA_UadpWriterGroupMessageDataType_new();
    UA_UadpNetworkMessageContentMask mask =  (UA_UadpNetworkMessageContentMask) UA_UADPNETWORKMESSAGECONTENTMASK_TIMESTAMP |
    (UA_UadpNetworkMessageContentMask) UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID;
    wgm->networkMessageContentMask = mask;
    writerGroupConfig.messageSettings.content.decoded.data = wgm;
    writerGroupConfig.messageSettings.content.decoded.type =
            &UA_TYPES[UA_TYPES_UADPWRITERGROUPMESSAGEDATATYPE];
    writerGroupConfig.messageSettings.encoding = UA_EXTENSIONOBJECT_DECODED;


    UA_Server_addWriterGroup(server, connectionIdent, &writerGroupConfig, &writerGroupIdent);
}

void
normalizeTimeSpec(struct timespec *timespec) {
    while(timespec->tv_nsec > 999999999){
        timespec->tv_sec += 1;
        timespec->tv_nsec -= ONE_SEC;
    }

    while(timespec->tv_nsec < 0){
        timespec->tv_sec -= 1;
        timespec->tv_nsec += ONE_SEC;
    }
}

static unsigned char tx_buffer[256];
static UA_UInt64 msgDropCounterMiss = 0;
static UA_UInt64 msgDropCounterParam = 0;

#define pr_err(s)	fprintf(stderr, s "\n")
#define pr_info(s)	fprintf(stdout, s "\n")

static int 
process_socket_error_queue(int fd)
{
	uint8_t msg_control[CMSG_SPACE(sizeof(struct sock_extended_err))];
	unsigned char err_buffer[sizeof(tx_buffer)];
	struct sock_extended_err *serr;
	struct cmsghdr *cmsg;
//	__u64 tstamp = 0;

	struct iovec iov = {
	        .iov_base = err_buffer,
	        .iov_len = sizeof(err_buffer)
	};
	struct msghdr msg = {
	        .msg_iov = &iov,
	        .msg_iovlen = 1,
	        .msg_control = msg_control,
	        .msg_controllen = sizeof(msg_control)
	};

	if (recvmsg(fd, &msg, MSG_ERRQUEUE) == -1) {
		pr_err("recvmsg failed");
	        return -1;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	while (cmsg != NULL) {
		serr = (struct sock_extended_err *) CMSG_DATA(cmsg);
		if (serr->ee_origin == SO_EE_ORIGIN_TXTIME) {
			// tstamp = ((__u64) serr->ee_data << 32) + serr->ee_info;

			switch(serr->ee_code) {
			case SO_EE_CODE_TXTIME_INVALID_PARAM:
				// UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "packet with tstamp %llu dropped due to invalid params\n", tstamp);
				msgDropCounterParam++;
				return 1;
			case SO_EE_CODE_TXTIME_MISSED:
				// UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "packet with tstamp %llu dropped due to missed deadline\n", tstamp);
				msgDropCounterMiss++;
                return 2;
            default:
                return -1;
			}
		}

		cmsg = CMSG_NXTHDR(&msg, cmsg);
	}

	return 0;
}

void
*publisherTBS(void *server) {

    cpu_set_t cpuSet;

    pthread_t publisherThreadId = pthread_self();

    struct sched_param schedMaxPriority;
    schedMaxPriority.sched_priority = 99; //sched_get_priority_max(SCHED_FIFO);

    int retval = pthread_setschedparam(publisherThreadId, SCHED_FIFO, &schedMaxPriority);
    if(retval != 0){
        printf("pthread_setschedparam failed!\n");
    }

    CPU_ZERO(&cpuSet);
    CPU_SET(cpuNumber, &cpuSet);

    retval = pthread_setaffinity_np(publisherThreadId, sizeof(cpu_set_t), &cpuSet);
    if(retval != 0){
        printf("pthread_setaffinity_np failed!\n");
        return NULL;
    }

    messageTraces = (MessageTrace *) malloc(maxMessageNumber * sizeof(MessageTrace));

    UA_UInt64 msgCounter = 0;

    UA_Server *uaServer = (UA_Server *) server;
    UA_Variant variant;
    UA_Variant_init(&variant);
    UA_Variant_setScalar(&variant, &msgCounter, &UA_TYPES[UA_TYPES_UINT64]);
    UA_Server_writeValue(uaServer, counterNodeId, variant);

    struct timespec nextNanoSleepTime;

    if (base_time == 0) 
    {
	    clock_gettime(CLOCK_TAI, &nextNanoSleepTime);
    	    nextNanoSleepTime.tv_sec += 1;
	    nextNanoSleepTime.tv_nsec = (__syscall_slong_t) (ONE_SEC - waketx_delay);
    } 
    else 
    {
	    nextNanoSleepTime.tv_sec = (__time_t) (base_time / ONE_SEC);
	    nextNanoSleepTime.tv_nsec = (__syscall_slong_t)  
		    ((base_time % ONE_SEC) - waketx_delay);
    }

    normalizeTimeSpec(&nextNanoSleepTime);
    
    pubSubConnection->channel->txTimestamp = (UA_UInt64) 
	    (nextNanoSleepTime.tv_sec * ONE_SEC + nextNanoSleepTime.tv_nsec);
    pubSubConnection->channel->txTimestamp += (UA_UInt64) waketx_delay;

    struct pollfd p_fd = {
	    .fd = pubSubConnection->channel->sockfd,
    };

    struct timespec currentTime;
    UA_UInt64 cTime;
    UA_UInt64 earlierCounter = 0, laterCounter = 0;
   
    clock_gettime(CLOCK_TAI, &currentTime);
    cTime = (UA_UInt64) (currentTime.tv_sec * ONE_SEC + currentTime.tv_nsec);

    printf("publisher: current time is\t %ld\n", cTime);
    printf("publisher: txtime 1st msg is\t %ld\n", pubSubConnection->channel->txTimestamp);

    while(running && msgCounter < maxMessageNumber)
    {
        clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &nextNanoSleepTime, NULL);
	
       	if(writerGroupRT->config.enableRealTime)
       	{
       	    writerGroupRT->config.pubCallback(uaServer, writerGroupRT->config.pubData);
	}

	clock_gettime(CLOCK_TAI, &currentTime);
	cTime = (UA_UInt64) (currentTime.tv_sec * ONE_SEC + currentTime.tv_nsec);	
	
	messageTraces[msgCounter].sendTime = cTime;
	messageTraces[msgCounter].msgNumber = msgCounter;
	messageTraces[msgCounter].wakeupTime = (uint64_t) 
		(nextNanoSleepTime.tv_sec * ONE_SEC + nextNanoSleepTime.tv_nsec);	
	messageTraces[msgCounter].txTime = pubSubConnection->channel->txTimestamp;
	messageTraces[msgCounter].missDeadline = 0;
		
	int err = poll(&p_fd, 1, 0);
	if (err == 1 && p_fd.revents & POLLERR) 
	{
		if (process_socket_error_queue(pubSubConnection->channel->sockfd) > 0) 
		{
			if (cTime > pubSubConnection->channel->txTimestamp) 
			{
				laterCounter++;
			}
		       	else
		       	{
		 		earlierCounter++;
			}
			messageTraces[msgCounter].missDeadline = 1; 
		}
	}
       
	msgCounter++;
	UA_Variant_setScalar(&variant, &msgCounter, &UA_TYPES[UA_TYPES_UINT64]);
	UA_Server_writeValue(uaServer, counterNodeId, variant);

        nextNanoSleepTime.tv_nsec += (__syscall_slong_t) writerGroupRT->config.publishingInterval;
        normalizeTimeSpec(&nextNanoSleepTime);  
        pubSubConnection->channel->txTimestamp += (UA_UInt64) writerGroupRT->config.publishingInterval;
    }
 
    printf("publisher: stops: %ld/%ld tot drops miss...\n", msgDropCounterMiss, msgCounter);
 
    printf("publisher: stops: %ld/%ld tot drops param...\n", msgDropCounterParam, msgCounter);

    printf("publisher: stops: %ld/%ld tot drops early...\n", earlierCounter, msgCounter);

    printf("publisher: stops: %ld/%ld tot drops late...\n", laterCounter, msgCounter);

    printf("publisher: save in file!\n");

    fp = fopen(fileName, "w");

    fprintf(fp, "wakeupTime,txTime,sendTime,msgNumber,missDeadline\n");

    uint64_t i;
    for (i = 0; i < maxMessageNumber; i++) 
    {
    	fprintf(fp, "%ld,%ld,%ld,%ld,%d\n",
		    messageTraces[i].wakeupTime,
		    messageTraces[i].txTime,
		    messageTraces[i].sendTime,
		    messageTraces[i].msgNumber,
		    messageTraces[i].missDeadline);
    }

    free(messageTraces);

    fclose(fp);

    printf("publisher: exit...\n");

    return NULL;
}

static int
run(UA_String *transportProfile, UA_NetworkAddressUrlDataType *networkAddressUrl) {
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);

    UA_ServerConfig *config = UA_ServerConfig_new_default();

    config->pubsubTransportLayers = (UA_PubSubTransportLayer *) UA_calloc(1, sizeof(UA_PubSubTransportLayer));
    if(!config->pubsubTransportLayers){
        UA_ServerConfig_delete(config);
        return EXIT_FAILURE;
    }

    const char *url = (char *) networkAddressUrl->url.data;
    if(strncmp(url, "opc.udp://", 10) == 0){
        config->pubsubTransportLayers[0] = UA_PubSubTransportLayerUDPTSN();
        config->pubsubTransportLayersSize++;
    } else if(strncmp(url, "opc.eth://", 10) == 0){
        config->pubsubTransportLayers[0] = UA_PubSubTransportLayerEthernetTSN();
        config->pubsubTransportLayersSize++;
    }


    UA_Server *server = UA_Server_new(config);

    int num = 0;
    addVariableScalar(server, &num, UA_TYPES_UINT64, "counter");

    addPubSubConnection(server, transportProfile, networkAddressUrl);
    addPublishedDataSet(server);
    addDataSetField(server);
    addWriterGroup(server);
    addDataSetWriter(server);

    writerGroupRT = UA_WriterGroup_findWGbyId(server, writerGroupIdent);

    pubSubConnection = UA_PubSubConnection_findConnectionbyId(server, writerGroupRT->linkedConnection);

    pthread_t publisherThreadId;

    printf("launching publisher thread...\n");

    int retval = pthread_create(&publisherThreadId, NULL, &publisherTBS, (void *) server);

    UA_StatusCode serverRetval = UA_STATUSCODE_GOOD;
    serverRetval |= UA_Server_run(server, &running);

    UA_Server_delete(server);
    UA_ServerConfig_delete(config);

    return retval == UA_STATUSCODE_GOOD ? EXIT_SUCCESS : EXIT_FAILURE;
}

