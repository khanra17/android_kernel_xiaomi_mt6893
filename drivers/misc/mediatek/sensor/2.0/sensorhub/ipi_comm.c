/*
 * Copyright (C) 2020 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#define pr_fmt(fmt) "ipi_comm " fmt

#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/delay.h>

#include "ipi_comm.h"
#include "scp_ipi_pin.h"
#include "scp_mbox_layout.h"
#include "mt-plat/mtk_tinysys_ipi.h"

struct ipi_controller {
	spinlock_t lock;
	bool running;
	struct list_head head;
	struct workqueue_struct *workqueue;
	struct work_struct work;
	void (*notify_callback)(int id, void *data, unsigned int len);
};

struct ipi_hw_transfer {
	struct completion done;
	int count;
	/* data buffers */
	int id;
	const unsigned char *tx;
	unsigned char *rx;
	unsigned int tx_len;
	unsigned int rx_len;
	void *context;
};

#define ipi_len(x) (((x) + MBOX_SLOT_SIZE - 1) / MBOX_SLOT_SIZE)

static struct ipi_controller controller;
static struct ipi_hw_transfer hw_transfer;
static DEFINE_SPINLOCK(hw_transfer_lock);
static uint8_t ctrl_payload[PIN_IN_SIZE_SENSOR_CTRL * MBOX_SLOT_SIZE];
static uint8_t notify_payload[PIN_IN_SIZE_SENSOR_NOTIFY * MBOX_SLOT_SIZE];

static int ipi_transfer_buffer(struct ipi_transfer *t)
{
	int ret = 0, retry = 0;
	int timeout;
	unsigned long flags;
	struct ipi_hw_transfer *hw = &hw_transfer;

	spin_lock_irqsave(&hw_transfer_lock, flags);
	hw->id = t->id;
	hw->tx = t->tx_buf;
	hw->rx = t->rx_buf;
	hw->tx_len = t->tx_len;
	hw->rx_len = t->rx_len;

	reinit_completion(&hw->done);
	hw->context = &hw->done;
	spin_unlock_irqrestore(&hw_transfer_lock, flags);
	do {
		ret = mtk_ipi_send(&scp_ipidev, hw->id, 0,
			(unsigned char *)hw->tx, ipi_len(hw->tx_len), 0);
		if (ret < 0 && ret != IPI_PIN_BUSY)
			return -EIO;
		if (ret == IPI_PIN_BUSY) {
			if (retry++ == 1000)
				return -EBUSY;
			if (retry % 100 == 0)
				usleep_range(1000, 2000);
		}
	} while (ret == IPI_PIN_BUSY);

	timeout = wait_for_completion_timeout(&hw->done,
			msecs_to_jiffies(500));
	spin_lock_irqsave(&hw_transfer_lock, flags);
	if (!timeout)
		hw->count = -ETIMEDOUT;
	hw->context = NULL;
	spin_unlock_irqrestore(&hw_transfer_lock, flags);
	return hw->count;
}

static void ipi_complete(void *arg)
{
	complete(arg);
}

static void ipi_transfer_messages(void)
{
	struct ipi_message *m;
	struct ipi_transfer *t = NULL;
	int status = 0;
	unsigned long flags;

	spin_lock_irqsave(&controller.lock, flags);
	if (list_empty(&controller.head) || controller.running)
		goto out;
	controller.running = true;
	while (!list_empty(&controller.head)) {
		m = list_first_entry(&controller.head,
			struct ipi_message, list);
		list_del(&m->list);
		spin_unlock_irqrestore(&controller.lock, flags);
		list_for_each_entry(t, &m->transfers, transfer_list) {
			if (!t->tx_buf && t->tx_len) {
				status = -EINVAL;
				break;
			}
			if (t->tx_len)
				status = ipi_transfer_buffer(t);
			if (status < 0) {
				break;
			} else if (status != t->rx_len) {
				status = -EBADMSG;
				break;
			}
			status = 0;
		}
		m->status = status;
		m->complete(m->context);
		spin_lock_irqsave(&controller.lock, flags);
	}
	controller.running = false;
out:
	spin_unlock_irqrestore(&controller.lock, flags);
}

static void ipi_prefetch_messages(void)
{
	ipi_transfer_messages();
}

static void ipi_work(struct work_struct *work)
{
	ipi_transfer_messages();
}

static int __ipi_transfer(struct ipi_message *m)
{
	unsigned long flags;

	m->status = -EINPROGRESS;

	spin_lock_irqsave(&controller.lock, flags);
	list_add_tail(&m->list, &controller.head);
	queue_work(controller.workqueue, &controller.work);
	spin_unlock_irqrestore(&controller.lock, flags);
	return 0;
}

