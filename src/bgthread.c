#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <math.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <errno.h>

#include "vcl.h"
#include "cache/cache.h"
#include "cache/cache_director.h"

#include "vrt.h"
#include "vtim.h"
#include "vdir.h"
#include "vcc_disco_if.h"
#include "disco.h"

#ifndef DNS_RR_SRV
#define DNS_RR_SRV 33
#endif

#ifndef DNS_CLASS_INET
#define DNS_CLASS_INET 1
#endif

static void expand_srv(disco_t *d, unsigned sz)
{
  CHECK_OBJ_NOTNULL(d, VMOD_DISCO_DIRECTOR_MAGIC);

  d->srv = realloc(d->srv, sz * sizeof(*d->srv));
  d->l_srv = sz;
}


static inline int cmpsrv_sa(adns_sockaddr *s1, adns_sockaddr *s2)
{
  if (s1->sa.sa_family == s2->sa.sa_family) {
    switch(s1->sa.sa_family) {
    case AF_INET6:
      return memcmp(&s1->inet6.sin6_addr, &s2->inet6.sin6_addr, sizeof(s1->inet6.sin6_addr));
    case AF_INET:
      return memcmp(&s1->inet.sin_addr, &s2->inet.sin_addr, sizeof(s1->inet.sin_addr));
    default:
      assert("unsupported protocol family" == NULL);
    }
  }
  return -1;
}

static int cmpsrv(dns_srv_t *s1, adns_rr_srvha *s2)
{
  if (s2->ha.naddrs > 0)
    return (s1->priority == s2->priority &&
           s1->weight == s2->weight &&
           s1->port == s2->port) ?
           cmpsrv_sa(&s1->addr.addr, &s2->ha.addrs->addr) : 1;

  return -1;
}

static void dump_director(disco_t *d)
{
  unsigned u;
  dns_srv_t *s;
  char buf[256];
  int buflen = sizeof(buf);
  int p;

  CHECK_OBJ_NOTNULL(d, VMOD_DISCO_DIRECTOR_MAGIC);

  for (u = 0; u < d->n_srv; u++) {
    s = &d->srv[u];
    p = s->port;
    AZ(adns_addr2text(&s->addr.addr.sa, 0, buf, &buflen, &p));

    VSL(SLT_Debug, 0, "%s #%u: %hu %hu %hu %s:%d", d->name,
            u+1, s->priority, s->weight, s->port, buf, p);
  }
}

static void disco_thread_dnsresp(void *priv, disco_t *d, adns_answer *ans)
{
  unsigned u, w;
  struct vmod_disco *mod;
  dns_srv_t *s;
  adns_rr_addr *a;
  char *cp;
  const char *hp;

  CAST_OBJ_NOTNULL(mod, priv, VMOD_DISCO_MAGIC);
  CHECK_OBJ_NOTNULL(d, VMOD_DISCO_DIRECTOR_MAGIC);

  if (ans->nrrs == 0) {
    VSL(SLT_Debug, 0, adns_strerror(ans->status));
    free(ans);
    return;
  }

  assert(ans->type == adns_r_srv);

  while (d->l_srv <= ans->nrrs + d->n_srv) {
    expand_srv(d, d->l_srv + 16);
  }

  for (u = 0; u < d->n_srv; u++) {
    if (d->srv[u].port > 0) {
      for (w = 0; w < ans->nrrs; w++) {
        if (ans->rrs.srvha[w].ha.naddrs > 0 && cmpsrv(&d->srv[u], &ans->rrs.srvha[w]) == 0) {
          ans->rrs.srvha[w].ha.naddrs = 0;
          w = ans->nrrs+1;
          break;
        }
      }
      if (w <= ans->nrrs) {
        if (u < d->n_srv-1) {
          memcpy(&d->srv[u], &d->srv[u+1], ((d->n_srv-1) - u) * sizeof(adns_rr_srvha));
          u--;
        }
        d->changes++;
        d->n_srv--;
      }
    }
  }

  for (u = 0; u < ans->nrrs; u++) {
    a = ans->rrs.srvha[u].ha.addrs;
    AN(a);

    if (ans->rrs.srvha[u].ha.naddrs > 0) {
      s = &d->srv[d->n_srv];
      d->n_srv++;
      d->changes++;
      assert(d->n_srv < d->l_srv);
      memset(s, 0, sizeof(*s));
      cp = &s->name[0];
      for (hp = ans->rrs.srvha[u].ha.host; (hp && *hp) && *hp != '.'; hp++) {
        *cp++ = *hp;
        if(cp - &s->name[0] >= sizeof(s->name)-1)
          break;
      }
      *cp = '\0';
      s->priority = ans->rrs.srvha[u].priority;
      s->weight = ans->rrs.srvha[u].weight;
      s->port = ans->rrs.srvha[u].port;
      memcpy(&s->addr, ans->rrs.srvha[u].ha.addrs, sizeof(s->addr));
      switch (s->addr.addr.sa.sa_family) {
      case AF_INET6:
        s->addr.addr.inet6.sin6_port = htons(s->port);
        break;
      case AF_INET:
        s->addr.addr.inet.sin_port = htons(s->port);
        break;
      default:
        assert("unsupported address family" == NULL);
      }
    }
  }

  free(ans);
  if (d->n_srv > 0)
    dump_director(d);
}

