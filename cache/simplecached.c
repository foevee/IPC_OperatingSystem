#include <stdio.h>
#include <unistd.h>
#include <printf.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <sys/signal.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>

#include "gfserver.h"
#include "cache-student.h"
#include "shm_channel.h"
#include "simplecache.h"

#if !defined(CACHE_FAILURE)
#define CACHE_FAILURE (-1)
#endif // CACHE_FAILURE

#define MAX_CACHE_REQUEST_LEN 82021

bool quitProcess = false;
MSQRequest_t *g_request;

static void _sig_handler(int signo){
    if (signo == SIGINT || signo == SIGTERM){
        /* Unlink IPC mechanisms here*/
        quitProcess = true;
        if (g_request) free(g_request);
        exit(signo);
    }
}

unsigned long int cache_delay;

#define USAGE                                                                 \
"usage:\n"                                                                    \
"  simplecached [options]\n"                                                  \
"options:\n"                                                                  \
"  -c [cachedir]       Path to static files (Default: ./)\n"                  \
"  -t [thread_count]   Thread count for work queue (Default is 42, Range is 1-235711)\n"      \
"  -d [delay]          Delay in simplecache_get (Default is 0, Range is 0-2500000 (microseconds)\n "	\
"  -h                  Show this help message\n"

/* OPTIONS DESCRIPTOR ====================================================== */
static struct option gLongOptions[] = {
        {"cachedir",           required_argument,      NULL,           'c'},
        {"help",               no_argument,            NULL,           'h'},
        {"nthreads",           required_argument,      NULL,           't'},
        {"hidden",			 no_argument,			 NULL,			 'i'}, /* server side */
        {"delay", 			 required_argument,		 NULL, 			 'd'}, // delay.
        {NULL,                 0,                      NULL,             0}
};

void Usage() {
    fprintf(stdout, "%s", USAGE);
}

int main(int argc, char **argv) {
    int nthreads = 7;
    char *cachedir = "locals.txt";
    char option_char;

    /* disable buffering to stdout */
    setbuf(stdout, NULL);

    while ((option_char = getopt_long(argc, argv, "id:c:hlxt:", gLongOptions, NULL)) != -1) {
        switch (option_char) {
            default:
                Usage();
                exit(1);
            case 'h': // help
                Usage();
                exit(0);
                break;
            case 't': // thread-count
                nthreads = atoi(optarg);
                break;
            case 'c': //cache directory
                cachedir = optarg;
                break;
            case 'd':
                cache_delay = (unsigned long int) atoi(optarg);
                break;
            case 'i': // server side usage
            case 'u': // experimental
            case 'j': // experimental
                break;
        }
    }

    if (cache_delay > 2500000) {
        fprintf(stderr, "Cache delay must be less than 2500000 (us)\n");
        exit(__LINE__);
    }

    if ((nthreads>235711) || (nthreads < 1)) {
        fprintf(stderr, "Invalid number of threads must be in between 1-235711\n");
        exit(__LINE__);
    }

    if (SIG_ERR == signal(SIGTERM, _sig_handler)){
        fprintf(stderr,"Unable to catch SIGTERM...exiting.\n");
        exit(CACHE_FAILURE);
    }

    if (SIG_ERR == signal(SIGINT, _sig_handler)){
        fprintf(stderr,"Unable to catch SIGINT...exiting.\n");
        exit(CACHE_FAILURE);
    }

    // Initialize cache
    simplecache_init(cachedir);

    // Cache code goes here
    threadInfo_t threadsInfo[nthreads];

    cache_queue = (steque_t*) malloc(sizeof(steque_t));
    steque_init(cache_queue);

    cache_lock  = (lock_t*) malloc(sizeof(lock_t));
    pthread_cond_init(&cache_lock->cond, NULL);
    pthread_mutex_init(&cache_lock->mutex, NULL);

    // Open message request queue (with R/W locks) again and post the request
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_MSG_NUM;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    for(int i=0; i <= nthreads; i++){
        threadsInfo[i].isEnabled = true;
        if (pthread_create(&threadsInfo[i].pthread, NULL, cache_worker, &threadsInfo[i])){
            fprintf(stderr, "Error creating thread");
        }
    }

    // OPEN Message Request Queue (passing locks) again and read the request
    mqd_t mqRequest;
    while((mqRequest = mq_open(MQ_REQUEST_NAME, O_RDWR, 0666, &attr)) < 0){
        fprintf(stdout, "keep waiting for message queue %s \n", MQ_REQUEST_NAME);
        sleep(1);
    }

    while(!quitProcess){
        //read MQ_REQUEST and enqueue
        g_request = (MSQRequest_t *) malloc(MAX_MSG_SIZE);
        if (mq_receive(mqRequest, (char *)g_request, MAX_MSG_SIZE, 0) == -1){
            fprintf(stdout, "Warning: keep waiting on mq_receive. \n");
            continue;
        }

        if (cache_queue){
            pthread_mutex_lock(&cache_lock->mutex);
            steque_enqueue(cache_queue, g_request);
            pthread_mutex_unlock(&cache_lock->mutex);
        }

        if (pthread_cond_signal(&cache_lock->cond) != 0)
            fprintf(stderr, "Broadcast Failed with Error %s \n", strerror(errno));
    }

    pthread_mutex_destroy(&cache_lock->mutex);
    pthread_cond_destroy(&cache_lock->cond);

    // Clean up
    if (cache_queue) steque_destroy(cache_queue);
    if (cache_lock) free(cache_lock);

    for (int i=0; i <= nthreads; i++){
        threadsInfo[i].isEnabled = false; //signal all thread to close
        if (pthread_cond_broadcast(&cache_lock->cond) != 0)
            fprintf(stderr, "error broadcasting with error %s \n", strerror(errno));
        pthread_join(threadsInfo[i].pthread, NULL);
    }

    // Won't execute
    return 0;
}