static int __ipi_xfer(struct ipi_message *message)
{
	DECLARE_COMPLETION_ONSTACK(done);
	int status;

	message->complete = ipi_complete;
	message->context = &done;

	status = __ipi_transfer(message);

	if (status == 0) {
		ipi_prefetch_messages();
		wait_for_completion(&done);
		status = message->status;
	}
	message->context = NULL;
	return status;
}

static int ipi_sync(int id, const unsigned char *txbuf, unsigned int n_tx,
		unsigned char *rxbuf, unsigned int n_rx)
{
	struct ipi_transfer t;
	struct ipi_message m;

	t.id = id;
	t.tx_buf = txbuf;
	t.tx_len = n_tx;
	t.rx_buf = rxbuf;
	t.rx_len = n_rx;

	ipi_message_init(&m);
	ipi_message_add_tail(&t, &m);

	return __ipi_xfer(&m);
}

static int ipi_async(struct ipi_message *m)
{
	return __ipi_transfer(m);
}

int ipi_comm_sync(int id, unsigned char *tx, unsigned int n_tx,
		unsigned char *rx, unsigned int n_rx)
{
	return ipi_sync(id, tx, n_tx, rx, n_rx);
}

int ipi_comm_async(struct ipi_message *m)
{
	return ipi_async(m);
}

int ipi_comm_noack(int id, unsigned char *tx, unsigned int n_tx)
{
	int ret = 0, retry = 0;

	do {
		ret = mtk_ipi_send(&scp_ipidev, id, 0, tx, ipi_len(n_tx), 0);
		if (ret < 0 && ret != IPI_PIN_BUSY)
			return -EIO;
		if (ret == IPI_PIN_BUSY) {
			if (retry++ == 1000)
				return -EBUSY;
			if (retry % 100 == 0)
				usleep_range(1000, 2000);
		}
	} while (ret == IPI_PIN_BUSY);

	return 0;
}

static void ipi_comm_complete(unsigned char *buffer, unsigned int len)
{
	struct ipi_hw_transfer *hw = &hw_transfer;

	spin_lock(&hw_transfer_lock);
	if (!hw->context) {
		pr_err("dropped transfer\n");
		goto out;
	}
	/* only copy hw->rx_len bytes to hw->rx to avoid memory corruption */
	memcpy(hw->rx, buffer, hw->rx_len);
	/* hw->count give real len */
	hw->count = len;
	complete(hw->context);
out:
	spin_unlock(&hw_transfer_lock);
}

static int ipi_comm_ctrl_handler(unsigned int id, void *prdata,
		void *data, unsigned int len)
{
	WARN_ON(id != IPI_IN_SENSOR_CTRL);
	ipi_comm_complete(data, len);
	return 0;
}

static int ipi_comm_notify_handler(unsigned int id, void *prdata,
		void *data, unsigned int len)
{
	WARN_ON(id != IPI_IN_SENSOR_NOTIFY);
	if (controller.notify_callback)
		controller.notify_callback(id, data, len);
	return 0;
}

int get_ctrl_id(void)
{
	return IPI_OUT_SENSOR_CTRL;
}

int get_notify_id(void)
{
	return IPI_OUT_SENSOR_NOTIFY;
}

void ipi_comm_notify_handler_register(
		void (*f)(int id, void *data, unsigned int len))
{
	controller.notify_callback = f;
}

void ipi_comm_notify_handler_unregister(void)
{
	controller.notify_callback = NULL;
}

int ipi_comm_init(void)
{
	int ret = 0;

	init_completion(&hw_transfer.done);
	INIT_WORK(&controller.work, ipi_work);
	INIT_LIST_HEAD(&controller.head);
	spin_lock_init(&controller.lock);
	controller.workqueue = alloc_workqueue("ipi_comm",
		WQ_MEM_RECLAIM | WQ_HIGHPRI, 0);
	if (controller.workqueue == NULL) {
		pr_err("create workqueue fail\n");
		return -1;
	}
	ret = mtk_ipi_register(&scp_ipidev, IPI_IN_SENSOR_CTRL,
		ipi_comm_ctrl_handler, NULL, ctrl_payload);
	if (ret < 0)
		pr_err("register ipi %u fail %d\n", IPI_IN_SENSOR_CTRL, ret);
	ret = mtk_ipi_register(&scp_ipidev, IPI_IN_SENSOR_NOTIFY,
		ipi_comm_notify_handler, NULL, notify_payload);
	if (ret < 0)
		pr_err("register ipi %u fail %d\n", IPI_IN_SENSOR_NOTIFY, ret);
	return 0;
}

void ipi_comm_exit(void)
{
	mtk_ipi_unregister(&scp_ipidev, IPI_IN_SENSOR_CTRL);
	mtk_ipi_unregister(&scp_ipidev, IPI_IN_SENSOR_NOTIFY);
	flush_workqueue(controller.workqueue);
	destroy_workqueue(controller.workqueue);
}
