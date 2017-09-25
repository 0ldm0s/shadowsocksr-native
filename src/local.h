/*
 * local.h - Define the client's buffers and callbacks
 *
 * Copyright (C) 2013 - 2016, Max Lv <max.c.lv@gmail.com>
 *
 * This file is part of the shadowsocks-libev.
 *
 * shadowsocks-libev is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * shadowsocks-libev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with shadowsocks-libev; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef _LOCAL_H
#define _LOCAL_H

#include <uv.h>
#include <ev.h>
#include <libcork/ds.h>

#include "encrypt.h"
#include "jconf.h"
#include "protocol.h"

#include "common.h"

// use this as a profile or environment
struct listen_ctx_t {
    uv_tcp_t listen_socket; // ev_io io;
    ss_host_port tunnel_addr;

    struct cork_dllist_item entries; // for inactive profile list
    struct cork_dllist connections_eden; // For connections just created but not attach to a server

    char *iface;
    int timeout;
    //int fd;
    int mptcp;

    int server_num;
    struct server_env_t servers[MAX_SERVER_NUM];
};

struct server_ctx_t {
    //ev_io io;
    int connected;
    __weak_ptr struct server_t *server;
};

struct remote_ctx_t {
    //ev_io io;
    ev_timer watcher;
    int connected;
    __weak_ptr struct remote_t *remote;
};

struct remote_t {
    uv_tcp_t socket; // int fd;
    uv_connect_t connect;
    uv_write_t write_req;
    struct buffer_t *buf;
    struct remote_ctx_t *recv_ctx;
    struct remote_ctx_t *send_ctx;
    uint32_t counter;
    __weak_ptr struct server_t *server;

    int direct;
    struct { // direct = 1
        struct sockaddr_storage addr;
        size_t addr_len;
    } direct_addr;
};

struct server_t {
    uv_tcp_t client_connect; // int fd;
    uv_write_t write_req;
    enum net_stage stage;
    struct enc_ctx *e_ctx;
    struct enc_ctx *d_ctx;
    struct server_ctx_t *recv_ctx;
    struct server_ctx_t *send_ctx;
    __weak_ptr struct listen_ctx_t *listener;
    struct remote_t *remote;

    struct buffer_t *buf;

    struct cork_dllist_item entries;
    struct cork_dllist_item entries_all; // for all_connections

    __weak_ptr struct server_env_t *server_env;

    // SSR
    struct obfs_t *protocol;
    struct obfs_t *obfs;
};

#endif // _LOCAL_H
