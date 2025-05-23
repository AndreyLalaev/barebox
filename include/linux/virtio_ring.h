/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2018, Tuomas Tynkkynen <tuomas.tynkkynen@iki.fi>
 * Copyright (C) 2018, Bin Meng <bmeng.cn@gmail.com>
 *
 * From Linux kernel include/uapi/linux/virtio_ring.h
 */

#ifndef _LINUX_VIRTIO_RING_H
#define _LINUX_VIRTIO_RING_H

#include <linux/virtio_types.h>
#include <linux/scatterlist.h>
#include <linux/ktime.h>

/* This marks a buffer as continuing via the next field */
#define VRING_DESC_F_NEXT		1
/* This marks a buffer as write-only (otherwise read-only) */
#define VRING_DESC_F_WRITE		2
/* This means the buffer contains a list of buffer descriptors */
#define VRING_DESC_F_INDIRECT		4

/*
 * The Host uses this in used->flags to advise the Guest: don't kick me when
 * you add a buffer. It's unreliable, so it's simply an optimization. Guest
 * will still kick if it's out of buffers.
 */
#define VRING_USED_F_NO_NOTIFY		1

/*
 * The Guest uses this in avail->flags to advise the Host: don't interrupt me
 * when you consume a buffer. It's unreliable, so it's simply an optimization.
 */
#define VRING_AVAIL_F_NO_INTERRUPT	1

/* We support indirect buffer descriptors */
#define VIRTIO_RING_F_INDIRECT_DESC	28

/*
 * The Guest publishes the used index for which it expects an interrupt
 * at the end of the avail ring. Host should ignore the avail->flags field.
 *
 * The Host publishes the avail index for which it expects a kick
 * at the end of the used ring. Guest should ignore the used->flags field.
 */
#define VIRTIO_RING_F_EVENT_IDX		29

/* Virtio ring descriptors: 16 bytes. These can chain together via "next". */
struct vring_desc {
	/* Address (guest-physical) */
	__virtio64 addr;
	/* Length */
	__virtio32 len;
	/* The flags as indicated above */
	__virtio16 flags;
	/* We chain unused descriptors via this, too */
	__virtio16 next;
};

struct vring_avail {
	__virtio16 flags;
	__virtio16 idx;
	__virtio16 ring[];
};

struct vring_used_elem {
	/* Index of start of used descriptor chain */
	__virtio32 id;
	/* Total length of the descriptor chain which was used (written to) */
	__virtio32 len;
};

struct vring_used {
	__virtio16 flags;
	__virtio16 idx;
	struct vring_used_elem ring[];
};

struct vring {
	unsigned int num;
	struct vring_desc *desc;
	struct vring_avail *avail;
	struct vring_used *used;
};

struct vring_desc_state_split {
	void *data;			/* Data for callback. */
};

/**
 * virtqueue - a queue to register buffers for sending or receiving.
 *
 * @list: the chain of virtqueues for this device
 * @vdev: the virtio device this queue was created for
 * @index: the zero-based ordinal number for this queue
 * @num_free: number of elements we expect to be able to fit
 * @vring: actual memory layout for this queue
 * @event: host publishes avail event idx
 * @free_head: head of free buffer list
 * @num_added: number we've added since last sync
 * @last_used_idx: last used index we've seen
 * @avail_flags_shadow: last written value to avail->flags
 * @avail_idx_shadow: last written value to avail->idx in guest byte order
 * @desc_state: cookie data associated with descritptors
 */
struct virtqueue {
	struct list_head list;
	struct virtio_device *vdev;
	unsigned int index;
	unsigned int num_free;
	struct vring vring;
	bool event;
	unsigned int free_head;
	unsigned int num_added;
	u16 last_used_idx;
	u16 avail_flags_shadow;
	u16 avail_idx_shadow;
	dma_addr_t queue_dma_addr;
	size_t queue_size_in_bytes;
	struct vring_desc_state_split desc_state[];
};

/*
 * Alignment requirements for vring elements.
 * When using pre-virtio 1.0 layout, these fall out naturally.
 */
#define VRING_AVAIL_ALIGN_SIZE		2
#define VRING_USED_ALIGN_SIZE		4
#define VRING_DESC_ALIGN_SIZE		16

/*
 * We publish the used event index at the end of the available ring,
 * and vice versa. They are at the end for backwards compatibility.
 */
