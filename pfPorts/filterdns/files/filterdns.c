/*
 * Copyright (C) 2009 - 2011 Ermal Lu�i
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/queue.h>

#include <net/if.h>
#include <net/pfvar.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip_fw.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h>
#include <pthread_np.h>
#include <syslog.h>
#include <stdarg.h>
#include <err.h>
#include <sysexits.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "filterdns.h"

static int interval = 30;
static int dev = -1;
static int debug = 0;
static char *ipfwctx = NULL;
static char *file = NULL;
static pthread_attr_t attr;
static pthread_cond_t sig_condvar;
static pthread_mutex_t sig_mtx;
static pthread_t sig_thr;
static pthread_rwlock_t main_lock;

static void pf_tableentry(struct thread_data *, struct sockaddr *, int);
static void ipfw_tableentry(struct thread_data *, struct sockaddr *, int);
static int host_dns(struct thread_data *);
static int filterdns_clean_table(struct thread_data *, int);
static void clear_config(struct thread_list *);
static void clear_hostname_addresses(struct thread_data *);

void *check_hostname(void *arg);

#define DELETE 1
#define ADD 2

#define satosin(sa)	((struct sockaddr_in *)(sa))
#define satosin6(sa)	((struct sockaddr_in6 *)(sa))

#if 0
static void
flush_table(char *tablename)
{
        struct pfioc_table      io;

        memset(&io, 0, sizeof(io));
        if (strlcpy(io.pfrio_table.pfrt_name, tablename,
            sizeof(io.pfrio_table.pfrt_name)) >=
            sizeof(io.pfrio_table.pfrt_name))
		return; /* XXX */
        /* pfctl -Tflush */
        if (ioctl(dev, DIOCRCLRADDRS, &io) == -1)
                syslog(LOG_WARNING, "Cannot flush table %s addresses", tablename);
}
#endif

static int
add_table_entry(struct table_entry *rnh, struct sockaddr *addr, struct thread_data *thrdata)
{
	struct table *ent, *tmp;
	char buffer[INET6_ADDRSTRLEN] = { 0 };
	TAILQ_FOREACH(tmp, rnh, entry) {
		if (tmp->addr->sa_family != addr->sa_family)
			continue;
		if (!memcmp(addr, tmp->addr, addr->sa_len)) {
			if (debug >= 2) {
				if (addr->sa_family == AF_INET)
					syslog(LOG_WARNING, "\t\tentry %s exists in table %s", inet_ntop(addr->sa_family, &satosin(addr)->sin_addr.s_addr, buffer, sizeof buffer), thrdata->tablename);
				else if (addr->sa_family == AF_INET6)
					syslog(LOG_WARNING, "\t\tentry %s exists in table %s", inet_ntop(addr->sa_family, satosin6(addr)->sin6_addr.s6_addr, buffer, sizeof buffer), thrdata->tablename);
			}
			tmp->refcnt++;
			return (EEXIST);
		}
	}

	ent = calloc(1, sizeof(*ent));
	if (ent == NULL) {
		if (debug >= 1)
			syslog(LOG_WARNING, "\tFailed to allocate new entry for table %s.", thrdata->tablename);
		return (ENOMEM);
	}
	ent->addr = calloc(1, addr->sa_len);
	if (ent->addr == NULL) {
		free(ent);
		if (debug >= 1)
			syslog(LOG_WARNING, "\tFailed to allocate new address entry for table %s.", thrdata->tablename);
		return (ENOMEM);
	}
	memcpy(ent->addr, addr, addr->sa_len);;
	ent->refcnt = 2;
	TAILQ_INSERT_HEAD(rnh, ent, entry);
	if (thrdata->type == PF_TYPE) {
		if (debug >= 2) {
			if(addr->sa_family == AF_INET)
				syslog(LOG_WARNING, "\tadding entry %s to table %s on host %s", inet_ntop(addr->sa_family, addr->sa_data + 2, buffer, sizeof buffer), thrdata->tablename, thrdata->hostname);
			if(addr->sa_family == AF_INET6)
				syslog(LOG_WARNING, "\tadding entry %s to table %s on host %s", inet_ntop(addr->sa_family, addr->sa_data + 6, buffer, sizeof buffer), thrdata->tablename, thrdata->hostname);
		}
		pf_tableentry(thrdata, ent->addr, ADD);
	}
	else if (thrdata->type == IPFW_TYPE) {
		if (debug >= 2)	
			syslog(LOG_WARNING, "\tadding entry %s to table %s on host %s", inet_ntop(addr->sa_family, addr->sa_data + 2, buffer, sizeof buffer), thrdata->tablename, thrdata->hostname);
		ipfw_tableentry(thrdata, addr, ADD);
	}
	
	return (0);
}