void *cache_worker(void* arg){
    struct stat fileStat;
    bool isFileExist = false;

    MSQRequest_t *fileReq = NULL;
    ContextShm_t* shmMapped = NULL;
    size_t readLen = 0;
    size_t fileRead = 0;
    void* shmDataAddr;
    int fileDesc = -1;

    threadInfo_t *threadInfo = (threadInfo_t*) arg;

    //This needs to be called from handler
    while(threadInfo->isEnabled){
        //Read the request from queue
        pthread_mutex_lock(&cache_lock->mutex);
        while(steque_isempty(cache_queue))
            pthread_cond_wait(&cache_lock->cond, &cache_lock->mutex);
        fileReq = (MSQRequest_t*) steque_pop(cache_queue);
        pthread_mutex_unlock(&cache_lock->mutex);

        if (fileReq == NULL){
            fprintf(stderr, "keep waiting to read request queue\n");
            continue;
        }

        // Check if cache exist
        fprintf(stdout, "Requested file path %s \n", fileReq->filePath);
        isFileExist = false;
        if ((fileDesc = simplecache_get(fileReq->filePath)) != -1){
            if((fcntl(fileDesc, F_GETFD) != -1 || errno != EBADF) && fstat(fileDesc, &fileStat) != -1){
                isFileExist = true;
            }
        }

        // Now share the file contents since proxy is ready to receive
        int shmFD = shm_open(fileReq->shmName, O_RDWR, 0600);
        shmMapped = (ContextShm_t*) mmap(NULL, fileReq->segmentSize,PROT_READ | PROT_WRITE, MAP_SHARED, shmFD, 0);
        if(shmMapped == MAP_FAILED){
            fprintf(stderr, "simplecached mmap failed \n");
        }

        // lock the semaphore for write
        sem_wait(&shmMapped->semWRITE);
        
        if (!isFileExist){
            strcpy(shmMapped->filePath, fileReq->filePath);
            shmMapped->fileLen = 0;
            shmMapped->status = GF_FILE_NOT_FOUND;
            fprintf(stdout, "GF_FILE_NOT_FOUND for path %s \n ", fileReq->filePath);
        }
        else { //FILE EXIST
            shmDataAddr = shmMapped +1 ;
            strcpy(shmMapped->filePath, fileReq->filePath);
            shmMapped->status = GF_OK;
            shmMapped->fileLen = fileStat.st_size;

            fileRead = 0; // Start with zero
            while(fileRead < shmMapped->fileLen){
                readLen = pread(fileDesc, (char*)shmDataAddr, fileReq->segmentSize - sizeof(ContextShm_t), fileRead);
                shmMapped->dataLen = readLen;
                fileRead += readLen;

                sem_post(&shmMapped->semREAD);
                sem_wait(&shmMapped->semWRITE);

            }
            fprintf(stdout, "File Read %zu of file %s \n", fileRead, shmMapped->filePath);
        }

        // unlock the semaphore to the reader
        sem_post(&shmMapped->semREAD);

        // Release MQ Request Memmory
        if (fileReq) free(fileReq);

    }

    return (void*) NULL;

}