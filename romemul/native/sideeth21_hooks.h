/* Included by lwIP before IPv4 dispatch. F21 only; no legacy build change. */
#ifndef SIDETNFS_SIDEETH21_HOOKS_H
#define SIDETNFS_SIDEETH21_HOOKS_H
struct pbuf;
struct netif;
int s21_nat_ip4_input(struct pbuf *p,struct netif *inp);
#define LWIP_HOOK_IP4_INPUT(p,inp) s21_nat_ip4_input((p),(inp))
#endif