static int
filterdns_clean_table(struct thread_data *thrdata, int donotcheckrefcount)
{
	struct table *e, *tmp;
	char buffer[INET6_ADDRSTRLEN] = { 0 };

	if (TAILQ_EMPTY(&thrdata->rnh))
		return (0);

	TAILQ_FOREACH_SAFE(e, &thrdata->rnh, entry, tmp) {
		e->refcnt--;
		if (donotcheckrefcount || e->refcnt <= 0) {
			if (thrdata->type == PF_TYPE) {
				if (debug >= 2) {
					if(e->addr->sa_family == AF_INET)
						syslog(LOG_WARNING, "\t\tclearing entry %s from table %s on host %s", inet_ntop(e->addr->sa_family, e->addr->sa_data + 2, buffer, sizeof buffer), thrdata->tablename, thrdata->hostname);
					if(e->addr->sa_family == AF_INET6)
						syslog(LOG_WARNING, "\t\tclearing entry %s from table %s on host %s", inet_ntop(e->addr->sa_family, e->addr->sa_data + 6, buffer, sizeof buffer), thrdata->tablename, thrdata->hostname);
				}
				pf_tableentry(thrdata, e->addr, DELETE);
			}
			else if (thrdata->type == IPFW_TYPE) {
				if (debug >= 2)
					syslog(LOG_WARNING, "\t\tclearing entry %s from table %s on host %s", inet_ntop(e->addr->sa_family, e->addr->sa_data + 2, buffer, sizeof buffer), thrdata->tablename, thrdata->hostname);
				ipfw_tableentry(thrdata, e->addr, DELETE);
			}
			TAILQ_REMOVE(&thrdata->rnh, e, entry);
			free(e->addr);
			free(e);
		} else if (debug >= 2) {
			if (thrdata->type == PF_TYPE) {
				if (debug >= 2) {
					if(e->addr->sa_family == AF_INET)
						syslog(LOG_WARNING, "\tNOT clearing entry %s from table %s on host %s", inet_ntop(e->addr->sa_family, e->addr->sa_data + 2, buffer, sizeof buffer), thrdata->tablename, thrdata->hostname);
					if(e->addr->sa_family == AF_INET6)
						syslog(LOG_WARNING, "\tNOT clearing entry %s from table %s on host %s", inet_ntop(e->addr->sa_family, e->addr->sa_data + 6, buffer, sizeof buffer), thrdata->tablename, thrdata->hostname);
				}
			}
			else if (thrdata->type == IPFW_TYPE) {
				if (debug >= 2)
					syslog(LOG_WARNING, "\tNOT clearing entry %s from table %s on host %s", inet_ntop(e->addr->sa_family, e->addr->sa_data + 2, buffer, sizeof buffer), thrdata->tablename, thrdata->hostname);
			}
		}
	}

	return (0);
}

