/*
 * net/l3mdev/l3mdev.c - L3 master device implementation
 * Copyright (c) 2015 Cumulus Networks
 * Copyright (c) 2015 David Ahern <dsa@cumulusnetworks.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/netdevice.h>
#include <net/fib_rules.h>
#include <net/l3mdev.h>

/**
 *	l3mdev_master_ifindex - get index of L3 master device
 *	@dev: targeted interface
 */

int l3mdev_master_ifindex_rcu(const struct net_device *dev)
{
	int ifindex = 0;

	if (!dev)
		return 0;

	if (netif_is_l3_master(dev)) {
		ifindex = dev->ifindex;
	} else if (netif_is_l3_slave(dev)) {
		struct net_device *master;
		struct net_device *_dev = (struct net_device *)dev;

		/* netdev_master_upper_dev_get_rcu calls
		 * list_first_or_null_rcu to walk the upper dev list.
		 * list_first_or_null_rcu does not handle a const arg. We aren't
		 * making changes, just want the master device from that list so
		 * typecast to remove the const
		 */
		master = netdev_master_upper_dev_get_rcu(_dev);
		if (master)
			ifindex = master->ifindex;
	}

	return ifindex;
}
EXPORT_SYMBOL_GPL(l3mdev_master_ifindex_rcu);

/**
 *	l3mdev_master_upper_ifindex_by_index - get index of upper l3 master
 *					       device
 *	@net: network namespace for device index lookup
 *	@ifindex: targeted interface
 */
int l3mdev_master_upper_ifindex_by_index_rcu(struct net *net, int ifindex)
{
	struct net_device *dev;

	dev = dev_get_by_index_rcu(net, ifindex);
	while (dev && !netif_is_l3_master(dev))
		dev = netdev_master_upper_dev_get_rcu(dev);

	return dev ? dev->ifindex : 0;
}
EXPORT_SYMBOL_GPL(l3mdev_master_upper_ifindex_by_index_rcu);

/**
 *	l3mdev_fib_table - get FIB table id associated with an L3
 *                             master interface
 *	@dev: targeted interface
 */

u32 l3mdev_fib_table_rcu(const struct net_device *dev)
{
	u32 tb_id = 0;

	if (!dev)
		return 0;

	if (netif_is_l3_master(dev)) {
		if (dev->l3mdev_ops->l3mdev_fib_table)
			tb_id = dev->l3mdev_ops->l3mdev_fib_table(dev);
	} else if (netif_is_l3_slave(dev)) {
		/* Users of netdev_master_upper_dev_get_rcu need non-const,
		 * but current inet_*type functions take a const
		 */
		struct net_device *_dev = (struct net_device *) dev;
		const struct net_device *master;

		master = netdev_master_upper_dev_get_rcu(_dev);
		if (master &&
		    master->l3mdev_ops->l3mdev_fib_table)
			tb_id = master->l3mdev_ops->l3mdev_fib_table(master);
	}

	return tb_id;
}
EXPORT_SYMBOL_GPL(l3mdev_fib_table_rcu);

u32 l3mdev_fib_table_by_index(struct net *net, int ifindex)
{
	struct net_device *dev;
	u32 tb_id = 0;

	if (!ifindex)
		return 0;

	rcu_read_lock();

	dev = dev_get_by_index_rcu(net, ifindex);
	if (dev)
		tb_id = l3mdev_fib_table_rcu(dev);

	rcu_read_unlock();

	return tb_id;
}
EXPORT_SYMBOL_GPL(l3mdev_fib_table_by_index);

/**
 *	l3mdev_link_scope_lookup - IPv6 route lookup based on flow for link
 *			     local and multicast addresses
 *	@net: network namespace for device index lookup
 *	@fl6: IPv6 flow struct for lookup
 */

struct dst_entry *l3mdev_link_scope_lookup(struct net *net,
					   struct flowi6 *fl6)
{
	struct dst_entry *dst = NULL;
	struct net_device *dev;

	if (fl6->flowi6_oif) {
		rcu_read_lock();

		dev = dev_get_by_index_rcu(net, fl6->flowi6_oif);
		if (dev && netif_is_l3_slave(dev))
			dev = netdev_master_upper_dev_get_rcu(dev);

		if (dev && netif_is_l3_master(dev) &&
		    dev->l3mdev_ops->l3mdev_link_scope_lookup)
			dst = dev->l3mdev_ops->l3mdev_link_scope_lookup(dev, fl6);

		rcu_read_unlock();
	}

	return dst;
}
EXPORT_SYMBOL_GPL(l3mdev_link_scope_lookup);

/**
 *	l3mdev_fib_rule_match - Determine if flowi references an
 *				L3 master device
 *	@net: network namespace for device index lookup
 *	@fl:  flow struct
 */

int l3mdev_fib_rule_match(struct net *net, struct flowi *fl,
			  struct fib_lookup_arg *arg)
{
	struct net_device *dev;
	int rc = 0;

	/* update flow ensures flowi_l3mdev is set when relevant */
	if (!fl->flowi_l3mdev)
		return 0;

	rcu_read_lock();

	dev = dev_get_by_index_rcu(net, fl->flowi_l3mdev);
	if (dev && netif_is_l3_master(dev) &&
	    dev->l3mdev_ops->l3mdev_fib_table) {
		arg->table = dev->l3mdev_ops->l3mdev_fib_table(dev);
		rc = 1;
	}

	rcu_read_unlock();

	return rc;
}

void l3mdev_update_flow(struct net *net, struct flowi *fl)
{
	struct net_device *dev;

	rcu_read_lock();

	if (fl->flowi_oif) {
		dev = dev_get_by_index_rcu(net, fl->flowi_oif);
		if (dev) {
			if (!fl->flowi_l3mdev)
				fl->flowi_l3mdev = l3mdev_master_ifindex_rcu(dev);

			/* oif set to L3mdev directs lookup to its table;
			 * reset to avoid oif match in fib_lookup
			 */
			if (netif_is_l3_master(dev))
				fl->flowi_oif = 0;
			goto out;
		}
	}

	if (fl->flowi_iif > LOOPBACK_IFINDEX && !fl->flowi_l3mdev) {
		dev = dev_get_by_index_rcu(net, fl->flowi_iif);
		if (dev)
			fl->flowi_l3mdev = l3mdev_master_ifindex_rcu(dev);
	}

out:
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(l3mdev_update_flow);
