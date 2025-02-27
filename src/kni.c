/*
 * Copyright (c) 2022 Baidu.com, Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Jianzhang Peng (pengjianzhang@baidu.com)
 */

#include "kni.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <rte_ethdev.h>
#include <rte_kni.h>

#include "config.h"
#include "port.h"
#include "mbuf.h"
#include "work_space.h"

static int kni_set_mtu(uint16_t port_id, struct rte_kni_conf *conf)
{
    int ret = 0;
    struct rte_eth_dev_info dev_info;

    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        return -1;
    }

    conf->min_mtu = dev_info.min_mtu;
    conf->max_mtu = dev_info.max_mtu;
	rte_eth_dev_get_mtu(port_id, &conf->mtu);

    return 0;
}

static int kni_set_mac(uint16_t port_id, struct rte_kni_conf *conf)
{
    int ret = 0;

    ret = rte_eth_macaddr_get(port_id, (struct rte_ether_addr *)&(conf->mac_addr));
    if (ret != 0) {
        return -1;
    }

    return 0;
}

static void kni_set_name(struct config *cfg, struct netif_port *port, char *name)
{
    int idx = 0;

    /*
     * do not use 'port->id'.
     * we want ifname id starting from zero.
     * */
    idx = port - &(cfg->ports[0]);
    snprintf(name, RTE_KNI_NAMESIZE, "%s%d", cfg->kni_ifname, idx);
}

static struct rte_kni *kni_alloc(struct config *cfg, struct netif_port *port)
{
    uint16_t port_id = 0;
    struct rte_kni *kni = NULL;
    struct rte_mempool *mbuf_pool = NULL;
    struct rte_kni_conf conf;

    /* the first thread of a port process this kni */
    mbuf_pool = port->mbuf_pool[0];
    port_id = port->id;
    memset(&conf, 0, sizeof(conf));

    conf.group_id = port_id;
    conf.mbuf_size = RTE_MBUF_DEFAULT_DATAROOM;

    kni_set_name(cfg, port, conf.name);

    if (kni_set_mtu(port_id, &conf) < 0) {
        return NULL;
    }

    if (kni_set_mac(port_id, &conf) < 0) {
        return NULL;
    }

    kni = rte_kni_alloc(mbuf_pool, &conf, NULL);

    return kni;
}

static int kni_set_link_up(struct config *cfg, struct netif_port *port)
{
    int fd = -1;
    int ret = -1;
    struct ifreq ifr;

    if (!port->kni) {
        return 0;
    }

    fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&ifr, 0, sizeof(struct ifreq));
    kni_set_name(cfg, port, ifr.ifr_name);

    if (ioctl(fd, SIOCGIFFLAGS, (void *) &ifr) < 0) {
        printf("kni get flags error\n");
        goto out;
    }
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, (void *) &ifr) < 0) {
        printf("kni set flags error\n");
        goto out;
    }
    ret = 0;

out:
    close(fd);
    return ret;
}

/*
 * kni_link_up() should be called at the ctl thread.
 *
 * When a kni interface is linked up, the kni will send some messages,
 * which need to be processed by rte_kni_handle_request().
 * These messages have a timeout and need to be processed immediately.
 * Otherwise, the interface will fail to link up.
 */
int kni_link_up(struct config *cfg)
{
    struct netif_port *port = NULL;

    config_for_each_port(cfg, port) {
        if (kni_set_link_up(cfg, port) < 0) {
            return -1;
        }
    }

    return 0;
}

static void kni_free(struct config *cfg)
{
    struct netif_port *port = NULL;

    config_for_each_port(cfg, port) {
        if (port->kni == NULL) {
            continue;
        }

        if (rte_kni_release(port->kni)) {
            printf("failed to free kni\n");
        }
        port->kni = NULL;
    }
}

/*
 * kni address cannot in client or server range
 */
static int kni_create(struct config *cfg)
{
    struct rte_kni *kni = NULL;
    struct netif_port *port = NULL;

    rte_kni_init(NETIF_PORT_MAX);
    config_for_each_port(cfg, port) {
        kni = kni_alloc(cfg, port);
        if (kni == NULL) {
            return -1;
        }
        port->kni = kni;
    }

    return 0;
}

int kni_start(struct config *cfg)
{
    if (!cfg->kni) {
        return 0;
    }

    return kni_create(cfg);
}

void kni_stop(struct config *cfg)
{
    if (cfg->kni) {
        kni_free(cfg);
    }
}

void kni_recv(struct work_space *ws, struct rte_mbuf *m)
{
    struct netif_port *port = NULL;
    struct rte_kni *kni = NULL;

    port = ws->port;
    kni = port->kni;
    if (kni) {
        if (rte_kni_tx_burst(kni, &m, 1) == 1) {
            net_stats_kni_rx();
            return;
        }
    }

    mbuf_free2(m);
}

void kni_send(struct work_space *ws)
{
    int i = 0;
    int num = 0;
    struct rte_mbuf *mbufs[NB_RXD];
    struct netif_port *port = NULL;
    struct rte_kni *kni = NULL;

    port = ws->port;
    kni = port->kni;
    rte_kni_handle_request(kni);
    num = rte_kni_rx_burst(kni, mbufs, NB_RXD);
    for (i = 0; i < num; i++) {
        work_space_tx_send(ws, mbufs[i]);
        net_stats_kni_tx();
    }
}