static int
host_dns(struct thread_data *hostd)
{
        struct addrinfo          *res0, *res;
        int                      error, cnt = 0;
	char buffer[INET6_ADDRSTRLEN];

	res0 = NULL;
        error = getaddrinfo(hostd->hostname, NULL, NULL, &res0);
        if (error == EAI_AGAIN) {
		if (debug >= 1)
			syslog(LOG_WARNING, "failed to resolve host %s will retry later again.", hostd->hostname);
		if (res0 != NULL)
			freeaddrinfo(res0);
                return (0);
	}
        if (error) {
		if (debug >= 1)
                	syslog(LOG_WARNING, "host_dns: failed looking up \"%s\": %s", hostd->hostname,
                    		gai_strerror(error));
		if (res0 != NULL)
			freeaddrinfo(res0);
                return (-1);
        }

        for (res = res0; res; res = res->ai_next) {
                if (res->ai_family == AF_INET) {
			if (debug > 9)
				syslog(LOG_WARNING, "\t\tfound entry %s for %s", inet_ntop(res->ai_family, res->ai_addr->sa_data + 2, buffer, sizeof buffer), hostd->tablename);
			if (!add_table_entry(&hostd->rnh, res->ai_addr, hostd))
                		cnt++;
		}
		if(res->ai_family == AF_INET6) {
			if (debug > 9)
				syslog(LOG_WARNING, "\t\tfound entry %s for %s", inet_ntop(res->ai_family, res->ai_addr->sa_data + 6, buffer, sizeof buffer), hostd->tablename);
			if (!add_table_entry(&hostd->rnh, res->ai_addr, hostd))
                		cnt++;
                }
        }
        freeaddrinfo(res0);
        return (cnt);
}

static void
ipfw_tableentry(struct thread_data *ipfwd, struct sockaddr *address, int action)
{
	ipfw_table_entry ent;
	static int s = -1;

	if (address->sa_family != AF_INET) /* XXX */
		return;
	bzero(&ent, sizeof(ent));
	ent.masklen = ipfwd->mask;
	ent.tbl = ipfwd->tablenr;
	ent.addr = satosin(address)->sin_addr.s_addr;
	ent.value = ipfwd->pipe; /* XXX */

	if (s == -1)
		s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	if (s < 0)
		return;

#ifndef IP_FW_CTX_SET
#define	IP_FW_CTX_SET	92
#endif
	if (ipfwctx != NULL)
		setsockopt(s, IPPROTO_IP, IP_FW_CTX_SET, (void *)ipfwctx, strlen(ipfwctx));
	setsockopt(s, IPPROTO_IP, action == ADD ? IP_FW_TABLE_ADD : IP_FW_TABLE_DEL, (void *)&ent, sizeof(ent));
}

static void
set_ipmask(struct in6_addr *h, int b)
{
        struct pf_addr m;
        int i, j = 0;

	memset(&m, 0, sizeof m);

	while (b >= 32) {
                m.addr32[j++] = 0xffffffff;
                b -= 32;
        }
        for (i = 31; i > 31-b; --i)
                m.addr32[j] |= (1 << i);
        if (b)
                m.addr32[j] = htonl(m.addr32[j]);

        /* Mask off bits of the address that will never be used. */
        for (i = 0; i < 4; i++)
        	h->__u6_addr.__u6_addr32[i] = h->__u6_addr.__u6_addr32[i] & m.addr32[i];
}

static void
pf_tableentry(struct thread_data *pfd, struct sockaddr *address, int action)
{
	struct pfioc_table io;
	struct pfr_table table;
	struct pfr_addr addr;

	bzero(&table, sizeof(table));
	if (strlcpy(table.pfrt_name, pfd->tablename,
		sizeof(table.pfrt_name)) >= sizeof(table.pfrt_name)) {
		if (debug >= 1)
			syslog(LOG_WARNING, "could not add address to table %s", pfd->tablename);
		return;
	}
	
	bzero(&addr, sizeof(addr));
	if (address->sa_family == AF_INET) {
		addr.pfra_af = address->sa_family;
		addr.pfra_net = pfd->mask;
		addr.pfra_ip4addr = satosin(address)->sin_addr;
	}
	if (address->sa_family == AF_INET6) {
		addr.pfra_af = address->sa_family;
		memcpy(&addr.pfra_ip6addr, &satosin6(address)->sin6_addr, sizeof(addr.pfra_ip6addr));
		addr.pfra_net = pfd->mask6;
		set_ipmask(&addr.pfra_ip6addr, pfd->mask6);
	}
	if(debug >= 4)
		syslog(LOG_WARNING, "setting subnet mask for family %i to %i", addr.pfra_af, addr.pfra_net);

	bzero(&io, sizeof io);
        io.pfrio_table = table;
        io.pfrio_buffer = &addr;
        io.pfrio_esize = sizeof(addr);
        io.pfrio_size = 1;

	if (action == DELETE) {
		 if (ioctl(dev, DIOCRDELADDRS, &io)) {
			if (debug >= 3)
				syslog(LOG_WARNING, "FAILED to delete address from table %s.", pfd->tablename);
		} else {
			if (debug >= 3)
				syslog(LOG_WARNING, "\t DELETED %d addresses(%d) to table %s.", io.pfrio_nadd, address->sa_family, pfd->tablename);
		}
	} else if (action == ADD) {
		if (ioctl(dev, DIOCRADDADDRS, &io)) {
			if (debug >= 3)
				syslog(LOG_WARNING, "FAILED to add address to table %s with errno %d.", pfd->tablename, errno);
		} else {
			if (debug >= 3)
				syslog(LOG_WARNING, "\t ADDED %d addresses(%d) to table %s.", io.pfrio_nadd, address->sa_family, pfd->tablename);
		}
	}
}

