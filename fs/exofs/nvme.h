#pragma once

#include <linux/types.h>

struct nvme_ns;

extern ssize_t nvme_submit_sync_kv_cmd(struct nvme_ns *ns, u8 opcode, 
	u64 key_low, u64 key_high, u8 key_length, 
	u32 offset, void *buffer, unsigned bufflen);

extern ssize_t nvme_submit_sync_kv_read(struct nvme_ns* ns, uint64_t key, void *buffer, unsigned bufflen, unsigned off);
extern ssize_t nvme_submit_sync_kv_write(struct nvme_ns* ns, uint64_t key, const void *buffer, unsigned bufflen, unsigned off);
extern int nvme_submit_sync_kv_delete(struct nvme_ns* ns, uint64_t key);
extern int nvme_submit_sync_kv_exists(struct nvme_ns* ns, uint64_t key);

