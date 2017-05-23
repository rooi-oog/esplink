#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <espressif/esp_common.h>
#include <esp/uart.h>
#include <esp8266.h>

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

#include <dhcpserver.h>
#include <lwip/api.h>

#define SWCLK				0
#define SWDIO				2

#define AP_SSID 			"uart_bridge AP"
#define AP_PSK 				"uart_bridge"
#define BRIDGE_PORT			20001
#define OPENOCD_PORT 		7777

#define MAX_BUFFER_SIZE		2053

#define IDLE_ST				0
#define	LENGTH_ST			1
#define	PAYLOAD_ST			2

static char tx_buf [MAX_BUFFER_SIZE];
volatile static char task_error;

void sdk_hostap_handle_timer(void *cnx_node) {}

IRAM void openocd_handler (void *pvParameters)
{
	struct netconn *nc = (struct netconn *) pvParameters;
	struct netbuf *rbuf = NULL;
	char *rx_buf;
	char d, r;
	err_t err;
	uint16_t len;
	uint16_t i;
	
	gpio_set_pullup (SWCLK, false, false);
	gpio_set_pullup (SWDIO, false, false);
	gpio_enable (SWCLK, GPIO_OUTPUT);
	gpio_enable (SWDIO, GPIO_OUTPUT);
	
	while (1)
	{
		if ((err = netconn_recv (nc, &rbuf)) != ERR_OK) {
			printf ("R ERROR %d\n", err);
			return;
		}
		
		netbuf_data (rbuf, (void **) &rx_buf, &len);	
		
		for (i = 0; i < len; i++)
		{
			switch (rx_buf [i])
			{
			case 'Q':		// Quit
				netconn_disconnect (nc);
				return;
			
			case '0'...'7':
				d = rx_buf [i] - '0';
				gpio_write (SWDIO, (d & 0x1));
				gpio_write (SWCLK, !!(d & 0x4));								
				break;
			
			case 'i':
				gpio_enable (SWDIO, GPIO_INPUT);
				break;
			
			case 'o':
				gpio_enable (SWDIO, GPIO_OUTPUT);
				break;
			
			case 'R':		// Writeback
				r = ((char) gpio_read (SWDIO)) + '0';
				netconn_write (nc, &r, 1, NETCONN_COPY);
				break; 
			}
		}
		
		netbuf_delete (rbuf);
	}
}

void openocd_serverTask (void *pvParameters)
{
	struct netconn *nc;
	struct netconn *client = NULL;
	err_t err;
	
	if (!(nc = netconn_new (NETCONN_TCP))) 
	{
		printf ("Status monitor: Failed to allocate socket.\n");
		return;
	}
	
	netconn_bind (nc, IP_ADDR_ANY, OPENOCD_PORT);
	netconn_listen (nc);
	
	while (1)
	{
		
		if (client)
		{
			netconn_delete (client);
			client = NULL;
		}
		
		if ((err = netconn_accept (nc, &client)) != ERR_OK)
		{
			printf ("accept error: %d\n", err);
			continue;
		}
		
		openocd_handler ((void *) client);
	}
}

IRAM void fromUartToNet (void *pvParameters)
{
	struct netconn *nc = (struct netconn *) pvParameters;
	struct netbuf *rbuf = NULL;
	char *rx_buf;
	err_t err;
	uint16_t len;
	
	while (1)
	{
		if ((err = netconn_recv (nc, &rbuf)) != ERR_OK)
		{
			printf ("R ERROR %d\n", err);
			task_error = err;
			while (1);
		}
		
		netbuf_data (rbuf, (void **) &rx_buf, &len);
		write (1, rx_buf, len);
		netbuf_delete (rbuf);
	}
}

