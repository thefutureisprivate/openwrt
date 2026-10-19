# SPDX-License-Identifier: GPL-2.0-only

obj-m += vrx619_ep.o
obj-m += vrx619_pmi.o
obj-m += vrx619_dp.o
vrx619_pmi-y := vrx619_pmi_main.o vrx619_fw.o
vrx619_dp-y := vrx619_dp_main.o vrx619_atm.o vrx619_aca.o vrx619_aca_fw.o
ccflags-y += -I$(src)/include