void *check_hostname(void *arg)
{
	struct thread_data *thrd = arg;
	struct timespec ts;
        char *p, *q;
	int howmuch, error1, tmp;

	if (!thrd->hostname)
		return (NULL);

        if ((p = strrchr(thrd->hostname, '/')) != NULL) {
                thrd->mask = strtol(p+1, &q, 0);
		if(thrd->mask == 32)
			thrd->mask6 = 128;
		if(thrd->mask <32)
			thrd->mask6 = thrd->mask *2;
                if (!q || *q || thrd->mask > 32 || q == (p+1)) {
			if (debug >= 1)
                        	syslog(LOG_WARNING, "invalid netmask '%s' for hostname %s\n", p, thrd->hostname);
                        return (NULL);
                }
		tmp = strlen(p) + 1;
		thrd->hostname[tmp] = '\0'; 
		q = thrd->hostname + tmp + 1;
		free(q);
        } else {
		thrd->mask = 32;
		thrd->mask6 = 128;
	}

	if (debug >= 2)
		syslog(LOG_WARNING, "Found hostname %s with netmask %d.", thrd->hostname, thrd->mask);

	//flush_table(thrd->tablename);

	while ((ts.tv_sec = time(NULL)) < 0)
		;

	pthread_mutex_lock(&thrd->mtx);
	for (;;) {

		ts.tv_sec += interval;
		ts.tv_sec += (interval % 30);
		ts.tv_nsec = 0;

		pthread_rwlock_rdlock(&main_lock);
		howmuch = host_dns(thrd);	
		if (debug >= 2)
			syslog(LOG_WARNING, "\tFound %d entries for %s", howmuch, thrd->hostname);

		if (howmuch > 0 && thrd->cmd != NULL) {
			error1 = system(thrd->cmd);
			if (debug >= 2)
				syslog(LOG_WARNING, "Ran command %s with exit status %d because a dns change on hostname %s was detected.", thrd->cmd, error1, thrd->hostname);
		}
		if (howmuch != -1)
			filterdns_clean_table(thrd, 0);

		pthread_rwlock_unlock(&main_lock);
		/* Hack for sleeping a thread */
		pthread_cond_timedwait(&thrd->cond, &thrd->mtx, &ts);
		if (debug >= 6)
			syslog(LOG_WARNING, "\tAwaking from the sleep for hostname %s, table %s", thrd->hostname, thrd->tablename);
	}
	pthread_mutex_unlock(&thrd->mtx);

	filterdns_clean_table(thrd, 1);
	//flush_table(thrd->tablename);

	return (NULL);
}

static void
clear_hostname_addresses(struct thread_data *thr)
{
	struct table *a;

	if (TAILQ_EMPTY(&thr->rnh))
		return;

	filterdns_clean_table(thr, 1);

	while ((a = TAILQ_FIRST(&thr->rnh)) != NULL) {
		TAILQ_REMOVE(&thr->rnh, a, entry);
		if (a->addr)
			free(a->addr);
		free(a);
	}
}

