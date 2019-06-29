#include "queue.h"

#include <unistd.h>
#include <sys/shm.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


int LFQueue_create(int key, uint64_t data_size, uint32_t count, bool overwrite)
{
        int shmid;
        uint64_t i;
        uint64_t queue_size = 0;
        uint64_t ring_size, node_size;
        LFHeader *header;
        char *m;

        count = upper_power_of_two(count);
        data_size = (data_size + 63) & ~63;
        ring_size = sizeof(LFRing) + sizeof(LFRingNode) * count;
        node_size = data_size + sizeof(LFNode);

        queue_size += sizeof(LFHeader);
        queue_size += ring_size * 2;
        queue_size += node_size * count;

        if ((shmid = shmget(key, queue_size, IPC_CREAT | IPC_EXCL | 0666)) < 0)
                return -1;

        if ((m = (char *)shmat(shmid, NULL, 0)) == NULL)
                return -1;

        header = (LFHeader *)m;
        header->magic = QUEUE_MAGIC;
        header->node_count = count;
        header->node_data_size = data_size;
        header->node_total_size = node_size;
        header->overwrite = overwrite;
        header->key = key;

        m += sizeof(LFHeader);
        LFRing_init((LFRing *)m, count, count);
        
        m += ring_size;
        LFRing_init((LFRing *)m, count, 0);

        m += ring_size;
        memset(m, 0, node_size * count);
        
        return 0;
}

void LFQueue_reset(LFQueue *queue)
{
        uint64_t i;
        uint32_t count = queue->header->node_count;
        uint64_t data_size = queue->header->node_data_size;
        uint64_t ring_size, node_size;
        LFRing *ring;
        char *m;

        ring_size = sizeof(LFRing) + sizeof(LFRingNode) * count;
        node_size = data_size + sizeof(LFNode);
        m = (char *)queue->header;

        m += sizeof(LFHeader);
        LFRing_init((LFRing *)m, count, count);
        
        m += ring_size;
        LFRing_init((LFRing *)m, count, 0);

        m += ring_size;
        memset(m, 0, node_size * count);
}

int LFQueue_init(LFQueue *queue, void *mem)
{
        char *m = (char *)mem;
        uint64_t ring_total_size;

        queue->header = (LFHeader *)m;
        if (queue->header->magic != QUEUE_MAGIC)
                return -1;
        ring_total_size = sizeof(LFRing) + queue->header->node_count * sizeof(LFRingNode);

        m += sizeof(LFHeader);
        queue->resc_ring = (LFRing *)m;

        m += ring_total_size;
        queue->node_ring = (LFRing *)m;

        m += ring_total_size;
        queue->nodes = m;

        return 0;
}

int LFQueue_destroy(int key)
{
        int shmid = shmget(key, 0, 0);
        if (shmid < 0)
                return -1;
        return shmctl(shmid, IPC_RMID, NULL);
}

LFQueue *LFQueue_open(int key)
{
        int shmid;
        char *m;
        LFQueue *queue;

        if ((shmid = shmget(key, 0, 0)) < 0)
                return NULL;

        if ((m = shmat(shmid, NULL, 0)) == NULL)
                return NULL;

        if ((queue = malloc(sizeof(LFQueue))) == NULL) {
                shmdt(m);
                return NULL;
        }

        if (LFQueue_init(queue, m) != 0) {
                shmdt(m);
                free(queue);
                return NULL;
        }

        return queue;
}

void LFQueue_close(LFQueue *queue)
{
        if (queue) {
                if (queue->header->key >= 0)
                        shmdt(queue->header);
                free(queue);
        }
}

void LFQueue_pause(LFQueue *queue)
{
        queue->header->pause = true;
}

void LFQueue_resume(LFQueue *queue)
{
        queue->header->pause = false;
}

void LFQueue_print(LFQueue *queue)
{
        LFHeader *header = queue->header;
        LFRing *ring;

        printf("LFQueue\n");

        printf("\tLFHeader:\n");
        printf("\t\tMagic Number:\t%lu\n", header->magic);
        printf("\t\tNode Data Size:\t%lu\n", header->node_data_size);

        ring = queue->resc_ring;
        printf("\tResource LFRing:\n");
        printf("\t\tNode Count:\t%lu\n", ring->node_count);
        printf("\t\tHead Seq:\t%lu\n", ring->head_seq);
        printf("\t\tTail Seq:\t%lu\n", ring->tail_seq);

        ring = queue->node_ring;
        printf("\tNode LFRing:\n");
        printf("\t\tNode Count:\t%lu\n", ring->node_count);
        printf("\t\tHead Seq:\t%lu\n", ring->head_seq);
        printf("\t\tTail Seq:\t%lu\n", ring->tail_seq);
}

int LFQueue_push(LFQueue *queue, LFNode *node)
{
        uint32_t id;
        LFNode *n;
        LFHeader *header = queue->header;

        if (node->size > header->node_data_size)
                return -1;

        if (header->pause)
                return -3;

        id = LFRing_pop(queue->resc_ring, NULL);
        if (id == LFRING_INVALID_ID) {
                if (header->overwrite) {
                        id = LFRing_pop(queue->node_ring, NULL);
                        if (id == LFRING_INVALID_ID)
                                return -1;
                } else {
                        return -2;
                }
        }

        n = (LFNode *)(queue->nodes + header->node_total_size * id);
        memcpy(n, node, sizeof(LFNode) + node->size);
        LFRing_push(queue->node_ring, id);
        
        return 0;
}

int LFQueue_pop(LFQueue *queue, LFNode *node)
{
        uint32_t id;
        LFNode *n;
        LFHeader *header = queue->header;

        do {
                if (header->pause)
                        return -3;
                id = LFRing_pop(queue->node_ring, NULL);
        } while (id == LFRING_INVALID_ID);

        n = (LFNode *)(queue->nodes + header->node_total_size * id);
        memcpy(node, n, sizeof(LFNode) + n->size);
        LFRing_push(queue->resc_ring, id);

        return 0;
}