#define vring_used_event(vr)	((vr)->avail->ring[(vr)->num])
#define vring_avail_event(vr)	(*(__virtio16 *)&(vr)->used->ring[(vr)->num])

static inline void vring_init(struct vring *vr, unsigned int num, void *p,
			      unsigned long align)
{
	vr->num = num;
	vr->desc = p;
	vr->avail = p + num * sizeof(struct vring_desc);
	vr->used = (void *)(((uintptr_t)&vr->avail->ring[num] +
		   sizeof(__virtio16) + align - 1) & ~(align - 1));
}

static inline unsigned int vring_size(unsigned int num, unsigned long align)
{
	return ((sizeof(struct vring_desc) * num +
		sizeof(__virtio16) * (3 + num)  + align - 1) & ~(align - 1)) +
		sizeof(__virtio16) * 3 + sizeof(struct vring_used_elem) * num;
}

/*
 * The following is used with USED_EVENT_IDX and AVAIL_EVENT_IDX.
 * Assuming a given event_idx value from the other side, if we have just
 * incremented index from old to new_idx, should we trigger an event?
 */
static inline int vring_need_event(__u16 event_idx, __u16 new_idx, __u16 old)
{
	/*
	 * Note: Xen has similar logic for notification hold-off
	 * in include/xen/interface/io/ring.h with req_event and req_prod
	 * corresponding to event_idx + 1 and new_idx respectively.
	 * Note also that req_event and req_prod in Xen start at 1,
	 * event indexes in virtio start at 0.
	 */
	return (__u16)(new_idx - event_idx - 1) < (__u16)(new_idx - old);
}

/**
 * virtqueue_add_sgs - expose buffers to other end
 *
 * @vq:		the struct virtqueue we're talking about
 * @sgs:	array of terminated scatterlists
 * @out_sgs:	the number of scatterlists readable by other side
 * @in_sgs:	the number of scatterlists which are writable
 *		(after readable ones)
 * @data:	the token identifying the buffer.
 *
 * Caller must ensure we don't call this with other virtqueue operations
 * at the same time (except where noted).
 *
 * Returns zero or a negative error (ie. ENOSPC, ENOMEM, EIO).
 */
int virtqueue_add_sgs(struct virtqueue *vq, struct scatterlist *sgs[],
		      unsigned int out_sgs, unsigned int in_sgs,
		      void *data);

/**
 * virtqueue_add_outbuf - expose output buffers to other end
 * @vq: the struct virtqueue we're talking about.
 * @sg: scatterlist (must be well-formed and terminated!)
 * @num: the number of entries in @sg readable by other side
 * @data: the token identifying the buffer.
 *
 * Caller must ensure we don't call this with other virtqueue operations
 * at the same time (except where noted).
 *
 * Returns zero or a negative error (ie. ENOSPC, ENOMEM, EIO).
 */
static inline int virtqueue_add_outbuf(struct virtqueue *vq,
				       struct scatterlist *sg, unsigned int num,
				       void *data)
{
	return virtqueue_add_sgs(vq, &sg, num, 0, data);
}

/**
 * virtqueue_add_inbuf - expose input buffers to other end
 * @vq: the struct virtqueue we're talking about.
 * @sg: scatterlist (must be well-formed and terminated!)
 * @num: the number of entries in @sg writable by other side
 * @data: the token identifying the buffer.
 *
 * Caller must ensure we don't call this with other virtqueue operations
 * at the same time (except where noted).
 *
 * Returns zero or a negative error (ie. ENOSPC, ENOMEM, EIO).
 */
static inline int virtqueue_add_inbuf(struct virtqueue *vq,
				      struct scatterlist *sg, unsigned int num,
				      void *data)
{
	return virtqueue_add_sgs(vq, &sg, 0, num, data);
}

/**
 * virtqueue_kick - update after add_buf
 *
 * @vq:		the struct virtqueue
 *
 * After one or more virtqueue_add_sgs() calls, invoke this to kick
 * the other side.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 */
void virtqueue_kick(struct virtqueue *vq);

/**
 * virtqueue_get_buf - get the next used buffer
 *
 * @vq:		the struct virtqueue we're talking about
 * @len:	the length written into the buffer
 *
 * If the device wrote data into the buffer, @len will be set to the
 * amount written. This means you don't need to clear the buffer
 * beforehand to ensure there's no data leakage in the case of short
 * writes.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 *
 * Returns NULL if there are no used buffers, or the "data" token
 * handed to virtqueue_add_*().
 */