static double disco_thread_run(struct worker *wrk,
                             struct vmod_disco_bgthread *bg,
                             double now)
{
  char *name;
  size_t l;
  unsigned u;
  double interval;
  struct vmod_disco *mod;
  disco_t *d;

  CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
  CHECK_OBJ_NOTNULL(bg, VMOD_DISCO_BGTHREAD_MAGIC);
  CAST_OBJ_NOTNULL(mod, bg->priv, VMOD_DISCO_MAGIC);
  CHECK_OBJ_NOTNULL(bg->ws, WS_MAGIC);

  Lck_AssertHeld(&bg->mtx);
  (void)wrk;
  interval = bg->interval;
  AZ(pthread_rwlock_wrlock(&mod->mtx));
  VTAILQ_FOREACH(d, &mod->dirs, list) {
    CHECK_OBJ_NOTNULL(d, VMOD_DISCO_DIRECTOR_MAGIC);
    if (d->query) {
      adns_answer *ans = NULL;
      void *ctx = NULL;

      switch(adns_check(bg->dns, &d->query, &ans, &ctx)) {
      case ESRCH:
      case EAGAIN:
        interval = 1e-2;
        break;
      case 0:
        d->query = NULL;
        disco_thread_dnsresp(ctx, d, ans);
        d->nxt = now + d->freq;
      }
      continue;
    }
    if (d->nxt > now)
      continue;
    d->nxt = now + d->freq;
    AN(bg->dns);
    u = WS_Reserve(bg->ws, 0);
    l = strlen(d->name);
    assert(u > l+2);
    name = strncpy(bg->ws->f, d->name, u-1);
    *(name + l) = '.';
    *(name + l + 1) = '\0';
    AZ(adns_submit(bg->dns, name, adns_r_srv|adns__qtf_bigaddr,
       adns_qf_want_allaf|adns_qf_quoteok_query|adns_qf_cname_loose,
       mod, &d->query));
    interval = 1e-3;

    VSL(SLT_Debug, 0, "req for srv for '%s sent", name);
    WS_Release(bg->ws, 0);
  }
  AZ(pthread_rwlock_unlock(&mod->mtx));
  adns_processany(bg->dns);
  return now + interval;
}

static void * __match_proto__(bgthread_t)
disco_thread(struct worker *wrk, void *priv)
{
  struct vmod_disco_bgthread *bg;
  unsigned gen, shutdown;
  double d;

  CAST_OBJ_NOTNULL(bg, priv, VMOD_DISCO_BGTHREAD_MAGIC);
  (void)wrk;

  gen = 0;
  shutdown = 0;
  AZ(adns_init(&bg->dns, adns_if_nosigpipe|adns_if_permit_ipv4|adns_if_permit_ipv6, NULL));
  while (!shutdown) {
    Lck_Lock(&bg->mtx);
    d = disco_thread_run(wrk, bg, VTIM_real());
    Lck_AssertHeld(&bg->mtx);
    Lck_Unlock(&bg->mtx);
    adns_processany(bg->dns);
    Lck_Lock(&bg->mtx);
    if (gen == bg->gen) {
      (void)Lck_CondWait(&bg->cond, &bg->mtx, d);
    }
    gen = bg->gen;
    shutdown = bg->shutdown;
    Lck_Unlock(&bg->mtx);
    if (!shutdown) {
      adns_processany(bg->dns);
    }
  }
  VSL(SLT_Debug, 0, "disco thread shutdown");
  Lck_Lock(&bg->mtx);
  bg->gen = 0;
  adns_finish(bg->dns);
  AZ(pthread_cond_signal(&bg->cond));
  Lck_Unlock(&bg->mtx);
  pthread_exit(0);
  return NULL;
}

void vmod_disco_bgthread_start(struct vmod_disco_bgthread **wrkp, void *priv, unsigned interval)
{
  struct vmod_disco_bgthread *wrk;
  unsigned char *s;

  ALLOC_OBJ(wrk, VMOD_DISCO_BGTHREAD_MAGIC);
  AN(wrk);

  wrk->ws = (struct ws*)PRNDUP(&wrk->__scratch[0]);
  s  = (unsigned char*)wrk->ws + PRNDUP(sizeof(struct ws));
  WS_Init(wrk->ws, "mii", s, sizeof(wrk->__scratch) - (s - &wrk->__scratch[0]));

  Lck_New(&wrk->mtx, lck_vcl);
  AZ(pthread_cond_init(&wrk->cond, NULL));
  wrk->gen = 1;
  wrk->interval = interval;
  wrk->priv = priv;
  WRK_BgThread(&wrk->thr, "disco", disco_thread, wrk);
  if (wrkp) {
    *wrkp = wrk;
  }
}

void vmod_disco_bgthread_kick(struct vmod_disco_bgthread *wrk, unsigned shutdown)
{
  CHECK_OBJ_NOTNULL(wrk, VMOD_DISCO_BGTHREAD_MAGIC);

  Lck_Lock(&wrk->mtx);
  if(!wrk->gen) {
    AN(shutdown);
    Lck_Unlock(&wrk->mtx);
    return;
  }
  wrk->gen++;
  if (shutdown) {
    wrk->shutdown++;
  }
  AZ(pthread_cond_signal(&wrk->cond));
  Lck_Unlock(&wrk->mtx);
  AZ(pthread_join(wrk->thr, NULL));
}

void vmod_disco_bgthread_delete(struct vmod_disco_bgthread **wrkp)
{
  struct vmod_disco_bgthread *bg;

  bg = *wrkp;
  *wrkp = NULL;

  CHECK_OBJ_NOTNULL(bg, VMOD_DISCO_BGTHREAD_MAGIC);
  if (bg->gen) {
    vmod_disco_bgthread_kick(bg, 1);
  }
  AZ(bg->gen);
  AZ(pthread_cond_destroy(&bg->cond));
  Lck_Delete(&bg->mtx);
  FREE_OBJ(bg);
}
