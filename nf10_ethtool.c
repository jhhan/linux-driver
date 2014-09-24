/*******************************************************************************
*
*  NetFPGA-10G http://www.netfpga.org
*
*  File:
*        nf10_ethtool.c
*
*  Project:
*
*
*  Author:
*        Hwanju Kim
*
*  Description:
*	 This module provides the implementation of ethtool.
*	 It began providing only get/set_msglevel for debugging purpose, and
*	 will be extended while adding some parameter controls and offloading
*	 features.
*
*        TODO: 
*		- Parameter control to talk with DMA hardware
*		- Standard offloading control such as gso/gro
*
*	 This code is initially developed for the Network-as-a-Service (NaaS) project.
*        
*
*  Copyright notice:
*        Copyright (C) 2014 University of Cambridge
*
*  Licence:
*        This file is part of the NetFPGA 10G development base package.
*
*        This file is free code: you can redistribute it and/or modify it under
*        the terms of the GNU Lesser General Public License version 2.1 as
*        published by the Free Software Foundation.
*
*        This package is distributed in the hope that it will be useful, but
*        WITHOUT ANY WARRANTY; without even the implied warranty of
*        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*        Lesser General Public License for more details.
*
*        You should have received a copy of the GNU Lesser General Public
*        License along with the NetFPGA source package.  If not, see
*        http://www.gnu.org/licenses/.
*
*/

#include "nf10.h"

static u32 nf10_get_msglevel(struct net_device *netdev)
{
	struct nf10_adapter *adapter = netdev_adapter(netdev);
	return adapter->msg_enable;
}

static void nf10_set_msglevel(struct net_device *netdev, u32 data)
{
	struct nf10_adapter *adapter = netdev_adapter(netdev);
	adapter->msg_enable = data;
}

static const struct ethtool_ops nf10_ethtool_ops = {
	.get_msglevel           = nf10_get_msglevel,
	.set_msglevel           = nf10_set_msglevel,
};

void nf10_set_ethtool_ops(struct net_device *netdev)
{
	SET_ETHTOOL_OPS(netdev, &nf10_ethtool_ops);
}