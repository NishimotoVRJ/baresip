/**
 * @file queue.h  libavformat video-source
 *
 * Copyright (C) 2016 Jefry Tedjokusumo
 */

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

void packet_queue_init(PacketQueue *q);

void packet_queue_flush(PacketQueue *q);

void packet_queue_end(PacketQueue *q);

int packet_queue_put(PacketQueue *q, AVPacket *pkt);

void packet_queue_abort(PacketQueue *q);

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);

