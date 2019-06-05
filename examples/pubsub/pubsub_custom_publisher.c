// queste define servono per alcune cose
// unix, tipo i segnali
#if !defined(_XOPEN_SOURCE) && !defined(_WRS_KERNEL)
# define _XOPEN_SOURCE 600
#endif

#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE
#endif

// questa define server
// per avere CPU_ZERO ecc
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <asm/types.h>
#include <poll.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/plugin/pubsub_udp_tsn.h>
#include <open62541/server_pubsub.h>
#include <open62541/types.h>
#include <open62541/plugin/pubsub.h>
#include <ua_pubsub.h>
#include <open62541/plugin/pubsub_ethernet_tsn.h>

/* PUBLISHING_OFFSET is the internal target to send out the packets
*  When used with SOTXTIME, packet WILL get transmitted exactly at this
*  point of time. Without SOTXTIME, this is merely the target and
*  transmits roughly at that time.
*/
#define PUBLISHING_OFFSET 50000   //in nanoseconds

#define ONE_SEC 1000 * 1000 * 1000

UA_WriterGroup *writerGroupRT;
UA_PubSubConnection *pubSubConnection;

UA_Boolean running = UA_TRUE;

UA_NodeId connectionIdent;
UA_NodeId publishedDataSetIdent;
UA_NodeId writerGroupIdent;

UA_Duration pubInterval;

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

    if(argc > 1){
        if(strncmp(argv[1], "opc.udp://", 10) == 0){
            networkAddressUrl.url = UA_STRING(argv[1]);
            networkAddressUrl.networkInterface = UA_STRING("127.0.0.1");

            if(strcmp(argv[2], "-p") == 0){
                pubInterval = atof(argv[3]);
            }else
            {
                pubInterval = 100000;
            }

        }else if(strncmp(argv[1], "opc.eth://", 10) == 0){
            transportProfile =
                    UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-eth-uadp");
            if(argc < 3){
                printf("Error: UADP/ETH needs an interface name\n");
                return EXIT_FAILURE;
            }

            if(strcmp(argv[3], "-p") == 0){
                pubInterval = atof(argv[4]);
            }else
            {
                pubInterval = 100000;
            }

            networkAddressUrl.networkInterface = UA_STRING(argv[2]);
            networkAddressUrl.url = UA_STRING(argv[1]);
        }else{
            printf("Error: unknown URI\n");
            return EXIT_FAILURE;
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

    UA_NodeId integerNodeId = UA_NODEID_STRING(1, name);
    UA_QualifiedName integerName = UA_QUALIFIEDNAME(1, name);

    UA_NodeId parentNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentReferenceNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);

    UA_Server_addVariableNode(server, integerNodeId, parentNodeId,
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
    dataSetFieldConfig.field.variable.fieldNameAlias = UA_STRING("Server localtime");
    dataSetFieldConfig.field.variable.promotedField = UA_FALSE;
//    dataSetFieldConfig.field.variable.publishParameters.publishedVariable = UA_NODEID_STRING(1, "the answer");
//    dataSetFieldConfig.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;

    dataSetFieldConfig.field.variable.publishParameters.publishedVariable =
            UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERSTATUS_CURRENTTIME);
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
    connectionConfig.soTxTimeEnbableDeadlineMode = 0;
    connectionConfig.soTxTimeReceiveErrors = 0;
    connectionConfig.soTxTimePriority = 3;
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
    writerGroupConfig.publishingInterval = pubInterval;
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

void
*publisherTBS(void *server) {

    cpu_set_t cpuSet;

    pthread_t publisherThreadId = pthread_self();

    struct sched_param schedMaxPriority;
    schedMaxPriority.sched_priority = sched_get_priority_max(SCHED_FIFO);

    int retval = pthread_setschedparam(publisherThreadId, SCHED_FIFO, &schedMaxPriority);
    if(retval != 0){
        printf("pthread_setschedparam failed!");
    }

    CPU_ZERO(&cpuSet);
    CPU_SET(2, &cpuSet);

    retval = pthread_setaffinity_np(publisherThreadId, sizeof(cpu_set_t), &cpuSet);
    if(retval != 0){
        printf("pthread_setaffinity_np failed!");
        return NULL;
    }

    UA_Server *uaServer = (UA_Server *) server;

    struct timespec timespec;

    clock_gettime(CLOCK_REALTIME, &timespec);
    timespec.tv_sec += 1;
    timespec.tv_nsec = ONE_SEC - (time_t) (writerGroupRT->config.publishingInterval * 1000);

    normalizeTimeSpec(&timespec);

    pubSubConnection->channel->txTimestamp = (UA_UInt64) (timespec.tv_sec * ONE_SEC + timespec.tv_nsec);
    pubSubConnection->channel->txTimestamp += (UA_UInt64) writerGroupRT->config.publishingInterval * 1000;

    while(running){
        clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &timespec, NULL);

        timespec.tv_nsec += (long int) writerGroupRT->config.publishingInterval * 1000;
        normalizeTimeSpec(&timespec);

        if(writerGroupRT->config.enableRealTime){
            writerGroupRT->config.pubCallback(uaServer, writerGroupRT->config.pubData);
        }

        pubSubConnection->channel->txTimestamp += (UA_UInt64) writerGroupRT->config.publishingInterval * 1000;
    }

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

    int num = 42;
    addVariableScalar(server, &num, UA_TYPES_UINT32, "the answer");

    addPubSubConnection(server, transportProfile, networkAddressUrl);
    addPublishedDataSet(server);
    addDataSetField(server);
    addWriterGroup(server);
    addDataSetWriter(server);

    writerGroupRT = UA_WriterGroup_findWGbyId(server, writerGroupIdent);

    pubSubConnection = UA_PubSubConnection_findConnectionbyId(server, writerGroupRT->linkedConnection);

    pthread_t publisherThreadId;

    int retval = pthread_create(&publisherThreadId, NULL, &publisherTBS, (void *) server);

    UA_StatusCode serverRetval = UA_STATUSCODE_GOOD;
    serverRetval |= UA_Server_run(server, &running);

    UA_Server_delete(server);
    UA_ServerConfig_delete(config);

    return retval == UA_STATUSCODE_GOOD ? EXIT_SUCCESS : EXIT_FAILURE;
}

