#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/signal.h>
#include <fcntl.h>
#include <printf.h>
#include <signal.h>
#include <getopt.h>
#include <stdlib.h>

#include "gfserver.h"
#include "cache-student.h"

/* note that the -n and -z parameters are NOT used for Part 1 */
/* they are only used for Part 2 */
#define USAGE                                                                         \
"usage:\n"                                                                            \
"  webproxy [options]\n"                                                              \
"options:\n"                                                                          \
"  -n [segment_count]  Number of segments to use (Default: 7)\n"                      \
"  -p [listen_port]    Listen port (Default: 10823)\n"                                 \
"  -t [thread_count]   Num worker threads (Default: 34, Range: 1-420)\n"              \
"  -s [server]         The server to connect to (Default: GitHub test data)\n"     \
"  -z [segment_size]   The segment size (in bytes, Default: 5701).\n"                  \
"  -h                  Show this help message\n"



/* OPTIONS DESCRIPTOR ====================================================== */
static struct option gLongOptions[] = {
        {"server",        required_argument,      NULL,           's'},
        {"segment-count", required_argument,      NULL,           'n'},
        {"thread-count",  required_argument,      NULL,           't'},
        {"listen-port",   required_argument,      NULL,           'p'},
        {"segment-size",  required_argument,      NULL,           'z'},
        {"help",          no_argument,            NULL,           'h'},
        {"hidden",        no_argument,            NULL,           'i'}, /* server side */
        {NULL,            0,                      NULL,            0}
};

extern ssize_t handle_with_cache(gfcontext_t *ctx, char *path, void* arg);

static gfserver_t gfs;

static void _sig_handler(int signo){
    if (signo == SIGINT || signo == SIGTERM)
    {
        gfserver_stop(&gfs);
        printf("Cleaning shared memories \n");

        //Cleanup
        if (!proxy_lock){
            if (pthread_cond_broadcast(&proxy_lock->cond) != 0){
                printf("Function %s Line: %d Broadcast Failed with Error %d ! \n", __FUNCTION__, __LINE__, errno);
            }
        }

        for(int i =0 ; i <= g_webProxy.nSegments; i++)
        {
            char shmName[MAX_SHMNAME_LEN];
            sprintf(shmName, "%s%d", SHM_NAME, i);
            fprintf(stdout, "Closing SHM %s \n", shmName);
            shm_unlink(shmName);
        }

        fprintf(stdout, "Close MQ %d with name %s \n", g_webProxy.mqRequest, MQ_REQUEST_NAME);

        mq_close(g_webProxy.mqRequest);
        mq_unlink(MQ_REQUEST_NAME);

        pthread_mutex_destroy(&proxy_lock->mutex);
        pthread_cond_destroy(&proxy_lock->cond);

        int i = 1;
        while(!steque_isempty(proxy_queue)){
            ContextShm_t *ctxShm = steque_pop(proxy_queue);
            if(ctxShm){
                fprintf(stdout, "Successfully free queue item %d \n", i);
                free(ctxShm);
            }
            i++;
        }

        if(proxy_queue)
            steque_destroy(proxy_queue);
        if (cache_queue)
            steque_destroy(cache_queue);
        if (proxy_lock)
            free(proxy_lock);
    }
    exit(signo);
}

