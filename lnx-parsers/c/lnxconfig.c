/*
 * lnxconfig.c - C lnx parser
 *
 * This file constains the public API and structs representing an lnx
 * file.  For an overview of how to use this parser, see demo.c.
 *
 * NOTE: This parser uses the "list.h" linked-list implementation
 * provided in the c-utils repo.  If you are using different list.h,
 * you may need to rename the list.h here to avoid conflicts.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>
#include <arpa/inet.h>

#include "list.h"
#include "lnxconfig.h"

#define TOKEN_MAX_INTERFACE 5
#define TOKEN_MAX_NEIGHBOR 4
#define TOKEN_MAX_RIP_NEIGHBOR 1
#define TOKEN_MAX_ROUTE 3
#define TOKEN_MAX_NAME 64
#define TOKEN_MAX_NUMBER 1

#define DEFAULT_RIP_PERIODIC_UPDATE_RATE_MS 5000
#define DEFAULT_RIP_TIMEOUT_THRESHOLD_MS 12000
#define DEFAULT_TCP_RTO_MIN_US 1000
#define DEFAULT_TCP_RTO_MAX_US 5000000

int g_current_line = 0;

void do_abort(char *msg);
void do_parse_error(char *msg);
void parse_addr(char *ip_str, struct in_addr *addr);

// Get a struct in_addr from a string
void parse_addr(char *ip_str, struct in_addr *addr)
{
  memset(addr, 0, sizeof(struct in_addr));

  int rv;
  if ((rv = inet_pton(AF_INET, ip_str, addr)) < 0) {
      do_abort("inet_pton");
  }
}

// Add a struct to one of the linked lists
#define add_config(config_field, template, T)	\
    do { \
	T *__item = (T *)malloc(sizeof(T));		  \
	memcpy(__item, template, sizeof(T));		  \
	list_insert_tail((config_field), &__item->link);  \
    } while(0)

// Free all ements in one of the lists
#define config_clear(config_field, T) \
    do { \
	while(!list_empty((config_field))) {		\
	    T *__node = list_tail((config_field), T, link);	\
	    list_remove_tail((config_field));			\
	    free(__node);					\
	}							\
    } while(0)


lnxconfig_t *lnxconfig_parse(char *config_file) {
    FILE *f;
    lnxconfig_t *config;

    char buf[LINE_MAX];
    char *line;
    int tokens;

    int port;
    char ip_buf1[LINE_MAX];
    char ip_buf2[LINE_MAX];
    char first_token[TOKEN_MAX_NAME];

    if ((f = fopen(config_file, "r")) == NULL) {
	perror("fopen");
	exit(1);
    }

    config = (lnxconfig_t *)malloc(sizeof(lnxconfig_t));
    memset(config, 0, sizeof(lnxconfig_t));
    list_init(&config->interfaces);
    list_init(&config->neighbors);
    list_init(&config->rip_neighbors);
    list_init(&config->static_routes);

    // Set default config values
    config->routing_mode = ROUTING_MODE_STATIC; // Set as default unless otherwise specified
    config->rip_periodic_update_rate_ms = DEFAULT_RIP_PERIODIC_UPDATE_RATE_MS;
    config->rip_timeout_threshold_ms = DEFAULT_RIP_TIMEOUT_THRESHOLD_MS;
    config->tcp_rto_min_us = DEFAULT_TCP_RTO_MIN_US;
    config->tcp_rto_max_us = DEFAULT_TCP_RTO_MAX_US;

    // Template structs for storage when scanning
    // After parsing each line, copy these into config struct
    lnx_interface_t f_iface;
    lnx_neighbor_t  f_neighbor;
    lnx_rip_neighbor_t f_advertise_to;
    lnx_static_route_t f_route;

    while ((line = fgets(buf, LINE_MAX, f)) != NULL) {
	memset(&f_iface, 0, sizeof(lnx_interface_t));
	memset(&f_neighbor, 0, sizeof(lnx_neighbor_t));
	memset(&f_advertise_to, 0, sizeof(lnx_rip_neighbor_t));
	memset(&f_route, 0, sizeof(lnx_static_route_t));
	memset(ip_buf1, 0, LINE_MAX);
	memset(ip_buf2, 0, LINE_MAX);
	memset(first_token, 0, TOKEN_MAX_NAME);

	g_current_line++;

	if (line[0] == '#') {
	    continue;
	}

	if ((tokens = sscanf(line, "%10s", first_token)) != 1) {
	    continue;
	}

	if ((strncmp(first_token, "interface", TOKEN_MAX_NAME)) == 0) {
	    tokens = sscanf(line, "interface %32s %32[^/]/%2d %32[^:]:%d",
			    f_iface.name, ip_buf1, &f_iface.prefix_len, ip_buf2, &port);
	    if (tokens != TOKEN_MAX_INTERFACE) {
		do_parse_error("Did not find enough tokens");
	    }

	    parse_addr(ip_buf1, &f_iface.assigned_ip);
	    parse_addr(ip_buf2, &f_iface.udp_addr);
	    f_iface.udp_port = (uint16_t)port;

	    add_config(&config->interfaces, &f_iface, lnx_interface_t);
	} else if ((strncmp(first_token, "neighbor", TOKEN_MAX_NAME)) == 0) {
	    tokens = sscanf(line, "neighbor %32s at %32[^:]:%d via %32[^ #]",
			    ip_buf1, ip_buf2, &port, f_neighbor.ifname);
	    if (tokens != TOKEN_MAX_NEIGHBOR) {
		do_parse_error("Did not find enough tokens");
	    }

	    parse_addr(ip_buf1, &f_neighbor.dest_addr);
	    parse_addr(ip_buf2, &f_neighbor.udp_addr);
	    f_neighbor.udp_port = (uint16_t)port;
	    add_config(&config->neighbors, &f_neighbor, lnx_neighbor_t);
	} else if ((strncmp(first_token, "routing", TOKEN_MAX_NAME) == 0)) {
	    char *mode_str = ip_buf1; // Reuse this buffer
	    tokens = sscanf(line, "routing %32s", mode_str);
	    if (tokens != 1) {
		do_parse_error("Did not find enough tokens");
	    }

	    if (strncmp(mode_str, "rip", TOKEN_MAX_NAME) == 0) {
		config->routing_mode = ROUTING_MODE_RIP;
	    } else if (strncmp(mode_str, "static", TOKEN_MAX_NAME) == 0) {
		config->routing_mode = ROUTING_MODE_STATIC;
	    } else {
		do_parse_error("Unrecognized routing mode");
	    }
	} else if (strncmp(first_token, "rip", TOKEN_MAX_NAME) == 0) {
	    char second_token[TOKEN_MAX_NAME];
	    memset(second_token, 0, TOKEN_MAX_NAME);

	    if ((tokens = sscanf(line, "rip %32s", second_token)) != 1) {
		do_parse_error("Did not find enough tokens");
	    }

	    if (strncmp(second_token, "periodic-update-rate", TOKEN_MAX_NAME) == 0) {
		tokens = sscanf(line, "rip periodic-update-rate %lu",
				&config->rip_periodic_update_rate_ms);
		if (tokens != TOKEN_MAX_NUMBER) {
		    do_parse_error("Did not find enough tokens");
		}
	    } else if (strncmp(second_token, "route-timeout-threshold", TOKEN_MAX_NAME) == 0) {
		tokens = sscanf(line, "rip route-timeout-threshold %lu",
				&config->rip_timeout_threshold_ms);
		if (tokens != TOKEN_MAX_NUMBER) {
		    do_parse_error("Did not find enough tokens");
		}
	    } else if (strncmp(second_token, "advertise-to", TOKEN_MAX_NAME) == 0) {
		tokens = sscanf(line, "rip advertise-to %32s", ip_buf1);
		if (tokens != TOKEN_MAX_RIP_NEIGHBOR) {
		    do_parse_error("Did not find enough tokens");
		}
		parse_addr(ip_buf1, &f_advertise_to.dest);
		add_config(&config->rip_neighbors, &f_advertise_to, lnx_rip_neighbor_t);
	    } else {
		do_parse_error("Unrecognized RIP directive");
	    }
	} else if (strncmp(first_token, "route", TOKEN_MAX_NAME) == 0) {
	    tokens = sscanf(line, "route %32[^/]/%2d via %32s",
			    ip_buf1, &f_route.prefix_len, ip_buf2);
	    if (tokens != TOKEN_MAX_ROUTE) {
		do_parse_error("Did not find enough tokens");
	    }

	    parse_addr(ip_buf1, &f_route.network_addr);
	    parse_addr(ip_buf2, &f_route.next_hop);
	    add_config(&config->static_routes, &f_route, lnx_static_route_t);
	} else if (strncmp(first_token, "tcp", TOKEN_MAX_NAME) == 0) {
	    char second_token[TOKEN_MAX_NAME];
	    memset(second_token, 0, TOKEN_MAX_NAME);

	    if ((tokens = sscanf(line, "tcp %32s", second_token)) != 1) {
		do_parse_error("Did not find enough tokens");
	    }

	    if (strncmp(second_token, "rto-min", TOKEN_MAX_NAME) == 0) {
		tokens = sscanf(line, "tcp rto-min %lu",
				&config->tcp_rto_min_us);
		if (tokens != TOKEN_MAX_NUMBER) {
		    do_parse_error("Did not find enough tokens");
		}
	    } else if (strncmp(second_token, "rto-max", TOKEN_MAX_NAME) == 0) {
		tokens = sscanf(line, "tcp rto-max %lu",
				&config->tcp_rto_max_us);
		if (tokens != TOKEN_MAX_NUMBER) {
		    do_parse_error("Did not find enough tokens");
		}
	    } else {
		do_parse_error("Unrecognized TCP directive");
	    }
	}

    }

    fclose(f);
    return config;
}


void lnxconfig_destroy(lnxconfig_t *config) {
    if (config == NULL) {
	return;
    }

    config_clear(&config->interfaces, lnx_interface_t);
    config_clear(&config->neighbors, lnx_neighbor_t);
    config_clear(&config->rip_neighbors, lnx_rip_neighbor_t);
    config_clear(&config->static_routes, lnx_static_route_t);

    free(config);
}


void do_abort(char *msg) {
    char buf[LINE_MAX];
    snprintf(buf, LINE_MAX, "Line %d:  %s", g_current_line, msg);
    perror(buf);
    exit(1);
}

void do_parse_error(char *msg) {
    printf("Parse error, line %d:  %s\n", g_current_line, msg);
    exit(1);
}
