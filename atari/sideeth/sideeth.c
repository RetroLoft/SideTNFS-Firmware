/*********************************************************************/
/*                                                                   */
/*  SideETH.STX — STinG Ethernet port over SideTNFS cartridge        */
/*                                                                   */
/*  Template: th-otto/STinG ports/nfeth (poll-mode, no NatFeat IRQ)  */
/*  Transport: ROM3 shared memory + GEMDRV SideETH commands          */
/*  See ../../docs/sideeth-protocol.md                               */
/*                                                                   */
/*********************************************************************/

#include <stdio.h>
#include <string.h>
#include <osbind.h>

#include "transprt.h"
#include "port.h"

#include "sideeth.h"
#include "cart.h"

#define M_YEAR    2026
#define M_MONTH   9
#define M_DAY     25
#define M_VERSION "21.00"

#ifdef __GNUC__
#define _BasPag _base
extern unsigned long _PgmSize;
#endif

TPL *tpl;
STX *stx;

static MYPORT my_port = {
	{
		"SideETH",
		L_SER_BUS,
		FALSE,
		0,
		0,
		0xffffffffUL,
		576,  /* mtu — matches Pico SIDEETH_MTU */
		576,
		0,
		NULL,
		0,
		NULL,
		0,
		NULL,
		NULL
	},
	{ 0x02, 0x53, 0x49, 0x44, 0x45, 0x01 },
	0
};

#define BUFFER_SIZE 1536L
static uint8 tx_buffer[BUFFER_SIZE];
static uint8 rx_buffer[BUFFER_SIZE];

static char *hardware[] = { "SideTNFS", NULL };
static int16 ctrl_type = 0;

/* ---- cookie / STinG glue ------------------------------------------------ */

static long get_jar(void)
{
	return *((long *)0x5a0L);
}

static int get_cookie(long which, long *value)
{
	long *work;

	*value = 0;
	work = (long *)Supexec(get_jar);
	if (work)
	{
		for (; *work != 0; work += 2)
			if (*work == which)
			{
				*value = *++work;
				return TRUE;
			}
	}
	return FALSE;
}

/* ---- cart TX used by ARP reply path in tools.c -------------------------- */

void send_block(int unused, void *cp, short len)
{
	(void)unused;
	if (cp == NULL || len <= 0)
		return;
	(void)cart_tx_frame((const uint8 *)cp, (uint16)len);
}

/* ---- STinG driver callbacks --------------------------------------------- */

static void cdecl my_send(PORT *_port)
{
	MYPORT *port = (MYPORT *)_port;
	IP_DGRAM *dgram;
	int16 length;

	if (port != &my_port || port->generic.active == 0)
		return;

#ifdef SIDEETH_NO_TX
	(void)dgram;
	(void)length;
#else
	/* One queued frame per callback; never consume a datagram when busy. */
	if (!cart_tx_ready()) return;
	if (fetch_dgram(port, &dgram) == FALSE) return;
	length = xmit_dgram(port, dgram, tx_buffer);
	if (length != 0) send_block(0, tx_buffer, length);

#endif
}

static void recv_pending(MYPORT *port)
{
    int16 len=cart_receive_frame(rx_buffer, sizeof(rx_buffer));
    if(len>0)recve_dgram(port,rx_buffer,len);
}

static void cdecl my_receive(PORT *_port)
{
	MYPORT *port = (MYPORT *)_port;

	if (port != &my_port || port->generic.active == 0)
		return;
#ifndef SIDEETH_NO_RX
	recv_pending(port);
#endif
}

static int16 cdecl my_set_state(PORT *_port, int16 state)
{
	MYPORT *port = (MYPORT *)_port;

	if (port != &my_port)
		return FALSE;

	if (state)
	{
		(void)Cconws("\r\nSIDEETH: enabling STinG interface...\r\n");
		if (cart_cmd_u16(SIDEETH_CMD_SET_STATE, 1) != 0)
			return FALSE;
		if (cart_cmd(SIDEETH_CMD_GET_INFO, NULL, 0) != 0)
			return FALSE;

		{
			uint32 magic = cart_read_u32_swapped(SIDEETH_OFF_MAGIC);
			if (magic != SIDEETH_MAGIC)
				return FALSE;
		}

		cart_read_bytes(SIDEETH_OFF_MAC, port->address, ETH_ALEN);
		port->generic.ip_addr = cart_read_u32_swapped(SIDEETH_OFF_ATARI_IP);
		port->generic.sub_mask = cart_read_u32_swapped(SIDEETH_OFF_NETMASK);
		port->gateway = cart_read_u32_swapped(SIDEETH_OFF_GATEWAY);
		port->generic.mtu = cart_read_u16(SIDEETH_OFF_MTU);
		if (port->generic.mtu == 0 || port->generic.mtu > 1500)
			port->generic.mtu = 576;
		port->generic.max_mtu = port->generic.mtu;

		arp_init();
		(void)Cconws("SIDEETH F21: NAT Interface UP\r\n");
	}
	else
	{
		if (cart_cmd_u16(SIDEETH_CMD_SET_STATE, 0) != 0)
			return FALSE;
		deplete_queue(&port->generic.send);
		deplete_queue(&port->generic.receive);
	}

	return TRUE;
}

