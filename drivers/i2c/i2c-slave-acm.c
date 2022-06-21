// SPDX-License-Identifier: GPL-2.0
/*
 * I2C slave for ACM data from the K60 controller of BCM85100 modem
 *
 * Copyright (C) 2022 Siklu Communication Ltd.
 */
#include <linux/i2c.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/ktime.h>

#define BUFFER_MAX_SIZE		16

struct slave_acm_data {
	spinlock_t buffer_lock;
	u8 buffer[BUFFER_MAX_SIZE];
	u8 buffer_size;
	u8 tmp_buf[BUFFER_MAX_SIZE];
	u8 buf_idx;
	ktime_t timestamp;
	struct dentry *debugfs;
};

static int i2c_slave_acm_cb(struct i2c_client *client,
		enum i2c_slave_event event, u8 *val)
{
	struct slave_acm_data *slave_acm = i2c_get_clientdata(client);
	unsigned long flags;

	switch (event) {
	case I2C_SLAVE_WRITE_RECEIVED:
		if (slave_acm->buf_idx < sizeof(slave_acm->tmp_buf))
			slave_acm->tmp_buf[slave_acm->buf_idx++] = *val;
		break;

	case I2C_SLAVE_READ_PROCESSED:
	case I2C_SLAVE_READ_REQUESTED:
		*val = 0;
		break;

	case I2C_SLAVE_STOP:
		spin_lock_irqsave(&slave_acm->buffer_lock, flags);
		memcpy(slave_acm->buffer, slave_acm->tmp_buf,
				slave_acm->buf_idx);
		slave_acm->buffer_size = slave_acm->buf_idx;
		slave_acm->timestamp = ktime_get();
		spin_unlock_irqrestore(&slave_acm->buffer_lock, flags);
		break;

	case I2C_SLAVE_WRITE_REQUESTED:
		slave_acm->buf_idx = 0;
		break;

	default:
		break;
	}

	return 0;
}

static int slave_acm_show(struct seq_file *m, void *unused)
{
	struct slave_acm_data *slave_acm = m->private;
	unsigned long flags;

	spin_lock_irqsave(&slave_acm->buffer_lock, flags);
	if (ktime_ms_delta(ktime_get(), slave_acm->timestamp) < 10)
		seq_write(m, slave_acm->buffer, slave_acm->buffer_size);
	spin_unlock_irqrestore(&slave_acm->buffer_lock, flags);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slave_acm);

static int i2c_slave_acm_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct slave_acm_data *slave_acm;
	struct dentry *debugfs;

	slave_acm = devm_kzalloc(&client->dev, sizeof(struct slave_acm_data),
			GFP_KERNEL);
	if (!slave_acm)
		return -ENOMEM;
	i2c_set_clientdata(client, slave_acm);

	debugfs = debugfs_create_dir("slave_acm", NULL);
	if (IS_ERR(debugfs))
		return PTR_ERR(debugfs);
	debugfs_create_file("data", 0600, debugfs, slave_acm, &slave_acm_fops);

	spin_lock_init(&slave_acm->buffer_lock);
	slave_acm->debugfs = debugfs;

	return i2c_slave_register(client, i2c_slave_acm_cb);
}

static int i2c_slave_acm_remove(struct i2c_client *client)
{
	struct slave_acm_data *slave_acm = i2c_get_clientdata(client);

	debugfs_remove(slave_acm->debugfs);
	i2c_slave_unregister(client);

	return 0;
}

static const struct i2c_device_id i2c_slave_acm_id[] = {
	{ .name = "slave-acm" },
	{ },
};
MODULE_DEVICE_TABLE(i2c, i2c_slave_acm_id);

static struct i2c_driver i2c_slave_acm_driver = {
	.driver = {
		.name = "i2c-slave-acm",
	},
	.probe = i2c_slave_acm_probe,
	.remove = i2c_slave_acm_remove,
	.id_table = i2c_slave_acm_id,
};
module_i2c_driver(i2c_slave_acm_driver);
