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

#include <ua_server_pubsub.h>
#include <ua_config_default.h>
#include <ua_log_stdout.h>
#include <ua_network_pubsub_udp.h>
#include <ua_pubsub_manager.h>
#include <asm/types.h>
#include <poll.h>
#include <ua_pubsub_networkmessage.h>

#define ONE_SEC 1000 * 1000 * 1000

UA_Server *pubServer;
UA_ServerCallback pubCallback;
void *pubData;
UA_Double pubIntervalUs;

__u64 nextCycleStartTime;

UA_WriterGroupConfig writerGroupConfig;

void *publisherTBS(void *args);

void normalizeTimeSpec(struct timespec *timespec);

UA_Boolean running = true;

static void stopHandler(int sign)
{
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "received ctrl-c");
    running = false;
}

UA_StatusCode
UA_PubSubManager_addRepeatedCallback(UA_Server *server, UA_ServerCallback callback,
                                     void *data, UA_Double interval_us, UA_UInt64 *callbackId)
{
    pubServer = server;
    pubCallback = callback;
    pubData = data;
    pubIntervalUs = interval_us;

    return UA_STATUSCODE_GOOD;
}

UA_NodeId connectionIdent, publishedDataSetIdent, writerGroupIdent;

static
void add_variable_scalar(UA_Server *server, void *value, int ua_types_id, char *name)
{
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Variant_setScalar(&attr.value, value, &UA_TYPES[ua_types_id]);

    attr.description = UA_LOCALIZEDTEXT("en-US", name);
    attr.displayName = UA_LOCALIZEDTEXT("en-US", name);
    attr.dataType = UA_TYPES[ua_types_id].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

    UA_NodeId integer_node_id = UA_NODEID_STRING(1, name);
    UA_QualifiedName integer_name = UA_QUALIFIEDNAME(1, name);

    UA_NodeId parent_node_id = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parent_reference_node_id = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);

    UA_Server_addVariableNode(server, integer_node_id, parent_node_id,
                              parent_reference_node_id, integer_name,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                              attr, NULL, NULL);
}

