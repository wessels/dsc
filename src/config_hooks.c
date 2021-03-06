/*
 * Copyright (c) 2016-2017, OARC, Inc.
 * Copyright (c) 2007, The Measurement Factory, Inc.
 * Copyright (c) 2007, Internet Systems Consortium, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <syslog.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include "xmalloc.h"
#include "dns_message.h"
#include "syslog_debug.h"
#include "hashtbl.h"
#include "pcap.h"
#include "compat.h"

extern int promisc_flag;
extern int monitor_flag;
extern int immediate_flag;
extern int threads_flag;
uint64_t   minfree_bytes      = 0;
int        output_format_xml  = 0;
int        output_format_json = 0;
#define MAX_HASH_SIZE 512
static hashtbl* dataset_hash         = NULL;
uint64_t        statistics_interval  = 60; /* default interval in seconds*/
int             dump_reports_on_exit = 0;
char*           geoip_v4_dat         = NULL;
int             geoip_v4_options     = 0;
char*           geoip_v6_dat         = NULL;
int             geoip_v6_options     = 0;
char*           geoip_asn_v4_dat     = NULL;
int             geoip_asn_v4_options = 0;
char*           geoip_asn_v6_dat     = NULL;
int             geoip_asn_v6_options = 0;
int             pcap_buffer_size     = 0;
int             no_wait_interval     = 0;
int             pt_timeout           = 100;
int             drop_ip_fragments    = 0;

int open_interface(const char* interface)
{
    dsyslogf(LOG_INFO, "Opening interface %s", interface);
    Pcap_init(interface, promisc_flag, monitor_flag, immediate_flag, threads_flag, pcap_buffer_size);
    return 1;
}

int set_bpf_program(const char* s)
{
    extern char* bpf_program_str;
    dsyslogf(LOG_INFO, "BPF program is: %s", s);
    if (bpf_program_str)
        xfree(bpf_program_str);
    bpf_program_str = xstrdup(s);
    if (NULL == bpf_program_str)
        return 0;
    return 1;
}

int add_local_address(const char* s, const char* m)
{
    extern int ip_local_address(const char*, const char*);
    dsyslogf(LOG_INFO, "adding local address %s%s%s", s, m ? " mask " : "", m ? m : "");
    return ip_local_address(s, m);
}

int set_run_dir(const char* dir)
{
    dsyslogf(LOG_INFO, "setting current directory to %s", dir);
    if (chdir(dir) < 0) {
        char errbuf[512];
        perror(dir);
        dsyslogf(LOG_ERR, "chdir: %s: %s", dir, dsc_strerror(errno, errbuf, sizeof(errbuf)));
        return 0;
    }
    return 1;
}

int set_pid_file(const char* s)
{
    extern char* pid_file_name;
    dsyslogf(LOG_INFO, "PID file is: %s", s);
    if (pid_file_name)
        xfree(pid_file_name);
    pid_file_name = xstrdup(s);
    if (NULL == pid_file_name)
        return 0;
    return 1;
}

static unsigned int
dataset_hashfunc(const void* key)
{
    return hashendian(key, strlen(key), 0);
}

static int
dataset_cmpfunc(const void* a, const void* b)
{
    return strcasecmp(a, b);
}

int set_statistics_interval(const char* s)
{
    dsyslogf(LOG_INFO, "Setting statistics interval to: %s", s);
    statistics_interval = strtoull(s, NULL, 10);
    if (statistics_interval == ULLONG_MAX) {
        char errbuf[512];
        dsyslogf(LOG_ERR, "strtoull: %s", dsc_strerror(errno, errbuf, sizeof(errbuf)));
        return 0;
    }
    if (!statistics_interval) {
        dsyslog(LOG_ERR, "statistics_interval can not be zero");
        return 0;
    }
    return 1;
}

int add_dataset(const char* name, const char* layer_ignored,
    const char* firstname, const char* firstindexer,
    const char* secondname, const char* secondindexer, const char* filtername, dataset_opt opts)
{
    char* dup;

    if (!dataset_hash) {
        if (!(dataset_hash = hash_create(MAX_HASH_SIZE, dataset_hashfunc, dataset_cmpfunc, 0, xfree, xfree))) {
            dsyslogf(LOG_ERR, "unable to create dataset %s due to internal error", name);
            return 0;
        }
    }

    if (hash_find(name, dataset_hash)) {
        dsyslogf(LOG_ERR, "unable to create dataset %s: already exists", name);
        return 0;
    }

    if (!(dup = xstrdup(name))) {
        dsyslogf(LOG_ERR, "unable to create dataset %s due to internal error", name);
        return 0;
    }

    if (hash_add(dup, dup, dataset_hash)) {
        xfree(dup);
        dsyslogf(LOG_ERR, "unable to create dataset %s due to internal error", name);
        return 0;
    }

    dsyslogf(LOG_INFO, "creating dataset %s", name);
    return dns_message_add_array(name, firstname, firstindexer, secondname, secondindexer, filtername, opts);
}

