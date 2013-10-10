#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>

static int __init hwln_init(void)
{
	printk("hwln_init\n");
	return 0;
}

static void __exit hwln_exit(void)
{
	printk("hwln_exit\n");
}

module_init(hwln_init);
module_exit(hwln_exit);

MODULE_LICENSE("GPL");
