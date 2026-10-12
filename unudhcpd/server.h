// Copyright 2021 Clayton Craft <clayton@craftyguy.net>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>
#include <netinet/in.h>

// Bootp message types
#define BOOTREQUEST 1
#define BOOTREPLY 2

// DHCP Message types
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_HOSTNAME 12

#define DHCP_MESSAGE_TYPE 53
#define SERVER_IDENTIFIER 54

// DHCP Option codes
#define DHCP_OPTION_SUBNET 1
#define DHCP_OPTION_LEASE 51

#define DHCP_HEADER_SIZE 236
// Minimum size for a DHCP DISCOVER/REQUEST message seems to be:
// 244 bytes = DHCP Header (236 bytes) + DHCP magic (4) + type (1) + length (1) + message (1) + 0xFF
#define DHCP_MESSAGE_SIZE_MIN 244
// 576 is max per RFC 2131 pg. 10, for un-extended message
#define DHCP_MESSAGE_SIZE_MAX  576

static const uint8_t dhcp_option_magic[4] = {0x63, 0x82, 0x53, 0x63};

// From: https://datatracker.ietf.org/doc/html/rfc2131#page-37
typedef struct dhcp_message {
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t chaddr[16];
	uint32_t sname[16];
	uint32_t file[32];
	// 340 = DHCP_MESSAGE_SIZE_MAX - DHCP_HEADER_SIZE
	uint8_t options[340];
} dhcp_message;

typedef struct dhcp_config {
        char *iface;
        char *client_ip;
        char *server_ip;
        int server_port;
        int server_sock;
        struct sockaddr_in server_addr;
} dhcp_config;

// Only options used for DHCP OFFER/ACK
typedef struct dhcp_response_options {
	// DHCP magic cookie
	uint8_t magic[4];
        // DHCP message type
        uint8_t msg_type_option;
        uint8_t msg_type_len;
        uint8_t msg_type_val;
        // Server ID
        uint8_t server_id_option;
        uint8_t server_id_len;
        uint8_t server_id_val[4];
        // Subnet mask
        uint8_t subnet_option;
        uint8_t subnet_len;
        uint8_t subnet_val[4];
	// Lease time
        uint8_t lease_option;
        uint8_t lease_len;
        uint8_t lease_val[4];
        // End
        uint8_t end_option;
} dhcp_response_options;

int dhcp_server_start(dhcp_config *config);