int set_bpf_vlan_tag_byte_order(const char* which)
{
    extern int vlan_tag_needs_byte_conversion;
    dsyslogf(LOG_INFO, "bpf_vlan_tag_byte_order is %s", which);
    if (0 == strcmp(which, "host")) {
        vlan_tag_needs_byte_conversion = 0;
        return 1;
    }
    if (0 == strcmp(which, "net")) {
        vlan_tag_needs_byte_conversion = 1;
        return 1;
    }
    dsyslogf(LOG_ERR, "unknown bpf_vlan_tag_byte_order '%s'", which);
    return 0;
}

int set_match_vlan(const char* s)
{
    extern void pcap_set_match_vlan(int);
    int         i;
    dsyslogf(LOG_INFO, "match_vlan %s", s);
    i = atoi(s);
    if (0 == i && 0 != strcmp(s, "0"))
        return 0;
    pcap_set_match_vlan(i);
    return 1;
}

int set_minfree_bytes(const char* s)
{
    dsyslogf(LOG_INFO, "minfree_bytes %s", s);
    minfree_bytes = strtoull(s, NULL, 10);
    return 1;
}

int set_output_format(const char* output_format)
{
    dsyslogf(LOG_INFO, "output_format %s", output_format);

    if (!strcmp(output_format, "XML")) {
        output_format_xml = 1;
        return 1;
    } else if (!strcmp(output_format, "JSON")) {
        output_format_json = 1;
        return 1;
    }

    dsyslogf(LOG_ERR, "unknown output format '%s'", output_format);
    return 0;
}

void set_dump_reports_on_exit(void)
{
    dsyslog(LOG_INFO, "dump_reports_on_exit");

    dump_reports_on_exit = 1;
}

int set_geoip_v4_dat(const char* dat, int options)
{
    char errbuf[512];

    geoip_v4_options = options;
    if (geoip_v4_dat)
        xfree(geoip_v4_dat);
    if ((geoip_v4_dat = xstrdup(dat))) {
        dsyslogf(LOG_INFO, "GeoIP v4 dat %s %d", geoip_v4_dat, geoip_v4_options);
        return 1;
    }

    dsyslogf(LOG_ERR, "unable to set GeoIP v4 dat, strdup: %s", dsc_strerror(errno, errbuf, sizeof(errbuf)));
    return 0;
}

int set_geoip_v6_dat(const char* dat, int options)
{
    char errbuf[512];

    geoip_v6_options = options;
    if (geoip_v6_dat)
        xfree(geoip_v6_dat);
    if ((geoip_v6_dat = xstrdup(dat))) {
        dsyslogf(LOG_INFO, "GeoIP v6 dat %s %d", geoip_v6_dat, geoip_v6_options);
        return 1;
    }

    dsyslogf(LOG_ERR, "unable to set GeoIP v6 dat, strdup: %s", dsc_strerror(errno, errbuf, sizeof(errbuf)));
    return 0;
}

int set_geoip_asn_v4_dat(const char* dat, int options)
{
    char errbuf[512];

    geoip_asn_v4_options = options;
    if (geoip_asn_v4_dat)
        xfree(geoip_asn_v4_dat);
    if ((geoip_asn_v4_dat = xstrdup(dat))) {
        dsyslogf(LOG_INFO, "GeoIP ASN v4 dat %s %d", geoip_asn_v4_dat, geoip_asn_v4_options);
        return 1;
    }

    dsyslogf(LOG_ERR, "unable to set GeoIP ASN v4 dat, strdup: %s", dsc_strerror(errno, errbuf, sizeof(errbuf)));
    return 0;
}

int set_geoip_asn_v6_dat(const char* dat, int options)
{
    char errbuf[512];

    geoip_asn_v6_options = options;
    if (geoip_asn_v6_dat)
        xfree(geoip_asn_v6_dat);
    if ((geoip_asn_v6_dat = xstrdup(dat))) {
        dsyslogf(LOG_INFO, "GeoIP ASN v6 dat %s %d", geoip_asn_v6_dat, geoip_asn_v6_options);
        return 1;
    }

    dsyslogf(LOG_ERR, "unable to set GeoIP ASN v6 dat, strdup: %s", dsc_strerror(errno, errbuf, sizeof(errbuf)));
    return 0;
}

int set_pcap_buffer_size(const char* s)
{
    dsyslogf(LOG_INFO, "Setting pcap buffer size to: %s", s);
    pcap_buffer_size = atoi(s);
    if (pcap_buffer_size < 0) {
        dsyslog(LOG_ERR, "pcap_buffer_size can not be negative");
        return 0;
    }
    return 1;
}

void set_no_wait_interval(void)
{
    dsyslog(LOG_INFO, "not waiting on interval sync to start");

    no_wait_interval = 1;
}

int set_pt_timeout(const char* s)
{
    dsyslogf(LOG_INFO, "Setting pcap-thread timeout to: %s", s);
    pt_timeout = atoi(s);
    if (pt_timeout < 0) {
        dsyslog(LOG_ERR, "pcap-thread timeout can not be negative");
        return 0;
    }
    return 1;
}

void set_drop_ip_fragments(void)
{
    dsyslog(LOG_INFO, "dropping ip fragments");

    drop_ip_fragments = 1;
}
