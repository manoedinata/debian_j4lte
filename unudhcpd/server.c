// Copyright 2021 Clayton Craft <clayton@craftyguy.net>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <errno.h>
#include <net/if_arp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"

// Convert 48-bit ethernet MAC to a pretty string
char *mac_to_str(uint8_t mac[6]) {
	static char str[20];
	snprintf(str, 20, "%.2x:%.2x:%.2x:%.2x:%.2x:%.2x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return str;
}

int add_arp_entry(dhcp_config *config, uint8_t *mac, int mac_len, uint32_t ip) {
	struct arpreq areq;
	memset(&areq, 0, sizeof(areq));

	strncpy(areq.arp_dev, config->iface, sizeof(areq.arp_dev));

	struct sockaddr_in *sock_addr = (struct sockaddr_in *) &areq.arp_pa;
	sock_addr->sin_family = AF_INET;
	sock_addr->sin_addr.s_addr = ip;

	memcpy(areq.arp_ha.sa_data, mac, mac_len);
	// completed ARP entry
	areq.arp_flags = ATF_COM;

	if (ioctl(config->server_sock, SIOCSARP, (char *)&areq) < 0)  {
		perror("Unable to add entry to ARP table");
		return 1;
	}    
	return 0;
}

int dhcp_create_response(dhcp_config *config, dhcp_message *request, dhcp_message *response, unsigned char type) {
	response->op = BOOTREPLY;
	response->htype = request->htype;
	response->hlen = request->hlen;
	memcpy(&response->xid, &request->xid, sizeof(request->xid));
	memcpy(&response->flags, &request->flags, sizeof(request->flags));
	memcpy(&response->giaddr, &request->giaddr, sizeof(request->giaddr));
	memcpy(response->chaddr, request->chaddr, request->hlen);
	memset(&response->flags, 0, sizeof(response->flags));

	memset(&response->options, 0, sizeof(response->options));

	dhcp_response_options options = {0};

	// DHCP magic cookie
	memcpy(&options.magic, &dhcp_option_magic, sizeof(options.magic));

	// DHCP message type
	options.msg_type_option = DHCP_MESSAGE_TYPE;
	options.msg_type_len = 1;
	options.msg_type_val = type;

	// server ID
	struct sockaddr_in server_addr;
	if (inet_aton(config->server_ip, &server_addr.sin_addr) == 0) {
		return 1;
	}
	options.server_id_option = SERVER_IDENTIFIER;
	options.server_id_len = 4;
	memcpy(&options.server_id_val, &server_addr.sin_addr, 4);

	// subnet mask
	struct sockaddr_in subnet_addr;
	if (inet_aton("255.255.255.0", &subnet_addr.sin_addr) == 0) {
		return 1;
	}
	options.subnet_option = DHCP_OPTION_SUBNET;
	options.subnet_len = 4;
	memcpy(&options.subnet_val, &subnet_addr.sin_addr, 4);

	// lease time
	options.lease_option = DHCP_OPTION_LEASE;
	options.lease_len = 4;
	// set arbitrarily to 42 days. making it too high might(?) mess with
	// some clients(?). server ignores it anyways.
	uint8_t lease_time[4] = {0x00, 0x37, 0x5f, 0x00};
	memcpy(&options.lease_val, &lease_time, 4);

	// end
	options.end_option = 0xff;

	memcpy(&response->options, &options, sizeof(options));

	struct sockaddr_in client_addr;
	if (inet_aton(config->client_ip, &client_addr.sin_addr) == 0) {
		return 1;
	}
	memcpy(&response->yiaddr, &client_addr.sin_addr, sizeof(response->yiaddr));

	return 0;
}

int dhcp_send_response(dhcp_config *config, dhcp_message *response, struct sockaddr_in *client_addr) {
	int len = sizeof(*response);

	client_addr->sin_addr.s_addr = inet_addr(config->client_ip);
	if (add_arp_entry(config, response->chaddr, response->hlen, response->yiaddr) != 0) {
		return 1;
	}
	if ((sendto(config->server_sock, response, len, 0, (struct sockaddr *)client_addr, sizeof(*client_addr))) < 0) {
		perror("Unable to send DHCP response");
		return 1;
	}

	return 0;
}

int dhcp_handle_discover(dhcp_config *config, dhcp_message *request, dhcp_message *response, struct sockaddr_in *client_addr) {
	if (dhcp_create_response(config, request, response, DHCP_OFFER) != 0) {
		return 1;
	}
	if (dhcp_send_response(config, response, client_addr) != 0)
		return 1;

	return 0;
}

int dhcp_handle_request(dhcp_config *config, dhcp_message *request, dhcp_message *response, struct sockaddr_in *client_addr) {
	if (dhcp_create_response(config, request, response, DHCP_ACK) != 0) {
		return 1;
	}
	if (dhcp_send_response(config, response, client_addr) != 0)
		return 1;

	return 0;
}

int dhcp_server_init(dhcp_config *config) {
	// UDP's protocol number is a fixed constant (17); it was previously
	// looked up at runtime via getprotobyname("udp"), but that goes
	// through NSS, and NSS-backed libc calls in a statically linked
	// binary try to dlopen the NSS plugin matching the *runtime* glibc -
	// which segfaults outright when it doesn't match the glibc this was
	// linked against (see j4-debian-attempt's build_unudhcpd.sh).
	if ((config->server_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		perror("Unable to open UDP socket");
		goto ERROR;
	}

	int so_reuseaddr = 1;
	setsockopt(config->server_sock, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr));

	memset(&config->server_addr, 0, sizeof(config->server_addr));
	config->server_addr.sin_family = AF_INET;
	config->server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	config->server_addr.sin_port = htons(config->server_port);

	printf("Trying to bind to interface: %s\n", config->iface);
	if (setsockopt(config->server_sock, SOL_SOCKET, SO_BINDTODEVICE, config->iface, strlen(config->iface)) != 0) {
		perror("Unable to bind to interface");
		goto ERROR;
	}

	if (bind(config->server_sock, (struct sockaddr*)&config->server_addr, sizeof(config->server_addr)) < 0)
	{
		perror("Unable to bind to UDP socket");
		goto ERROR;
	}

	return 0;

ERROR:
	if (config->server_sock >= 0)
		close(config->server_sock);
	return 1;
}

int dhcp_is_invalid_request(dhcp_message *request, ssize_t request_len) {
	if (request_len < DHCP_MESSAGE_SIZE_MIN || request_len > DHCP_MESSAGE_SIZE_MAX)
		return 1;

	// 1 == ethernet, from RFC 1700, "Hardware Type" table
	if (request->htype != 1) {
		printf("Received request for unsupported hardware type: %d\n", request->htype);
		return 1;
	}

	if (request->op != BOOTREQUEST)
		return 1;

	uint8_t magic[4] = {
		request->options[0],
		request->options[1],
		request->options[2],
		request->options[3]};
	if (memcmp(&magic, &dhcp_option_magic, sizeof(magic)) != 0)
		return 1;

	return 0;
}

int dhcp_get_request_type(dhcp_message *request, ssize_t request_len) {

	int option_len = request_len - DHCP_HEADER_SIZE;
	int idx = 4;
	int len;

	// idx + 3, since this is can access up to 3 bytes beyond the idx value
	// for the request type. The check must be changed if more bytes need
	// to be accessed, to ensure no overflow.
	while (request->options[idx] != 0xFF && idx + 3 <= option_len) {
		if (request->options[idx] == DHCP_MESSAGE_TYPE) {
			if (request->options[idx+2] == DHCP_DISCOVER) {
				return DHCP_DISCOVER;
			}
			if (request->options[idx+2] == DHCP_REQUEST) {
				return DHCP_REQUEST;
			}
			len = (int)request->options[idx+1];
			idx += len + 2;
		} else if (request->options[idx] == 0) {
			// padding
			idx++;
		} else {
			// some unsupported option type
			len = (int)request->options[idx+1];
			idx += len + 2;
		}
	}

	return -1;
}

int dhcp_server_handle_receive(dhcp_config *config, dhcp_message *request, ssize_t request_len,
		dhcp_message *response, struct sockaddr_in *client_addr) {

	if (dhcp_is_invalid_request(request, request_len))
		return 0;

	switch (dhcp_get_request_type(request, request_len)) {
	case DHCP_DISCOVER:
		if (request->hlen == 6)
			printf("Received DHCP DISCOVER from client: %s\n", mac_to_str(request->chaddr));
		dhcp_handle_discover(config, request, response, client_addr);
		break;
	case DHCP_REQUEST:
		if (request->hlen == 6)
			printf("Received DHCP REQUEST from client: %s\n", mac_to_str(request->chaddr));
		dhcp_handle_request(config, request, response, client_addr);
		break;
	default:
		return 0;
	}

	return 0;
}

int dhcp_server_start(dhcp_config *config){
	printf("Trying to start server with parameters: ");
	printf("Server IP addr: %s:%d, client IP addr: %s, interface: %s\n", config->server_ip, config->server_port,
		config->client_ip, config->iface);
	if (dhcp_server_init(config) != 0) {
		return 1;
	}
	printf("Server started!\n");

	for (;;) {
		dhcp_message request = {0};
		dhcp_message response = {0};
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		memset(&client_addr, 0, addr_len);
		ssize_t recv_len;

		recv_len = recvfrom(config->server_sock, &request, sizeof(request), 0, (struct sockaddr *)&client_addr, &addr_len);
		if (recv_len < 0) {
			perror("Unable to receive from socket");
			close(config->server_sock);
			return 1;
		}

		if (dhcp_server_handle_receive(config, &request, recv_len, &response, &client_addr)) {
			fprintf(stderr, "failed to handle received request!\n");
		}
	}
	close(config->server_sock);
	return 0;
}