static void *
merge_config(void *arg __unused) {
	struct thread_list tmp_thread_list;
	struct thread_data *thr, *tmpthr, *tmpthr2, *tmpthr3;
	int foundexisting, error;

	TAILQ_INIT(&tmp_thread_list);

	for (;;) {
		pthread_mutex_lock(&sig_mtx);
		error = pthread_cond_wait(&sig_condvar, &sig_mtx);
		if (error != 0) {
			syslog(LOG_ERR, "unable to wait on output queue retrying");
			continue;
		}
		pthread_mutex_unlock(&sig_mtx);

		pthread_rwlock_wrlock(&main_lock);

		if (!TAILQ_EMPTY(&thread_list)) {
			while ((thr = TAILQ_FIRST(&thread_list)) != NULL) {
				TAILQ_REMOVE(&thread_list, thr, next);
				TAILQ_INSERT_TAIL(&tmp_thread_list, thr, next);
			}
		}

		if (parse_config(file)) {
			syslog(LOG_ERR, "could not parse new configuration file, exiting...");
			exit(10);
		}

		if (!TAILQ_EMPTY(&thread_list)) {
			TAILQ_FOREACH_SAFE(thr, &thread_list, next, tmpthr2) {
				foundexisting = 0;

				TAILQ_FOREACH_SAFE(tmpthr, &tmp_thread_list, next, tmpthr3) {
					if (thr->type != tmpthr->type)
						continue;
					if (strlen(thr->hostname) != strlen(tmpthr->hostname))
						continue;
					if (strncmp(thr->hostname, tmpthr->hostname, strlen(thr->hostname)))
						continue;

					if (strlen(thr->tablename) != strlen(tmpthr->tablename) || strncmp(thr->tablename, tmpthr->tablename, strlen(thr->tablename)))
						continue;

					if (tmpthr->thr_pid == 0)
						continue;
					TAILQ_REMOVE(&thread_list, thr, next);
					TAILQ_REMOVE(&tmp_thread_list, tmpthr, next);
					TAILQ_INSERT_HEAD(&thread_list, tmpthr, next);
					TAILQ_INSERT_HEAD(&tmp_thread_list, thr, next);
					thr->thr_pid = 0;
					foundexisting = 1;
					break;
				}

				if (foundexisting == 0) {
					if (debug > 3)
						syslog(LOG_ERR, "Creating a new thread for host %s!", thr->hostname);
					error = pthread_create(&thr->thr_pid, &attr, check_hostname, thr);
					if (error != 0) {
						if (debug >= 1)
							syslog(LOG_ERR, "Unable to create monitoring thread for host %s! It will not be monitored!", thr->hostname);
					}
					pthread_set_name_np(thr->thr_pid, thr->hostname);
				}

			}
		}
		pthread_rwlock_unlock(&main_lock);

		if (debug > 3)
			syslog(LOG_ERR, "Cleaning up previous hostnames");
		clear_config(&tmp_thread_list);
	}
}

static void
handle_signal(int sig)
{
	if (debug >= 3)
		syslog(LOG_WARNING, "Received signal %s(%d).", (sig == SIGHUP) ? "SIGHUP" : (sig == SIGTERM) ? "SIGTERM" : "", sig);
	switch(sig) {
        case SIGHUP:
		pthread_mutex_lock(&sig_mtx);
		pthread_cond_signal(&sig_condvar);
		pthread_mutex_unlock(&sig_mtx);
                break;
        case SIGTERM:
		pthread_rwlock_wrlock(&main_lock);
		clear_config(&thread_list);
		pthread_rwlock_unlock(&main_lock);
		exit(0);
		break;
        default:
		if (debug >= 3)
                	syslog(LOG_WARNING, "unhandled signal");
        }
}

static void
clear_config(struct thread_list *thrlist)
{
	struct thread_data *thr;

	pthread_mutex_lock(&sig_mtx);
	while ((thr = TAILQ_FIRST(thrlist)) != NULL) {
		if (debug >= 4)
			syslog(LOG_ERR, "Cleaning up hostname %s", thr->hostname);
		TAILQ_REMOVE(thrlist, thr, next);
		if (thr->thr_pid != 0)
			pthread_cancel(thr->thr_pid);
		clear_hostname_addresses(thr);
		if (thr->hostname)
			free(thr->hostname);
		if (thr->tablename)
			free(thr->tablename);
		free(thr);
	}
	pthread_mutex_unlock(&sig_mtx);
}

