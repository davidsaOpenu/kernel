#pragma once

#define exofs_pr_debug(fmt, ...) pr_debug("EXOFS: " fmt, ##__VA_ARGS__)

#define exofs_pr_err(fmt, ...) pr_err("EXOFS: " fmt, ##__VA_ARGS__)
