/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012,2013 Magnus Edenhill
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_msg.h"
#include "rdkafka_topic.h"
#include "rdrand.h"
#include "rdtime.h"

#include "rdsysqueue.h"

void rd_kafka_msg_destroy (rd_kafka_t *rk, rd_kafka_msg_t *rkm) {

	assert(rk->rk_producer.msg_cnt > 0);
	(void)rd_atomic_sub(&rk->rk_producer.msg_cnt, 1);

	if (rkm->rkm_flags & RD_KAFKA_MSG_F_FREE)
		free(rkm->rkm_payload);

	if (rkm->rkm_key)
		rd_kafkap_bytes_destroy(rkm->rkm_key);

	free(rkm);
}

/**
 * Produce: creates a new message, runs the partitioner and enqueues
 *          into on the selected partition.
 *
 * Returns 0 on success or -1 on error.
 */
int rd_kafka_msg_new (rd_kafka_topic_t *rkt, int32_t force_partition,
		      int msgflags,
		      char *payload, size_t len,
		      const void *key, size_t keylen,
		      void *msg_opaque) {
	rd_kafka_msg_t *rkm;
	size_t mlen = sizeof(*rkm);
	rd_kafka_resp_err_t err;

	if (unlikely(len + keylen > rkt->rkt_rk->rk_conf.max_msg_size)) {
		errno = EMSGSIZE;
		return -1;
	}

	if (unlikely(rd_atomic_add(&rkt->rkt_rk->rk_producer.msg_cnt, 1) >
		     rkt->rkt_rk->rk_conf.queue_buffering_max_msgs)) {
		(void)rd_atomic_sub(&rkt->rkt_rk->rk_producer.msg_cnt, 1);
		errno = ENOBUFS;
		return -1;
	}

	/* If we are to make a copy of the payload, allocate space for it too */
	if (msgflags & RD_KAFKA_MSG_F_COPY) {
		msgflags &= ~RD_KAFKA_MSG_F_FREE;
		mlen += len;
	}

	/* Note: using malloc here, not calloc, so make sure all fields
	 *       are properly set up. */
	rkm = malloc(mlen);
	rkm->rkm_len        = len;
	rkm->rkm_flags      = msgflags;
	rkm->rkm_opaque     = msg_opaque;
	rkm->rkm_key        = rd_kafkap_bytes_new(key, keylen);
	rkm->rkm_partition  = force_partition;
	rkm->rkm_ts_timeout = rd_clock() +
		rkt->rkt_conf.message_timeout_ms * 1000;

	if (msgflags & RD_KAFKA_MSG_F_COPY) {
		/* Copy payload to space following the ..msg_t */
		rkm->rkm_payload = (void *)(rkm+1);
		memcpy(rkm->rkm_payload, payload, len);

	} else {
		/* Just point to the provided payload. */
		rkm->rkm_payload = payload;
	}


	err = rd_kafka_msg_partitioner(rkt, rkm, 1);
	if (likely(!err))
		return 0;

	/* Handle partitioner failures: it only fails when the application
	 * attempts to force a destination partition that does not exist
	 * in the cluster. */

	rd_kafka_msg_destroy(rkt->rkt_rk, rkm);

	/* Translate error codes to errnos. */
	if (err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION)
		errno = ESRCH;
	else if (err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC)
		errno = ENOENT;
	else
		errno = EINVAL; /* NOTREACHED */

	return -1;
}



/**
 * Scan 'rkmq' for messages that have timed out and remove them from
 * 'rkmq' and add to 'timedout'.
 */
int rd_kafka_msgq_age_scan (rd_kafka_msgq_t *rkmq,
			    rd_kafka_msgq_t *timedout,
			    rd_ts_t now) {
	rd_kafka_msg_t *rkm, *tmp;
	int cnt = timedout->rkmq_msg_cnt;

	/* Assume messages are added in time sequencial order */
	TAILQ_FOREACH_SAFE(rkm, &rkmq->rkmq_msgs, rkm_link, tmp) {
		if (likely(rkm->rkm_ts_timeout > now))
			break;

		rd_kafka_msgq_deq(rkmq, rkm, 1);
		rd_kafka_msgq_enq(timedout, rkm);
	}

	return timedout->rkmq_msg_cnt - cnt;
}