static int16 cdecl my_cntrl(PORT *_port, uint32 argument, int16 code)
{
	MYPORT *port = (MYPORT *)_port;
	int16 result = E_NORMAL;

	if (port != &my_port)
		return E_PARAMETER;

	switch (code)
	{
	case CTL_ETHER_GET_MAC:
		memcpy((void *)argument, port->address, ETH_ALEN);
		break;
	case CTL_ETHER_SET_MAC:
		if (memcmp(port->address, (void *)argument, ETH_ALEN) != 0)
			result = E_LOCKED;
		break;
	case CTL_ETHER_INQ_SUPPTYPE:
		*((char ***)argument) = hardware;
		break;
	case CTL_ETHER_SET_TYPE:
		ctrl_type = (int16)(argument & 7);
		if (ctrl_type != 0)
			result = E_PARAMETER;
		break;
	case CTL_ETHER_GET_TYPE:
		*((int16 *)argument) = ctrl_type;
		break;
	case CTL_GENERIC_SET_IP:
		/* Fixed NAT address from Pico — allow only the advertised value. */
		if (argument != port->generic.ip_addr)
			result = E_LOCKED;
		break;
	case CTL_GENERIC_SET_MASK:
		if (argument != port->generic.sub_mask)
			result = E_LOCKED;
		break;
	default:
		result = E_FNAVAIL;
		break;
	}
	return result;
}

static DRIVER my_driver = {
	my_set_state,
	my_cntrl,
	my_send,
	my_receive,
	"SideETH",
	M_VERSION,
	((M_YEAR - 1980) << 9) | (M_MONTH << 5) | M_DAY,
	"RetroLoft / SideTNFS",
	NULL,
	NULL
};

static void failure(const char *reason)
{
	(void)Cconws("\r\nSIDEETH.STX : ");
	(void)Cconws(reason);
	(void)Cconws("\r\n");
}

static void failure_magic(uint32 magic)
{
	char buf[48];
	static const char hex[] = "0123456789ABCDEF";
	int i;

	(void)Cconws("\r\nSIDEETH.STX : magic mismatch (got ");
	for (i = 7; i >= 0; i--)
		buf[7 - i] = hex[(magic >> (i * 4)) & 0xF];
	buf[8] = '\0';
	(void)Cconws(buf);
	(void)Cconws(", want SETH) — update firmware?\r\n");
}

static int install(void)
{
	PORT *ports;
	DRIVER *driver;
	uint32 magic;

	if (cart_cmd(SIDEETH_CMD_GET_INFO, NULL, 0) != 0)
	{
		failure("SideTNFS SideETH not responding");
		return FALSE;
	}
	magic = cart_read_u32_swapped(SIDEETH_OFF_MAGIC);
	if (magic != SIDEETH_MAGIC)
	{
		failure_magic(magic);
		return FALSE;
	}

	cart_read_bytes(SIDEETH_OFF_MAC, my_port.address, ETH_ALEN);
	my_port.generic.ip_addr = cart_read_u32_swapped(SIDEETH_OFF_ATARI_IP);
	my_port.generic.sub_mask = cart_read_u32_swapped(SIDEETH_OFF_NETMASK);
	my_port.gateway = cart_read_u32_swapped(SIDEETH_OFF_GATEWAY);
	my_port.generic.mtu = cart_read_u16(SIDEETH_OFF_MTU);
	if (my_port.generic.mtu == 0)
		my_port.generic.mtu = 576;
	my_port.generic.max_mtu = my_port.generic.mtu;

	query_chains(&ports, &driver, NULL);
	while (ports->next)
		ports = ports->next;
	ports->next = &my_port.generic;
	my_port.generic.driver = &my_driver;

	while (driver->next)
		driver = driver->next;
	driver->next = &my_driver;
	my_driver.basepage = _BasPag;

	return TRUE;
}

int main(void)
{
	DRV_LIST *sting_drivers;

	if (strcmp(_BasPag->p_cmdlin, "\012STinG_Load") != 0)
	{
		failure("STinG extension module. Only to be started by STinG");
		return 1;
	}

	if (!get_cookie(STIK_COOKIE_MAGIC, (long *)&sting_drivers) || sting_drivers == 0)
		return 1;
	if (strcmp(sting_drivers->magic, STIK_DRVR_MAGIC) != 0)
		return 1;

	tpl = (TPL *)(*sting_drivers->get_dftab)(TRANSPORT_DRIVER);
	stx = (STX *)(*sting_drivers->get_dftab)(MODULE_DRIVER);

	if (tpl != NULL && stx != NULL)
	{
		if (install())
			Ptermres(_PgmSize, 0);
	}
	return 1;
}