IRAM void fromNetToUart (void *pvParameters)
{
	struct netconn *nc = (struct netconn *) pvParameters;
	uint8_t state = IDLE_ST;
	uint16_t len = 0;
	err_t err;
	
	while (1)
	{		
		switch (state)
		{
		case IDLE_ST:
			read (0, tx_buf, 1);
			if (tx_buf [0] == 'W') 
				state = LENGTH_ST;
			else if (tx_buf [0] == 'F') {
				len = 2048;
				state = PAYLOAD_ST;
			}
			break;
			
		case LENGTH_ST:
			read (0, &tx_buf [1], 4);
			state = PAYLOAD_ST;
			tx_buf [5] = 0;
			len = atoi (&tx_buf [1]);
			break;
			
		case PAYLOAD_ST:
			read (0, &tx_buf [5], len);
			state = IDLE_ST;				
			
			if (tx_buf [0] == 'W') {
				tx_buf [0] = 'R';
				err = netconn_write (nc, tx_buf, len + 5, NETCONN_COPY);
			} 
			else
				err = netconn_write (nc, &tx_buf [5], len, NETCONN_COPY);
				
			if (err != ERR_OK)
			{
				printf ("W ERROR %d\n", err);
				task_error = err;
				while (1);
			}
			else
				write (1, ">", 1);
		}
	}
}

void serverTask (void *pvParameters)
{
	struct netconn *nc;
	struct netconn *client = NULL;
	struct ip_info ap_ip;
	struct sdk_softap_config ap_config = {
		.ssid = AP_SSID,
		.ssid_hidden = 0,
		.channel = 3,
		.ssid_len = strlen (AP_SSID),
		.authmode = AUTH_WPA_WPA2_PSK,
		.password = AP_PSK,
		.max_connection = 3,
		.beacon_interval = 100,
	};
	ip_addr_t first_client_ip;
	err_t err;
	
	TaskHandle_t h_fromUartToNet = NULL;
	TaskHandle_t h_fromNetToUart = NULL;
	
	sdk_wifi_set_opmode (SOFTAP_MODE);    			
	IP4_ADDR (&ap_ip.ip, 172, 16, 0, 1);
	IP4_ADDR (&ap_ip.gw, 0, 0, 0, 0);
	IP4_ADDR (&ap_ip.netmask, 255, 255, 255, 0);
	
	sdk_wifi_set_ip_info (1, &ap_ip);				
	sdk_wifi_softap_set_config (&ap_config);
	
	IP4_ADDR (&first_client_ip, 172, 16, 0, 2);
	dhcpserver_start(&first_client_ip, 4);
	
	if (!(nc = netconn_new (NETCONN_TCP))) 
	{
		printf ("Status monitor: Failed to allocate socket.\n");
		return;
	}
	
	netconn_bind (nc, IP_ADDR_ANY, BRIDGE_PORT);
	netconn_listen (nc);
	
	while (1)
	{
		if (client) {
			netconn_delete (client);
			client = NULL;
		}
		
		if ((err = netconn_accept (nc, &client)) != ERR_OK) {
			printf ("accept error: %d\n", err);
			continue;
		}
		
		xTaskCreate (fromUartToNet, "fromUartToNet", 512, (void *) client, 2, &h_fromUartToNet);
		xTaskCreate (fromNetToUart, "fromNetToUart", 512, (void *) client, 2, &h_fromNetToUart);		
		
		while (1) {
			vTaskDelay (100);
			
			if (task_error) {
				vTaskDelete (h_fromUartToNet);
				vTaskDelete (h_fromNetToUart);				
				task_error = ERR_OK;
				break;
			}			
		}
	}
}

void user_init (void)
{		
/*	sdk_system_update_cpu_freq (160);*/
/*	sdk_os_delay_us (1000);*/
	
	uart_set_baud(0, 115200);
	sdk_os_delay_us (100);
	printf ("READY\n");
	
	xTaskCreate (serverTask, "serverTask", 512, NULL, 2, NULL);	
	xTaskCreate (openocd_serverTask, "serverTask", 512, NULL, 2, NULL);
}


