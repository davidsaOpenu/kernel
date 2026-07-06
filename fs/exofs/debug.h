#pragma once

extern bool exofs_debug;

#define exofs_dbg(fmt, ...) do { \
    if (exofs_debug) \
        printk(KERN_DEBUG "EXOFS-DEBUG: " fmt, ##__VA_ARGS__); \
} while (0)

#define exofs_msg(sb, level, fmt, ...) do { \
    printk(level "EXOFS (%s): " fmt, (sb)->s_id, ##__VA_ARGS__); \
} while (0)
