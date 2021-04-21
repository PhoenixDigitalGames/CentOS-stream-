/* Copyright (c) 2017 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#include <stddef.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} test_map_id SEC(".maps");

SEC("raw_tp/sys_enter")
int test_obj_id(void *ctx)
{
	__u32 key = 0;
	__u64 *value;

	value = bpf_map_lookup_elem(&test_map_id, &key);

	return 0;
}
