#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <asm/uaccess.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <mach/gpio.h>
#include <linux/poll.h>

static struct class *forthdrv_class;

volatile unsigned long *gph2con;
volatile unsigned long *gph2dat;

volatile unsigned long *gph3con;
volatile unsigned long *gph3dat;

static DECLARE_WAIT_QUEUE_HEAD(button_waitq);

/* 中断事件标志, 中断服务程序将它置1，forth_drv_read将它清0 */
static volatile int ev_press = 0;

static struct fasync_struct *fasync;

struct pin_desc{
	unsigned int pin;
	unsigned int key_val;
};

/* 键值: 按下时, 0x01, 0x02, 0x03, 0x04, 0x05 */
/* 键值: 松开时, 0x81, 0x82, 0x83, 0x84, 0x85 */
static unsigned char key_val;

struct pin_desc pins_desc[5] = {
	{S5PV210_GPH2(3), 0x01},
	{S5PV210_GPH3(0), 0x02},
	{S5PV210_GPH3(1), 0x03},
	{S5PV210_GPH3(2), 0x04},
	{S5PV210_GPH3(3), 0x05},
};


static atomic_t canopen = ATOMIC_INIT(1);     //定义原子变量v并初始化为1

static DECLARE_MUTEX(button_lock);//定义互斥锁


/*
  * 确定按键值
  */
static irqreturn_t buttons_irq(int irq, void *dev_id)
{
	struct pin_desc * pindesc = (struct pin_desc *)dev_id;
	unsigned int pinval;
		
	pinval = gpio_get_value(pindesc->pin);

	if (pinval)
	{
		/* 松开 */
		key_val = 0x80 | pindesc->key_val;
	}
	else
	{
		/* 按下 */
		key_val = pindesc->key_val;
	}

	ev_press = 1;                  /* 表示中断发生了 */
	wake_up_interruptible(&button_waitq);   /* 唤醒休眠的进程 */

	kill_fasync(&fasync, SIGIO, POLL_IN);
	
	return IRQ_RETVAL(IRQ_HANDLED);
}

static int forth_drv_open(struct inode *inode, struct file *file)
{
	if (!atomic_dec_and_test(&canopen))
	{
		atomic_inc(&canopen);
		return  -EBUSY; /*已经打开*/		
	}

	if (down_trylock(&button_lock))  //获得打开锁
		 return  -EBUSY;  //设备忙


	/* 注册中断 */
	request_irq(IRQ_EINT(19),  buttons_irq, IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "K4", &pins_desc[0]);
	request_irq(IRQ_EINT(24),  buttons_irq, IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "K5", &pins_desc[1]);
	request_irq(IRQ_EINT(25), buttons_irq, IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "K6", &pins_desc[2]);
	request_irq(IRQ_EINT(26), buttons_irq, IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "K7", &pins_desc[3]);	
	request_irq(IRQ_EINT(27), buttons_irq, IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "K8", &pins_desc[4]);	

	return 0;
}

ssize_t forth_drv_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
	if (size != 1)
		return -EINVAL;

	/* 如果没有按键动作, 休眠 */
	wait_event_interruptible(button_waitq, ev_press);

	/* 如果有按键动作, 返回键值 */
	copy_to_user(buf, &key_val, 1);
	ev_press = 0;
	
	return 1;
}

int forth_drv_close(struct inode *inode, struct file *file)
{
	free_irq(IRQ_EINT(19), &pins_desc[0]);
	free_irq(IRQ_EINT(24), &pins_desc[1]);
	free_irq(IRQ_EINT(25), &pins_desc[2]);
	free_irq(IRQ_EINT(26), &pins_desc[3]);
	free_irq(IRQ_EINT(27), &pins_desc[4]);
	
	atomic_inc(&canopen); /* 释放设备 */
	up(&button_lock);  //释放打开锁
	return 0;
}

static unsigned forth_drv_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;
	poll_wait(file, &button_waitq, wait); // 不会立即休眠

	if (ev_press)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static int forth_drv_fasync(int fd, struct file *fp, int on)
{
	return fasync_helper(fd, fp, on, &fasync);
}

static struct file_operations sencod_drv_fops = {
	.owner   =  THIS_MODULE,    /* 这是一个宏，推向编译模块时自动创建的__this_module变量 */
	.open    =  forth_drv_open,     
	.read	 =	forth_drv_read,	   
	.release =  forth_drv_close,
	.poll    =  forth_drv_poll,
	.fasync  =  forth_drv_fasync,
};

int major;
static int forth_drv_init(void)
{
	major = register_chrdev(0, "forth_drv", &sencod_drv_fops);

	forthdrv_class = class_create(THIS_MODULE, "forth_drv");

	device_create(forthdrv_class, NULL, MKDEV(major, 0), NULL, "buttons"); /* /dev/buttons */

	gph2con = (volatile unsigned long *)ioremap(0xe0200c40, 16);
	gph2dat = gph2con + 1;

	gph3con = (volatile unsigned long *)ioremap(0xE0200C60, 16);
	gph3dat = gph3con + 1;

	return 0;
}

static void forth_drv_exit(void)
{
	unregister_chrdev(major, "forth_drv");
	device_destroy(forthdrv_class, MKDEV(major, 0));
	class_destroy(forthdrv_class);
	iounmap(gph2con);
	iounmap(gph3con);
}

module_init(forth_drv_init);
module_exit(forth_drv_exit);

MODULE_LICENSE("GPL");