static void filterdns_usage(void) {
	
	fprintf(stderr, "usage: filterdns -f -p pidfile -i interval -c filecfg -d debuglevel\n");
	exit(4);
}

int main(int argc, char *argv[]) {
	struct thread_data *thr;
	int error, ch;
	char *pidfile;
	FILE *pidfd;
	sig_t sig_error;
	int foreground = 0;

	file = NULL;
	pidfile = NULL;

	while ((ch = getopt(argc, argv, "c:d:fi:p:y:")) != -1) {
		switch (ch) {
		case 'c':
			file = optarg;
			break;
		case 'd':
			debug = atoi(optarg);
			break;
		case 'f':
			foreground = 1;
			break;
		case 'i':
			interval = atoi(optarg);
			if (interval < 1) {
				fprintf(stderr, "Invalid interval %d\n", interval);
				return (3);
			}
			break;
		case 'p':
			pidfile = optarg;
			break;
		case 'y':
			ipfwctx = optarg;
			break;
		default:
			fprintf(stderr, "Wrong option: %c given!\n", ch);
			filterdns_usage();
			return (ch);
			break;
		}
	}

	if (file == NULL) {
		fprintf(stderr, "Configuration file is mandatory!");
		filterdns_usage();
		return (-1);
	}

	if (foreground == 0) {
		(void)freopen("/dev/null", "w", stdout);
		(void)freopen("/dev/null", "w", stdin);
		closefrom(3);
	}

	TAILQ_INIT(&thread_list);
        if (parse_config(file)) {
                syslog(LOG_ERR, "unable to open configuration file");
                return (EX_OSERR);
        }

	dev = open("/dev/pf", O_RDWR);
	if (dev < 0)
		errx(1, "Could not open device.");

	/* go into background */
	if (!foreground && daemon(0, 0) == -1) {
		printf("error in daemon\n");
		exit(1);
	}

	if (!foreground && pidfile) {
		/* write PID to file */
		pidfd = fopen(pidfile, "w");
		if (pidfd) {
			while (flock(fileno(pidfd), LOCK_EX) != 0)
				;
			fprintf(pidfd, "%d\n", getpid());
			flock(fileno(pidfd), LOCK_UN);
			fclose(pidfd);
		} else
			syslog(LOG_WARNING, "could not open pid file");
	}

	/*
         * Catch SIGHUP in order to reread configuration file.
         */
	sig_error = signal(SIGHUP, handle_signal);
        if (sig_error == SIG_ERR)
                err(EX_OSERR, "unable to set signal handler");
        sig_error = signal(SIGTERM, handle_signal);
        if (sig_error == SIG_ERR)
                err(EX_OSERR, "unable to set signal handler");

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	TAILQ_FOREACH(thr, &thread_list, next) {
		error = pthread_create(&thr->thr_pid, &attr, check_hostname, thr);
		if (error != 0) {
			if (debug >= 1)
				syslog(LOG_ERR, "Unable to create monitoring thread for host %s", thr->hostname);
		}
		pthread_set_name_np(thr->thr_pid, thr->hostname);
	}

	pthread_rwlock_init(&main_lock, NULL);
	sig_mtx = PTHREAD_MUTEX_INITIALIZER;
        sig_condvar = PTHREAD_COND_INITIALIZER;
	error = pthread_create(&sig_thr, &attr, merge_config, NULL);
	if (error != 0) {
		if (debug >= 1)
			syslog(LOG_ERR, "Unable to create signal thread %s", thr->hostname);
	}
	pthread_set_name_np(sig_thr, "signal-thread");
#if 0
	TAILQ_FOREACH(thr, &thread_list, next)
		pthread_join(thr->thr_pid, NULL);
	
	clear_config(&thread_list);
#endif

	pthread_exit(NULL);
}