void *virtqueue_get_buf(struct virtqueue *vq, unsigned int *len);

/**
 * virtqueue_get_buf_timeout - poll the next used buffer with timeout
 *
 * @vq:		the struct virtqueue we're talking about
 * @len:	the length written into the buffer
 * @timeout:	the timeout in nanoseconds
 *
 * If the device wrote data into the buffer, @len will be set to the
 * amount written. This means you don't need to clear the buffer
 * beforehand to ensure there's no data leakage in the case of short
 * writes.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 *
 * Returns NULL on timeout, or the "data" token
 * handed to virtqueue_add_*().
 */
void *virtqueue_get_buf_timeout(struct virtqueue *vq, unsigned int *len,
				ktime_t timeout);

/**
 * vring_create_virtqueue - create a virtqueue for a virtio device
 *
 * @index:	the index of the queue
 * @num:	number of elements of the queue
 * @vring_align:the alignment requirement of the descriptor ring
 * @udev:	the virtio transport device
 * @return: the virtqueue pointer or NULL if failed
 *
 * This creates a virtqueue and allocates the descriptor ring for a virtio
 * device. The caller should query virtqueue_get_ring_size() to learn the
 * actual size of the ring.
 *
 * This API is supposed to be called by the virtio transport driver in the
 * virtio find_vqs() uclass method.
 */
struct virtqueue *vring_create_virtqueue(unsigned int index, unsigned int num,
					 unsigned int vring_align,
					 struct virtio_device *vdev);

/**
 * vring_del_virtqueue - destroy a virtqueue
 *
 * @vq:		the struct virtqueue we're talking about
 *
 * This destroys a virtqueue. If created with vring_create_virtqueue(),
 * this also frees the descriptor ring.
 *
 * This API is supposed to be called by the virtio transport driver in the
 * virtio del_vqs() uclass method.
 */
void vring_del_virtqueue(struct virtqueue *vq);

/**
 * virtqueue_get_vring_size - get the size of the virtqueue's vring
 *
 * @vq:		the struct virtqueue containing the vring of interest
 * @return: the size of the vring in a virtqueue.
 */
unsigned int virtqueue_get_vring_size(struct virtqueue *vq);

/**
 * virtqueue_get_desc_addr - get the vring descriptor table address
 *
 * @vq:		the struct virtqueue containing the vring of interest
 * @return: the descriptor table address of the vring in a virtqueue.
 */
dma_addr_t virtqueue_get_desc_addr(struct virtqueue *vq);

/**
 * virtqueue_get_avail_addr - get the vring available ring address
 *
 * @vq:		the struct virtqueue containing the vring of interest
 * @return: the available ring address of the vring in a virtqueue.
 */
dma_addr_t virtqueue_get_avail_addr(struct virtqueue *vq);

/**
 * virtqueue_get_used_addr - get the vring used ring address
 *
 * @vq:		the struct virtqueue containing the vring of interest
 * @return: the used ring address of the vring in a virtqueue.
 */
dma_addr_t virtqueue_get_used_addr(struct virtqueue *vq);

/**
 * virtqueue_poll - query pending used buffers
 *
 * @vq:			the struct virtqueue we're talking about
 * @last_used_idx:	virtqueue last used index
 *
 * Returns "true" if there are pending used buffers in the queue.
 */
bool virtqueue_poll(const struct virtqueue *vq, u16 last_used_idx);

/**
 * virtio_notify() - notify the device to process the queue
 *
 * @vdev:	the real virtio device
 * @vq:		virtqueue to process
 * @return 0 if OK, -ve on error
 */
int virtio_notify(struct virtio_device *vdev, struct virtqueue *vq);


/**
 * virtqueue_dump - dump the virtqueue for debugging
 *
 * @vq:		the struct virtqueue we're talking about
 *
 * Caller must ensure we don't call this with other virtqueue operations
 * at the same time (except where noted).
 */
void virtqueue_dump(struct virtqueue *vq);

/*
 * Barriers in virtio are tricky. Since we are not in a hyperviosr/guest
 * scenario, having these as nops is enough to work as expected.
 */

static inline void virtio_mb(void)
{
}

static inline void virtio_rmb(void)
{
}

static inline void virtio_wmb(void)
{
}

static inline void virtio_store_mb(__virtio16 *p, __virtio16 v)
{
	WRITE_ONCE(*p, v);
}

#endif /* _LINUX_VIRTIO_RING_H */