int32_t rd_kafka_msg_partitioner_random (const rd_kafka_topic_t *rkt,
					 const void *key, size_t keylen,
					 int32_t partition_cnt,
					 void *rkt_opaque,
					 void *msg_opaque) {
	int32_t p = rd_jitter(0, partition_cnt-1);
	if (unlikely(!rd_kafka_topic_partition_available(rkt, p)))
		return rd_jitter(0, partition_cnt-1);
	else
		return p;
}

/**
 * Assigns a message to a topic partition using a partitioner.
 * Returns RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION or .._UNKNOWN_TOPIC if
 * partitioning failed, or 0 on success.
 */
int rd_kafka_msg_partitioner (rd_kafka_topic_t *rkt, rd_kafka_msg_t *rkm,
			      int do_lock) {
	int32_t partition;
	rd_kafka_toppar_t *rktp_new;
	rd_kafka_resp_err_t err;

	if (do_lock)
		rd_kafka_topic_rdlock(rkt);

	/* Fast path for failing messages with forced partition
	 * when the partition is not available.
	 * Only fail the message if its forced partition does not
	 * exist in the Kafka cluster, given that the topic's metadata
	 * can be trusted (is not older than 3 times the metadata
	 * refresh interval). */
	if (unlikely((rkt->rkt_state == RD_KAFKA_TOPIC_S_UNKNOWN ||
		      (rkm->rkm_partition != RD_KAFKA_PARTITION_UA &&
		       (rkm->rkm_partition >= rkt->rkt_partition_cnt ))) &&
		     rd_clock() < rkt->rkt_ts_metadata +
		     (rkt->rkt_rk->rk_conf.metadata_refresh_interval_ms *
		      3 * 1000))) {
		if (rkt->rkt_partition_cnt == 0)
			err = RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
		else
			err = RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;

		if (do_lock)
			rd_kafka_topic_unlock(rkt);
		return err;
	}

	if (unlikely(rkt->rkt_partition_cnt == 0)) {
		partition = RD_KAFKA_PARTITION_UA;
	} else if (rkm->rkm_partition == RD_KAFKA_PARTITION_UA)
		partition =
			rkt->rkt_conf.partitioner(rkt,
						  rkm->rkm_key->data,
						  RD_KAFKAP_BYTES_LEN(rkm->
								      rkm_key),
						  rkt->rkt_partition_cnt,
						  rkt->rkt_conf.opaque,
						  rkm->rkm_opaque);
	else /* Partition specified by the application */
		partition = rkm->rkm_partition;

	if (partition >= rkt->rkt_partition_cnt) {
		/* Partition is unknown (locally) */

		/* Temporary error, assign to UA partition for now */
		rd_kafka_dbg(rkt->rkt_rk, TOPIC, "PART",
			     "%.*s partition [%"PRId32"] not "
			     "currently available",
			     RD_KAFKAP_STR_PR(rkt->rkt_topic),
			     partition);
		partition = RD_KAFKA_PARTITION_UA;

		/* FALLTHRU */
	}

	if (0)
		rd_kafka_dbg(rkt->rkt_rk, MSG, "PART",
			     "Message %p assigned to %.*s "
			     "partition [%"PRId32"]/%"PRId32" "
			     "(fixed [%"PRId32"])",
			     rkm, 
			     RD_KAFKAP_STR_PR(rkt->rkt_topic), partition,
			     rkt->rkt_partition_cnt,
			     rkm->rkm_partition);

	/* Get new partition */
	rktp_new = rd_kafka_toppar_get(rkt, partition, 0);

	if (likely(!rktp_new)) {
		/* Unknown topic or partition */
		if (rkt->rkt_state == RD_KAFKA_TOPIC_S_UNKNOWN)
			err = RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
		else
			err = RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;

		if (do_lock)
			rd_kafka_topic_unlock(rkt);

		return  err;
	}

	/* Partition is available: enqueue msg on partition's queue */
	rd_kafka_toppar_enq_msg(rktp_new, rkm);
	if (do_lock)
		rd_kafka_topic_unlock(rkt);
	rd_kafka_toppar_destroy(rktp_new); /* from _get() */
	return 0;
}