static void
addPubSubConnection(UA_Server *server, UA_String *transportProfile,
                    UA_NetworkAddressUrlDataType *networkAddressUrl)
{
    /* Details about the connection configuration and handling are located
     * in the pubsub connection tutorial */
    UA_PubSubConnectionConfig connectionConfig;
    memset(&connectionConfig, 0, sizeof(connectionConfig));
    connectionConfig.name = UA_STRING("UADP Connection 1");
    connectionConfig.transportProfileUri = *transportProfile;
    connectionConfig.enabled = UA_TRUE;

    // custom properties
    connectionConfig.useSoTxTime = UA_TRUE;
    connectionConfig.useDeadlineMode = 0;
    connectionConfig.receiveErrors = 0;
    connectionConfig.soPriority = 3;

    UA_Variant_setScalar(&connectionConfig.address, networkAddressUrl,
                         &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    connectionConfig.publisherId.numeric = UA_UInt32_random();
    UA_Server_addPubSubConnection(server, &connectionConfig, &connectionIdent);
}

/**
 * **PublishedDataSet handling**
 *
 * The PublishedDataSet (PDS) and PubSubConnection are the toplevel entities and
 * can exist alone. The PDS contains the collection of the published fields. All
 * other PubSub elements are directly or indirectly linked with the PDS or
 * connection. */
static void
addPublishedDataSet(UA_Server *server)
{
    /* The PublishedDataSetConfig contains all necessary public
    * informations for the creation of a new PublishedDataSet */
    UA_PublishedDataSetConfig publishedDataSetConfig;
    memset(&publishedDataSetConfig, 0, sizeof(UA_PublishedDataSetConfig));
    publishedDataSetConfig.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    publishedDataSetConfig.name = UA_STRING("Demo PDS");
    /* Create new PublishedDataSet based on the PublishedDataSetConfig. */
    UA_Server_addPublishedDataSet(server, &publishedDataSetConfig, &publishedDataSetIdent);
}

/**
 * **DataSetField handling**
 *
 * The DataSetField (DSF) is part of the PDS and describes exactly one published
 * field. */
static void
addDataSetField(UA_Server *server)
{
    /* Add a field to the previous created PublishedDataSet */
    UA_NodeId dataSetFieldIdent;
    UA_DataSetFieldConfig dataSetFieldConfig;
    memset(&dataSetFieldConfig, 0, sizeof(UA_DataSetFieldConfig));
    dataSetFieldConfig.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
    dataSetFieldConfig.field.variable.fieldNameAlias = UA_STRING("Server localtime");
    dataSetFieldConfig.field.variable.promotedField = UA_FALSE;
    dataSetFieldConfig.field.variable.publishParameters.publishedVariable =
//            UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERSTATUS_CURRENTTIME);
            UA_NODEID_STRING(1, "the answer");
    dataSetFieldConfig.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;
    UA_Server_addDataSetField(server, publishedDataSetIdent,
                              &dataSetFieldConfig, &dataSetFieldIdent);
}

/**
 * **WriterGroup handling**
 *
 * The WriterGroup (WG) is part of the connection and contains the primary
 * configuration parameters for the message creation. */
static void
addWriterGroup(UA_Server *server)
{
    /* Now we create a new WriterGroupConfig and add the group to the existing
     * PubSubConnection. */
    memset(&writerGroupConfig, 0, sizeof(UA_WriterGroupConfig));
    writerGroupConfig.name = UA_STRING("Demo WriterGroup");
    writerGroupConfig.publishingInterval = 200;
    writerGroupConfig.enabled = UA_FALSE;
    writerGroupConfig.writerGroupId = 100;
    writerGroupConfig.encodingMimeType = UA_PUBSUB_ENCODING_UADP;
    writerGroupConfig.enableRealTime = UA_TRUE;

    /* The configuration flags for the messages are encapsulated inside the
     * message- and transport settings extension objects. These extension
     * objects are defined by the standard. e.g.
     * UadpWriterGroupMessageDataType */
    UA_Server_addWriterGroup(server, connectionIdent, &writerGroupConfig, &writerGroupIdent);
}

/**
 * **DataSetWriter handling**
 *
 * A DataSetWriter (DSW) is the glue between the WG and the PDS. The DSW is
 * linked to exactly one PDS and contains additional informations for the
 * message generation. */
static void
addDataSetWriter(UA_Server *server)
{
    /* We need now a DataSetWriter within the WriterGroup. This means we must
     * create a new DataSetWriterConfig and add call the addWriterGroup function. */
    UA_NodeId dataSetWriterIdent;
    UA_DataSetWriterConfig dataSetWriterConfig;
    memset(&dataSetWriterConfig, 0, sizeof(UA_DataSetWriterConfig));
    dataSetWriterConfig.name = UA_STRING("Demo DataSetWriter");
    dataSetWriterConfig.dataSetWriterId = 62541;
    dataSetWriterConfig.keyFrameCount = 10;
    UA_Server_addDataSetWriter(server, writerGroupIdent, publishedDataSetIdent,
                               &dataSetWriterConfig, &dataSetWriterIdent);
}

static int run(UA_String *transportProfile,
               UA_NetworkAddressUrlDataType *networkAddressUrl)
{
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);

    UA_ServerConfig *config = UA_ServerConfig_new_default();
    /* Details about the connection configuration and handling are located in
     * the pubsub connection tutorial */
    config->pubsubTransportLayers =
            (UA_PubSubTransportLayer *) UA_calloc(1, sizeof(UA_PubSubTransportLayer));
    if (!config->pubsubTransportLayers)
    {
        UA_ServerConfig_delete(config);
        return EXIT_FAILURE;
    }
    config->pubsubTransportLayers[0] = UA_PubSubTransportLayerUDPMP();
    config->pubsubTransportLayersSize++;

    //#ifdef UA_ENABLE_PUBSUB_ETH_UADP
//    config->pubsubTransportLayers[1] = UA_PubSubTransportLayerEthernet();
//    config->pubsubTransportLayersSize++;
//#endif

    UA_Server *server = UA_Server_new(config);

    int num = 42;
    add_variable_scalar(server, &num, UA_TYPES_UINT32, "the answer");

    addPubSubConnection(server, transportProfile, networkAddressUrl);
    addPublishedDataSet(server);
    addDataSetField(server);
    addWriterGroup(server);
    addDataSetWriter(server);

    /**************************************/
    /* Imposto il thread per il publisher */
    /**************************************/

    // Structure representing CPU
    cpu_set_t cpuSet;

    pthread_t publisherThreadId = pthread_self();

    struct sched_param schedMaxPriority;
    schedMaxPriority.sched_priority = sched_get_priority_max(SCHED_FIFO);

    int retval = pthread_setschedparam(publisherThreadId, SCHED_FIFO, &schedMaxPriority);
    if (retval != 0)
    {
        printf("pthread_setschedparam failed!");
    }

    CPU_ZERO(&cpuSet);
    CPU_SET(2, &cpuSet);

    retval = pthread_setaffinity_np(publisherThreadId, sizeof(cpu_set_t), &cpuSet);
    if (retval != 0)
    {
        printf("pthread_setaffinity_np failed!");
        return EXIT_FAILURE;
    }

    retval = pthread_create(&publisherThreadId, NULL, &publisherTBS, NULL);

    UA_StatusCode serverRetval = UA_STATUSCODE_GOOD;
    serverRetval |= UA_Server_run(server, &running);
    UA_Server_delete(server);
    UA_ServerConfig_delete(config);

    return retval == UA_STATUSCODE_GOOD ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void
usage(char *progname)
{
    printf("usage: %s <uri> [device]\n", progname);
}


int main(int argc, char *argv[])
{
    UA_String transportProfile =
            UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-udp-uadp");
    UA_NetworkAddressUrlDataType networkAddressUrl =
            { UA_STRING_NULL, UA_STRING("opc.udp://224.0.0.22:4840/") };

    if (argc > 1)
    {
        if (strcmp(argv[1], "-h") == 0)
        {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }
        else if (strncmp(argv[1], "opc.udp://", 10) == 0)
        {
            networkAddressUrl.url = UA_STRING(argv[1]);
            networkAddressUrl.networkInterface = UA_STRING("10.1.1.52");
        }
        else if (strncmp(argv[1], "opc.eth://", 10) == 0)
        {
            transportProfile =
                    UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-eth-uadp");
            if (argc < 3)
            {
                printf("Error: UADP/ETH needs an interface name\n");
                return EXIT_FAILURE;
            }
            networkAddressUrl.networkInterface = UA_STRING(argv[2]);
            networkAddressUrl.url = UA_STRING(argv[1]);
        }
        else
        {
            printf("Error: unknown URI\n");
            return EXIT_FAILURE;
        }
    }

    return run(&transportProfile, &networkAddressUrl);
}

void *publisherTBS(void *args)
{
    struct timespec timespec;
    int err;

    /*
     * If flags is TIMER_ABSTIME, then request is interpreted as an absolute
     * time as measured by the clock, clock_id.  If request is less than or
     * equal to the current value of the clock, then clock_nanosleep()
     * returns immediately without suspending the calling thread.
     *
     * clock_nanosleep() suspends the execution of the calling thread until
     * either at least the time specified by request has elapsed, or a sig‐
     * nal is delivered that causes a signal handler to be called or that
     * terminates the process.
     */

    clock_gettime(CLOCK_REALTIME, &timespec);
    timespec.tv_sec += 1;
    timespec.tv_nsec = ONE_SEC - 500000;

    normalizeTimeSpec(&timespec);

    nextCycleStartTime = (__u64) (timespec.tv_sec * ONE_SEC + timespec.tv_nsec);
    nextCycleStartTime += 500000;

    fprintf(stderr, "txtime of 1st packet is: %llu\n", nextCycleStartTime);

    while (running)
    {
        err = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &timespec, NULL);

        switch (err)
        {
            case 0:
                if (writerGroupConfig.enableRealTime)
                {

                    UA_PubSUb_sendNetworkMessage(pubServer, writerGroupIdent, &writerGroupConfig);
                }
                else
                {
                    pubCallback(pubServer, pubData);

                }

                timespec.tv_nsec += (long int) pubIntervalUs * 1000;
                normalizeTimeSpec(&timespec);
                nextCycleStartTime += (__u64) pubIntervalUs * 1000;

                fprintf(stderr, "txtime of 1st packet is: %lld\n", nextCycleStartTime / 1000);

                break;

            case EINTR:
                continue;

            default:
                fprintf(stderr, "clock_nanosleep returned %d: %s\n", err, strerror(err));
                break;
        }
    }

    return NULL;
}

void normalizeTimeSpec(struct timespec *timespec)
{
    while (timespec->tv_nsec > 999999999)
    {
        timespec->tv_sec += 1;
        timespec->tv_nsec -= ONE_SEC;
    }

    while (timespec->tv_nsec < 0)
    {
        timespec->tv_sec -= 1;
        timespec->tv_nsec += ONE_SEC;
    }
}