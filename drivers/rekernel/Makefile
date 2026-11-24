# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_REKERNEL) += rekernel.o
rekernel-y := rekernel_main.o rekernel_netlink.o rekernel_binder.o rekernel_signal.o rekernel_netfilter.o

ccflags-$(CONFIG_REKERNEL_LEGACY_NETLINK) += -DLEGACY_NETLINK
ifdef REKERNEL_VERSION
ccflags-y += -DREKERNEL_VERSION=\"$(REKERNEL_VERSION)\"
endif