/* Main ========================================================= */
int main(int argc, char **argv) {

    int option_char = 0;
    char *server = "https://raw.githubusercontent.com/gt-cs6200/image_data";
    unsigned int nsegments = 7;
    unsigned short port = 10823;
    unsigned short nworkerthreads = 34;
    size_t segsize = 5701;

    /* disable buffering on stdout so it prints immediately */
    setbuf(stdout, NULL);

    if (signal(SIGTERM, _sig_handler) == SIG_ERR) {
        fprintf(stderr,"Can't catch SIGTERM...exiting.\n");
        exit(SERVER_FAILURE);
    }

    if (signal(SIGINT, _sig_handler) == SIG_ERR) {
        fprintf(stderr,"Can't catch SIGINT...exiting.\n");
        exit(SERVER_FAILURE);
    }

    /* Parse and set command line arguments */
    while ((option_char = getopt_long(argc, argv, "s:qt:hn:xp:z:l", gLongOptions, NULL)) != -1) {
        switch (option_char) {
            default:
                fprintf(stderr, "%s", USAGE);
                exit(__LINE__);
            case 'h': // help
                fprintf(stdout, "%s", USAGE);
                exit(0);
                break;
            case 'p': // listen-port
                port = atoi(optarg);
                break;
            case 's': // file-path
                server = optarg;
                break;
            case 'n': // segment count
                nsegments = atoi(optarg);
                break;
            case 'z': // segment size
                segsize = atoi(optarg);
                break;
            case 't': // thread-count
                nworkerthreads = atoi(optarg);
                break;
            case 'i':
            case 'y':
            case 'k':
                break;
        }
    }

    if (segsize < 313) {
        fprintf(stderr, "Invalid segment size\n");
        exit(__LINE__);
    }

    if (server == NULL) {
        fprintf(stderr, "Invalid (null) server name\n");
        exit(__LINE__);
    }

    if (port > 50240) {
        fprintf(stderr, "Invalid port number\n");
        exit(__LINE__);
    }

    if (nsegments < 1) {
        fprintf(stderr, "Must have a positive number of segments\n");
        exit(__LINE__);
    }

    if ((nworkerthreads < 1) || (nworkerthreads > 420)) {
        fprintf(stderr, "Invalid number of worker threads\n");
        exit(__LINE__);
    }

    // Initialize shared memory set-up here
    g_webProxy.nSegments = nsegments;
    g_webProxy.segmentSize = segsize;

    proxy_queue = (steque_t*) malloc(sizeof(steque_t));
    steque_init(proxy_queue);

    //Create global mutex
    proxy_lock = (lock_t*) malloc(sizeof(lock_t));
    pthread_cond_init(&proxy_lock->cond, NULL);
    pthread_mutex_init(&proxy_lock->mutex, NULL);

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_MSG_NUM;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;


    int fdesc;
    for(int i = 0; i < nsegments; i++) {
        ContextProxy_t *proxy_req = (ContextProxy_t*) malloc (sizeof(ContextProxy_t));
        char shmName[MAX_SHMNAME_LEN];
        sprintf(shmName, "%s%d", SHM_NAME, i);

        if ((fdesc = shm_open(shmName, O_CREAT | O_RDWR, 0600)) < 0){
            fprintf(stderr, "error: Failed shm_open for %s \n", shmName);
        }

        ftruncate(fdesc, segsize);

        void* addr = mmap(NULL, segsize, PROT_READ | PROT_WRITE, MAP_SHARED, fdesc, 0);
        if (addr == MAP_FAILED){
            fprintf(stderr, "ERROR: mmap failed for %d \n", i);
        }

        proxy_req->shm_context = (ContextShm_t*) addr;

        //register SHM Details for cache
        ((ContextShm_t*) addr)->dataLen = 0;
        memcpy(proxy_req->shm_name, shmName, sizeof(shmName));
        bzero(((ContextShm_t*) addr)->filePath, MAX_PATH_LEN);
        ((ContextShm_t*) addr)->fileLen = 0;
        sem_init(&((ContextShm_t*) addr)->semREAD, 1, 0); //read
        sem_init(&((ContextShm_t*) addr)->semWRITE, 1, 1); //write

        if (proxy_queue){
            pthread_mutex_lock(&proxy_lock->mutex);
            steque_enqueue(proxy_queue, proxy_req);
            pthread_mutex_unlock(&proxy_lock->mutex);
        }
    }

    // Create Message Queue Request and Response (must after the malloc above, otherwise memory leakage)
    if((g_webProxy.mqRequest = mq_open(MQ_REQUEST_NAME, O_RDWR | O_CREAT , 0666, &attr)) < 0){
        printf("Error: mq_open %s failed errcode %s\n", MQ_REQUEST_NAME, strerror(errno));
        exit(SERVER_FAILURE);
    }

    // Initialize server structure here
    gfserver_init(&gfs, nworkerthreads);

    // Set server options here
    gfserver_setopt(&gfs, GFS_PORT, port);
    gfserver_setopt(&gfs, GFS_WORKER_FUNC, handle_with_cache);
    gfserver_setopt(&gfs, GFS_MAXNPENDING, 314);

    // Set up arguments for worker here
    for(int i = 0; i < nworkerthreads; i++) {
        gfserver_setopt(&gfs, GFS_WORKER_ARG, i, &g_webProxy);
    }

    // Invoke the framework - this is an infinite loop and shouldn't return
    gfserver_serve(&gfs);

    // not reached
    return 0;
}
