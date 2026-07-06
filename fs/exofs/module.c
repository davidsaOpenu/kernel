#include <linux/module.h>
#include "submodule.h"

extern const struct exofs_submodule exofs_fs_submodule;
extern const struct exofs_submodule exofs_sysfs_submodule;

static const struct exofs_submodule *exofs_submodules[] = {
	&exofs_fs_submodule,
};

static int exofs_init(void)
{
	int i = 0;
	int err = 0;

	for (; i < ARRAY_SIZE(exofs_submodules); ++i) {
		if (exofs_submodules[i]->init)
			err = exofs_submodules[i]->init();
		if (err)
			goto out_err;
	}

	return 0;

out_err:
	for (; i >= 0; --i) {
		if (exofs_submodules[i]->exit)
			exofs_submodules[i]->exit();
	}

	return err;
}

static void exofs_exit(void)
{
	int i;
	for (i = ARRAY_SIZE(exofs_submodules) - 1; i >= 0; --i) {
		if (exofs_submodules[i]->exit)
			exofs_submodules[i]->exit();
	}
}

MODULE_AUTHOR("Michael Somekh");
MODULE_DESCRIPTION("EXOFS filesystem (v2)");
MODULE_LICENSE("GPL");
module_init(exofs_init);
module_exit(exofs_exit);
