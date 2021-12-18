#include "gfserver.h"
#include "cache-student.h"

/*
 * Placeholder demonstrates use of gfserver library, replace with your own
 * implementation and any other functions you may need.
 */
ssize_t handle_with_cache(gfcontext_t *ctx, const char *path, void* arg){
    size_t bytes_transferred = 0;
    ContextWebProxy_t *webProxyCxt = (ContextWebProxy_t *) arg;
    void* shmDataAddr;
    MSQRequest_t cache_req;

    //Pop request from the queue
    pthread_mutex_lock(&proxy_lock->mutex);
    while(steque_isempty(proxy_queue)){
        pthread_cond_wait(&proxy_lock->cond, &proxy_lock->mutex);
    }
    ContextProxy_t * contxtProxy = (ContextProxy_t*) steque_pop(proxy_queue);
    pthread_mutex_unlock(&proxy_lock->mutex);

    if(contxtProxy == NULL){
        fprintf(stdout, "Failed to read request queue in current thread\n");
        goto EXIT;
    }


    sprintf(cache_req.filePath, "%s", path);
    cache_req.nSegments = webProxyCxt->nSegments;
    cache_req.segmentSize = webProxyCxt->segmentSize;
    strcpy(cache_req.shmName, contxtProxy->shm_name);

    if (webProxyCxt->mqRequest < 0){
        fprintf(stderr, "webProxyCxt->mqResponse is invalid\n");
        return SERVER_FAILURE;
    }

    fprintf(stdout, "cache_req.filePath %s \n", cache_req.filePath);
    if (mq_send(webProxyCxt->mqRequest, (const char *) &cache_req, sizeof(MSQRequest_t), 0) == -1){
        fprintf(stderr, "mq_send failed with errCode : %d webProxyCxt->mqRequest %d \n", errno, webProxyCxt->mqRequest);
        bytes_transferred = 0;
        return SERVER_FAILURE;
    }

    // Lock semaphores for reading
    sem_wait(&contxtProxy->shm_context->semREAD);

    if (contxtProxy->shm_context->status == GF_OK){ /*GF_OK*/
        fprintf(stdout, "Posting gf_sendheader GF_OK file with filelen %zu \n", contxtProxy->shm_context->fileLen);
        gfs_sendheader(ctx, GF_OK, contxtProxy->shm_context->fileLen);

        size_t write_len = 0;
        bytes_transferred = 0;
        while(bytes_transferred < contxtProxy->shm_context->fileLen){
            if (contxtProxy->shm_context->dataLen <= 0){
                fprintf(stderr, "handle_with_cache read error, %zd, %zu, %zu",
                        contxtProxy->shm_context->dataLen, bytes_transferred,contxtProxy->shm_context->fileLen);
                return SERVER_FAILURE;
            }

            shmDataAddr = contxtProxy->shm_context + 1;
            char localBuf[contxtProxy->shm_context->dataLen];

            memcpy(&localBuf, shmDataAddr, contxtProxy->shm_context->dataLen); // Copy only read length not segSize - sizeof(ContextShm_t) here, writer takes care of writing only that calculated amount.

            write_len = gfs_send(ctx, &localBuf, contxtProxy->shm_context->dataLen);

            if (write_len != contxtProxy->shm_context->dataLen){
                fprintf(stderr, "gfs_send write error\n");
                return SERVER_FAILURE;
            }

            bytes_transferred += write_len;

            // give turn to writer
            sem_post(&contxtProxy->shm_context->semWRITE);

            //wait for next read turn
            sem_wait(&contxtProxy->shm_context->semREAD);

        }
    }
    else{
        fprintf(stdout, "Posting gfs_sendheader GF_FILE_NOT_FOUND\n");
        gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
    }


    //Post it to writer to go with another request now
    sem_post(&contxtProxy->shm_context->semWRITE);

    // Release shared memory for other threads
    EXIT:
    fprintf(stdout, "Release SHM: bytes_transferred: %zu FileLen: %zu FilePath %s \n", bytes_transferred, contxtProxy->shm_context->fileLen, contxtProxy->shm_context->filePath);
    if (proxy_queue){
        contxtProxy->shm_context->fileLen = 0;
        bzero(contxtProxy->shm_context->filePath, MAX_PATH_LEN);
        contxtProxy->shm_context->dataLen = 0;

        pthread_mutex_lock(&proxy_lock->mutex);
        steque_enqueue(proxy_queue, contxtProxy);
        pthread_mutex_unlock(&proxy_lock->mutex);
        pthread_cond_broadcast(&proxy_lock->cond);
    }

    return bytes_transferred;
}
