/*
 *   This file is part of nftlb, nftables load balancer.
 *
 *   Copyright (C) ZEVENET SL.
 *   Author: Laura Garcia <laura.garcia@zevenet.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Affero General Public License as
 *   published by the Free Software Foundation, either version 3 of the
 *   License, or any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Affero General Public License for more details.
 *
 *   You should have received a copy of the GNU Affero General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "backends.h"
#include "farms.h"
#include "objects.h"
#include "network.h"


static struct backend * backend_create(struct farm *f, char *name)
{
	struct backend *b = (struct backend *)malloc(sizeof(struct backend));
	if (!b) {
		syslog(LOG_ERR, "Backend memory allocation error");
		return NULL;
	}

	b->parent = f;
	obj_set_attribute_string(name, &b->name);

	b->fqdn = DEFAULT_FQDN;
	b->ethaddr = DEFAULT_ETHADDR;
	b->ipaddr = DEFAULT_IPADDR;
	b->ports = DEFAULT_PORTS;
	b->weight = DEFAULT_WEIGHT;
	b->priority = DEFAULT_PRIORITY;
	b->mark = DEFAULT_MARK;
	b->state = DEFAULT_BACKEND_STATE;
	b->action = DEFAULT_ACTION;

	list_add_tail(&b->list, &f->backends);
	f->total_bcks++;

	return b;
}

static int backend_delete_node(struct backend *b)
{
	list_del(&b->list);
	if (b->name)
		free(b->name);
	if (b->fqdn && strcmp(b->fqdn, "") != 0)
		free(b->fqdn);
	if (b->ipaddr && strcmp(b->ipaddr, "") != 0)
		free(b->ipaddr);
	if (b->ethaddr && strcmp(b->ethaddr, "") != 0)
		free(b->ethaddr);
	if (b->ports && strcmp(b->ports, "") != 0)
		free(b->ports);

	free(b);

	return 0;
}

static int backend_delete(struct backend *b)
{
	struct farm *f = b->parent;

	if (backend_is_available(b)) {
		f->bcks_available--;
		f->total_weight -= b->weight;
	}
	f->total_bcks--;
	farm_set_action(f, ACTION_RELOAD);

	backend_delete_node(b);

	return 0;
}

void backend_s_print(struct farm *f)
{
	struct backend *b;

	list_for_each_entry(b, &f->backends, list) {
		syslog(LOG_DEBUG,"    [backend] ");
		syslog(LOG_DEBUG,"       [%s] %s", CONFIG_KEY_NAME, b->name);

		if (b->fqdn)
			syslog(LOG_DEBUG,"       [%s] %s", CONFIG_KEY_FQDN, b->fqdn);

		if (b->ipaddr)
			syslog(LOG_DEBUG,"       [%s] %s", CONFIG_KEY_IPADDR, b->ipaddr);

		if (b->ethaddr)
			syslog(LOG_DEBUG,"       [%s] %s", CONFIG_KEY_ETHADDR, b->ethaddr);

		if (b->ports)
			syslog(LOG_DEBUG,"       [%s] %s", CONFIG_KEY_PORTS, b->ports);

		syslog(LOG_DEBUG,"       [%s] 0x%x", CONFIG_KEY_MARK, b->mark);
		syslog(LOG_DEBUG,"       [%s] %d", CONFIG_KEY_WEIGHT, b->weight);
		syslog(LOG_DEBUG,"       [%s] %d", CONFIG_KEY_PRIORITY, b->priority);
		syslog(LOG_DEBUG,"       [%s] %s", CONFIG_KEY_STATE, obj_print_state(b->state));
		syslog(LOG_DEBUG,"      *[%s] %d", CONFIG_KEY_ACTION, b->action);
	}
}

struct backend * backend_lookup_by_name(struct farm *f, const char *name)
{
	struct backend *b;

	list_for_each_entry(b, &f->backends, list) {
		if (strcmp(b->name, name) == 0)
			return b;
	}

	return NULL;
}

static int backend_set_ipaddr_from_ether(struct backend *b)
{
	struct farm *f = b->parent;
	int ret = -1;
	unsigned char dst_ethaddr[ETH_HW_ADDR_LEN];
	unsigned char src_ethaddr[ETH_HW_ADDR_LEN];
	char streth[ETH_HW_STR_LEN] = {};

	if (!farm_is_ingress_mode(f))
		return 0;

	if (f->iethaddr == DEFAULT_ETHADDR ||
		b->ipaddr == DEFAULT_IPADDR ||
		f->ofidx == DEFAULT_IFIDX)
		return -1;

	sscanf(f->iethaddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &src_ethaddr[0], &src_ethaddr[1], &src_ethaddr[2], &src_ethaddr[3], &src_ethaddr[4], &src_ethaddr[5]);

	ret = net_get_neigh_ether((unsigned char **) &dst_ethaddr, src_ethaddr, f->family, f->virtaddr, b->ipaddr, f->ofidx);

	if (ret == 0) {
		sprintf(streth, "%02x:%02x:%02x:%02x:%02x:%02x", dst_ethaddr[0],
			dst_ethaddr[1], dst_ethaddr[2], dst_ethaddr[3], dst_ethaddr[4], dst_ethaddr[5]);

		syslog(LOG_DEBUG, "%s():%d: discovered ether address for %s is %s", __FUNCTION__, __LINE__, b->name, streth);

		obj_set_attribute_string(streth, &b->ethaddr);
	}

	return ret;
}

static int backend_set_weight(struct backend *b, int new_value)
{
	struct farm *f = b->parent;
	int old_value = b->weight;

	syslog(LOG_DEBUG, "%s():%d: current value is %d, but new value will be %d",
	       __FUNCTION__, __LINE__, old_value, new_value);

	b->weight = new_value;

	if (backend_is_available(b))
		f->total_weight += (b->weight-old_value);

	return 0;
}

static int backend_set_priority(struct backend *b, int new_value)
{
	struct farm *f = b->parent;
	int old_value = b->priority;

	syslog(LOG_DEBUG, "%s():%d: current value is %d, but new value will be %d",
	       __FUNCTION__, __LINE__, old_value, new_value);

	if (backend_is_available(b) &&
	    new_value > f->priority) {
		f->bcks_available--;
		f->total_weight -= b->weight;
	}

	else if (old_value > f->priority &&
		 backend_is_available(b)) {
		f->bcks_available++;
		f->total_weight += b->weight;
	}

	b->priority = new_value;

	return 0;
}

static int backend_s_set_marked(struct farm *f)
{
	struct backend *b;

	syslog(LOG_DEBUG, "%s():%d: finding marked backends for %s", __FUNCTION__, __LINE__, f->name);

	list_for_each_entry(b, &f->backends, list) {
		if (b->mark != DEFAULT_MARK) {
			f->bcks_are_marked = 1;
			return 1;
		}
	}

	f->bcks_are_marked = 0;
	return 0;
}

static int backend_set_mark(struct backend *b, int new_value)
{
	int old_value = b->mark;

	syslog(LOG_DEBUG, "%s():%d: current value is %d, but new value will be %d",
	       __FUNCTION__, __LINE__, old_value, new_value);

	b->mark = new_value;

	if (b->mark != DEFAULT_MARK)
		b->parent->bcks_are_marked = 1;
	else
		backend_s_set_marked(b->parent);

	return 0;
}

static int backend_validate(struct backend *b)
{
	struct farm *f = b->parent;

	syslog(LOG_DEBUG, "%s():%d: validating backend %s of farm %s",
	       __FUNCTION__, __LINE__, b->name, f->name);

	if (farm_is_ingress_mode(f) &&
		(!b->ethaddr || strcmp(b->ethaddr, "") == 0))
		return 0;

	if (!b->ipaddr || strcmp(b->ipaddr, "") == 0)
		return 0;

	return 1;
}

static int backend_is_usable(struct backend *b)
{
	struct farm *f = b->parent;

	syslog(LOG_DEBUG, "%s():%d: backend %s state is %s and priority %d",
	       __FUNCTION__, __LINE__, b->name, obj_print_state(b->state), b->priority);

	return (b->state == VALUE_STATE_UP) &&
			(b->priority <= f->priority);
}

int backend_is_available(struct backend *b)
{
	syslog(LOG_DEBUG, "%s():%d: backend %s state is %s and priority %d",
	       __FUNCTION__, __LINE__, b->name, obj_print_state(b->state), b->priority);

	return (backend_is_usable(b) &&
			backend_validate(b));
}

int backend_set_action(struct backend *b, int action)
{
	if (action == ACTION_DELETE) {
		backend_delete(b);
		return 1;
	}

	if (action == ACTION_STOP) {
		b->action = action;
		backend_set_state(b, VALUE_STATE_OFF);
		return 1;
	}

	if (action == ACTION_START) {
		b->action = action;
		backend_set_state(b, VALUE_STATE_UP);
		return 1;
	}


	if (b->action > action) {
		b->action = action;
		return 1;
	}

	return 0;
}

int backend_s_set_action(struct farm *f, int action)
{
	struct backend *b, *next;

	list_for_each_entry_safe(b, next, &f->backends, list)
		backend_set_action(b, action);

	return 0;
}

int backend_s_delete(struct farm *f)
{
	struct backend *b, *next;

	list_for_each_entry_safe(b, next, &f->backends, list)
		backend_delete(b);

	f->total_bcks = 0;
	f->bcks_available = 0;
	f->total_weight = 0;

	return 0;
}

int backend_s_validate(struct farm *f)
{
	struct backend *b, *next;

	list_for_each_entry_safe(b, next, &f->backends, list) {
		if (b->state == VALUE_STATE_CONFERR && backend_validate(b))
			backend_set_state(b, VALUE_STATE_UP);
	}

	return 0;
}

int backend_set_attribute(struct config_pair *c)
{
	struct obj_config *cur = obj_get_current_object();
	struct backend *b;

	if (!cur->fptr)
		return PARSER_OBJ_UNKNOWN;

	if (c->key != KEY_NAME && !cur->bptr)
		return PARSER_OBJ_UNKNOWN;

	b = cur->bptr;

	switch (c->key) {
	case KEY_NAME:
		b = backend_lookup_by_name(cur->fptr, c->str_value);
		if (!b) {
			b = backend_create(cur->fptr, c->str_value);
			if (!b)
				return -1;
		}
		cur->bptr = b;
		break;
	case KEY_NEWNAME:
		obj_set_attribute_string(c->str_value, &b->name);
		break;
	case KEY_FQDN:
		obj_set_attribute_string(c->str_value, &b->fqdn);
		break;
	case KEY_IPADDR:
		obj_set_attribute_string(c->str_value, &b->ipaddr);
		if (farm_set_ifinfo(b->parent, KEY_OFACE) == -1 ||
		    backend_set_ipaddr_from_ether(b) == -1) {
			syslog(LOG_DEBUG, "%s():%d: backend %s comes to OFF", __FUNCTION__, __LINE__, b->name);
			backend_set_state(b, VALUE_STATE_CONFERR);
		}
		break;
	case KEY_ETHADDR:
		obj_set_attribute_string(c->str_value, &b->ethaddr);
		break;
	case KEY_PORTS:
		obj_set_attribute_string(c->str_value, &b->ports);
		break;
	case KEY_WEIGHT:
		backend_set_weight(b, c->int_value);
		break;
	case KEY_PRIORITY:
		backend_set_priority(b, c->int_value);
		break;
	case KEY_MARK:
		backend_set_mark(b, c->int_value);
		break;
	case KEY_STATE:
		backend_set_state(b, c->int_value);
		break;
	case KEY_ACTION:
		backend_set_action(b, c->int_value);
		break;
	default:
		return -1;
	}

	return PARSER_OK;
}

static int backend_switch(struct backend *b, int new_state)
{
	struct farm *f = b->parent;

	syslog(LOG_DEBUG, "%s():%d: backend %s switched to %s",
	       __FUNCTION__, __LINE__, b->name, obj_print_state(new_state));

	if (b->state == VALUE_STATE_UP) {
		f->total_weight += b->weight;
		f->bcks_available++;
		b->action = ACTION_START;
	} else {
		f->total_weight -= b->weight;
		f->bcks_available--;
		b->action = ACTION_STOP;
	}

	return 0;
}

int backend_set_state(struct backend *b, int new_value)
{
	int old_value = b->state;

	syslog(LOG_DEBUG, "%s():%d: backend %s current value is %s, but new value will be %s",
	       __FUNCTION__, __LINE__, b->name, obj_print_state(old_value), obj_print_state(new_value));

	if (old_value == new_value)
		return 0;

	if (backend_is_available(b) &&
	    new_value != VALUE_STATE_UP) {

		b->state = new_value;
		backend_switch(b, new_value);
		return 0;
	}

	b->state = new_value;

	if (new_value == VALUE_STATE_UP) {

		if (!backend_validate(b))
			b->state = VALUE_STATE_CONFERR;

		if (backend_is_available(b))
			backend_switch(b, new_value);
	}

	return 0;
}

int backend_s_set_ether_by_ipaddr(struct farm *f, const char *ip_bck, char *ether_bck)
{
	struct backend *b;
	int changed = 0;

	list_for_each_entry(b, &f->backends, list) {

		if (strcmp(b->ipaddr, ip_bck) != 0)
			continue;

		syslog(LOG_DEBUG, "%s():%d: backend with ip address %s found", __FUNCTION__, __LINE__, ip_bck);

		if (!b->ethaddr || (b->ethaddr && strcmp(b->ethaddr, ether_bck) != 0)) {
			obj_set_attribute_string(ether_bck, &b->ethaddr);
			backend_set_state(b, VALUE_STATE_UP);
			changed = 1;
			syslog(LOG_INFO, "%s():%d: ether address changed for backend %s with %s", __FUNCTION__, __LINE__, b->name, ether_bck);
		}
	}

	return changed;
}

int backend_s_find_ethers(struct farm *f)
{
	struct backend *b;
	int changed = 0;

	syslog(LOG_DEBUG, "%s():%d: finding backends for %s", __FUNCTION__, __LINE__, f->name);

	list_for_each_entry(b, &f->backends, list) {
		if (!backend_is_usable(b) || backend_validate(b))
			continue;

		if (backend_set_ipaddr_from_ether(b) == -1)
			backend_set_state(b, VALUE_STATE_CONFERR);
		else
			backend_set_state(b, VALUE_STATE_UP);
	}

	return changed;
}

struct backend * backend_get_first(struct farm *f)
{
	if (list_empty(&f->backends))
		return NULL;

	return list_first_entry(&f->backends, struct backend, list);
}

int bck_pre_actionable(struct config_pair *c)
{
	struct obj_config *cur = obj_get_current_object();
	struct farm *f;
	struct backend *b;

	if (!cur->fptr || !cur->bptr)
		return -1;

	f = cur->fptr;
	b = cur->bptr;

	syslog(LOG_DEBUG, "%s():%d: pre actionable backend %s of farm %s with param %d", __FUNCTION__, __LINE__, b->name, f->name, c->key);

	if (!backend_is_available(b))
		return 0;

	switch (c->key) {
	case KEY_NAME:
		break;
	case KEY_ETHADDR:
	case KEY_IPADDR:
	case KEY_PRIORITY:

		if (backend_set_action(b, ACTION_STOP) &&
			farm_set_action(f, ACTION_RELOAD)) {
			farm_rulerize(f);
		}

		break;
	case KEY_STATE:
	case KEY_MARK:
	case KEY_WEIGHT:
	default:
		break;
	}

	return 0;
}

int bck_pos_actionable(struct config_pair *c)
{
	struct obj_config *cur = obj_get_current_object();
	struct farm *f;
	struct backend *b;

	if (!cur->fptr || !cur->bptr)
		return -1;

	f = cur->fptr;
	b = cur->bptr;

	syslog(LOG_DEBUG, "%s():%d: pos actionable backend %s of farm %s with param %d", __FUNCTION__, __LINE__, b->name, f->name, c->key);

	if (!backend_is_available(b) && c->key != KEY_STATE)
		return 0;

	switch (c->key) {
	case KEY_NAME:
		break;
	case KEY_ETHADDR:
	case KEY_IPADDR:
	case KEY_PRIORITY:

		if (backend_set_action(b, ACTION_START) &&
			farm_set_action(f, ACTION_RELOAD)) {
			farm_rulerize(f);
		}

		break;
	case KEY_STATE:
	case KEY_MARK:
	case KEY_WEIGHT:

		farm_set_action(f, ACTION_RELOAD);

		break;
	default:
		break;
	}

	return 0;
}
