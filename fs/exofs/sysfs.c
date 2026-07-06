#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sysctl.h>

#include "sysfs.h"
#include "submodule.h"

bool exofs_debug = false;

static struct ctl_table exofs_sysctl_table[] = {
	{
		.procname = "debug",
		.data = &exofs_debug,
		.maxlen = sizeof(exofs_debug),
		.mode = 0644,
		.proc_handler = proc_dobool,
	},
};

static struct ctl_table_header *exofs_sysctl_header;

static int exofs_sysfs_init(void)
{
	exofs_sysctl_header = register_sysctl("fs/exofs", exofs_sysctl_table);

	if (!exofs_sysctl_header)
		return -ENOMEM;

	return 0;
}

static void exofs_sysfs_exit(void)
{
	unregister_sysctl_table(exofs_sysctl_header);
}

struct exofs_submodule exofs_sysfs_submodule = {
	.init = exofs_sysfs_init,
	.exit = exofs_sysfs_exit,
};
