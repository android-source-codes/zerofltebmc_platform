/* dnsmasq is Copyright (c) 2000-2012 Simon Kelley

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 dated June, 1991, or
   (at your option) version 3 dated 29 June, 2007.
 
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
     
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dnsmasq.h"

#ifdef HAVE_LINUX_NETWORK

#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

/* linux 2.6.19 buggers up the headers, patch it up here. */ 
#ifndef IFA_RTA
#  define IFA_RTA(r)  \
       ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct ifaddrmsg))))

#  include <linux/if_addr.h>
#endif

#ifndef NDA_RTA
#  define NDA_RTA(r) ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg)))) 
#endif 


static struct iovec iov;
static u32 netlink_pid;

static void nl_err(struct nlmsghdr *h);
static void nl_routechange(struct nlmsghdr *h);
static void nl_newaddr(struct nlmsghdr *h);

void netlink_init(void)
{
  struct sockaddr_nl addr;
  socklen_t slen = sizeof(addr);

  addr.nl_family = AF_NETLINK;
  addr.nl_pad = 0;
  addr.nl_pid = 0; /* autobind */
#ifdef HAVE_IPV6
  addr.nl_groups = RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | RTMGRP_IPV6_IFADDR;
#else
  addr.nl_groups = RTMGRP_IPV4_ROUTE;
#endif
  
  /* May not be able to have permission to set multicast groups don't die in that case */
  if ((daemon->netlinkfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) != -1)
    {
      if (bind(daemon->netlinkfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
	{
	  addr.nl_groups = 0;
	  if (errno != EPERM || bind(daemon->netlinkfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
	    daemon->netlinkfd = -1;
	}
    }
  
  if (daemon->netlinkfd == -1 || 
      getsockname(daemon->netlinkfd, (struct sockaddr *)&addr, &slen) == 1)
    die(_("cannot create netlink socket: %s"), NULL, EC_MISC);
   
  /* save pid assigned by bind() and retrieved by getsockname() */ 
  netlink_pid = addr.nl_pid;
  
  iov.iov_len = 100;
  iov.iov_base = safe_malloc(iov.iov_len);
}

static ssize_t netlink_recv(void)
{
  struct msghdr msg;
  struct sockaddr_nl nladdr;
  ssize_t rc;

  while (1)
    {
      msg.msg_control = NULL;
      msg.msg_controllen = 0;
      msg.msg_name = &nladdr;
      msg.msg_namelen = sizeof(nladdr);
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_flags = 0;
      
      while ((rc = recvmsg(daemon->netlinkfd, &msg, MSG_PEEK | MSG_TRUNC)) == -1 && errno == EINTR);
      
      /* make buffer big enough */
      if (rc != -1 && (msg.msg_flags & MSG_TRUNC))
	{
	  /* Very new Linux kernels return the actual size needed, older ones always return truncated size */
	  if ((size_t)rc == iov.iov_len)
	    {
	      if (expand_buf(&iov, rc + 100))
		continue;
	    }
	  else
	    expand_buf(&iov, rc);
	}

      /* read it for real */
      msg.msg_flags = 0;
      while ((rc = recvmsg(daemon->netlinkfd, &msg, 0)) == -1 && errno == EINTR);
      
      /* Make sure this is from the kernel */
      if (rc == -1 || nladdr.nl_pid == 0)
	break;
    }
      
  /* discard stuff which is truncated at this point (expand_buf() may fail) */
  if (msg.msg_flags & MSG_TRUNC)
    {
      rc = -1;
      errno = ENOMEM;
    }
  
  return rc;
}
  

/* family = AF_UNSPEC finds ARP table entries.
   family = AF_LOCAL finds MAC addresses. */
int iface_enumerate(int family, void *parm, int (*callback)())
{
  struct sockaddr_nl addr;
  struct nlmsghdr *h;
  ssize_t len;
  static unsigned int seq = 0;
  int callback_ok = 1;

  struct {
    struct nlmsghdr nlh;
    struct rtgenmsg g; 
  } req;

  addr.nl_family = AF_NETLINK;
  addr.nl_pad = 0;
  addr.nl_groups = 0;
  addr.nl_pid = 0; /* address to kernel */
 
 again: 
  if (family == AF_UNSPEC)
    req.nlh.nlmsg_type = RTM_GETNEIGH;
  else if (family == AF_LOCAL)
    req.nlh.nlmsg_type = RTM_GETLINK;
  else
    req.nlh.nlmsg_type = RTM_GETADDR;

  req.nlh.nlmsg_len = sizeof(req);
  req.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH | NLM_F_REQUEST | NLM_F_ACK; 
  req.nlh.nlmsg_pid = 0;
  req.nlh.nlmsg_seq = ++seq;
  req.g.rtgen_family = family; 

  /* Don't block in recvfrom if send fails */
  while((len = sendto(daemon->netlinkfd, (void *)&req, sizeof(req), 0, 
		      (struct sockaddr *)&addr, sizeof(addr))) == -1 && retry_send());
  
  if (len == -1)
    return 0;
    
  while (1)
    {
      if ((len = netlink_recv()) == -1)
	{
	  if (errno == ENOBUFS)
	    {
	      sleep(1);
	      goto again;
	    }
	  return 0;
	}

      for (h = (struct nlmsghdr *)iov.iov_base; NLMSG_OK(h, (size_t)len); h = NLMSG_NEXT(h, len))
        if (h->nlmsg_type == RTM_NEWROUTE && h->nlmsg_seq != seq || h->nlmsg_pid != netlink_pid)
          nl_routechange(h); /* May be multicast arriving async */
        else if (h->nlmsg_type == NLMSG_ERROR)
          nl_err(h);
	else if (h->nlmsg_type == NLMSG_DONE)
	  return callback_ok;
	else if (h->nlmsg_type == RTM_NEWADDR && family != AF_UNSPEC && family != AF_LOCAL)
	  {
	    struct ifaddrmsg *ifa = NLMSG_DATA(h);  
	    struct rtattr *rta = IFA_RTA(ifa);
	    unsigned int len1 = h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa));
	    
	    if (ifa->ifa_family == family)
	      {
		if (ifa->ifa_family == AF_INET)
		  {
		    struct in_addr netmask, addr, broadcast;
		    
		    netmask.s_addr = htonl(0xffffffff << (32 - ifa->ifa_prefixlen));
		    addr.s_addr = 0;
		    broadcast.s_addr = 0;
		    
		    while (RTA_OK(rta, len1))
		      {
			if (rta->rta_type == IFA_LOCAL)
			  addr = *((struct in_addr *)(rta+1));
			else if (rta->rta_type == IFA_BROADCAST)
			  broadcast = *((struct in_addr *)(rta+1));
			
			rta = RTA_NEXT(rta, len1);
		      }
		    
		    if (addr.s_addr && callback_ok)
		      if (!((*callback)(addr, ifa->ifa_index, netmask, broadcast, parm)))
			callback_ok = 0;
		  }
#ifdef HAVE_IPV6
		else if (ifa->ifa_family == AF_INET6)
		  {
		    struct in6_addr *addrp = NULL;
		    while (RTA_OK(rta, len1))
		      {
			if (rta->rta_type == IFA_ADDRESS)
			  addrp = ((struct in6_addr *)(rta+1)); 
			
			rta = RTA_NEXT(rta, len1);
		      }
		    
		    if (addrp && callback_ok)
		      if (!((*callback)(addrp, (int)(ifa->ifa_prefixlen), (int)(ifa->ifa_scope), 
					(int)(ifa->ifa_index), (int)(ifa->ifa_flags & IFA_F_TENTATIVE), parm)))
			callback_ok = 0;
		  }
#endif
	      }
	  }
	else if (h->nlmsg_type == RTM_NEWNEIGH && family == AF_UNSPEC)
	  {
	    struct ndmsg *neigh = NLMSG_DATA(h);  
	    struct rtattr *rta = NDA_RTA(neigh);
	    unsigned int len1 = h->nlmsg_len - NLMSG_LENGTH(sizeof(*neigh));
	    size_t maclen = 0;
	    char *inaddr = NULL, *mac = NULL;
	    
	    while (RTA_OK(rta, len1))
	      {
		if (rta->rta_type == NDA_DST)
		  inaddr = (char *)(rta+1);
		else if (rta->rta_type == NDA_LLADDR)
		  {
		    maclen = rta->rta_len - sizeof(struct rtattr);
		    mac = (char *)(rta+1);
		  }
		
		rta = RTA_NEXT(rta, len1);
	      }

	    if (inaddr && mac && callback_ok)
	      if (!((*callback)(neigh->ndm_family, inaddr, mac, maclen, parm)))
		callback_ok = 0;
	  }
#ifdef HAVE_DHCP6
	else if (h->nlmsg_type == RTM_NEWLINK && family == AF_LOCAL)
	  {
	    struct ifinfomsg *link =  NLMSG_DATA(h);
	    struct rtattr *rta = IFLA_RTA(link);
	    unsigned int len1 = h->nlmsg_len - NLMSG_LENGTH(sizeof(*link));
	    char *mac = NULL;
	    size_t maclen = 0;

	    while (RTA_OK(rta, len1))
	      {
		if (rta->rta_type == IFLA_ADDRESS)
		  {
		    maclen = rta->rta_len - sizeof(struct rtattr);
		    mac = (char *)(rta+1);
		  }
		
		rta = RTA_NEXT(rta, len1);
	      }

	    if (mac && callback_ok && !((link->ifi_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) && 
		!((*callback)((int)link->ifi_index, (unsigned int)link->ifi_type, mac, maclen, parm)))
	      callback_ok = 0;
	  }
#endif
    }
}

void netlink_multicast(void)
{
  ssize_t len;
  struct nlmsghdr *h;
  int flags;
  
  /* don't risk blocking reading netlink messages here. */
  if ((flags = fcntl(daemon->netlinkfd, F_GETFL)) == -1 ||
      fcntl(daemon->netlinkfd, F_SETFL, flags | O_NONBLOCK) == -1) 
    return;
  
  if ((len = netlink_recv()) != -1)
    {
      for (h = (struct nlmsghdr *)iov.iov_base; NLMSG_OK(h, (size_t)len); h = NLMSG_NEXT(h, len))
	if (h->nlmsg_type == NLMSG_ERROR)
	  nl_err(h);
	else if (h->nlmsg_type == RTM_NEWADDR)
		nl_newaddr(h);
        else if (h->nlmsg_type == RTM_NEWROUTE)
		nl_routechange(h);
    }

  /* restore non-blocking status */
  fcntl(daemon->netlinkfd, F_SETFL, flags); 
}

static void nl_err(struct nlmsghdr *h)
{
  struct nlmsgerr *err = NLMSG_DATA(h);
  
  if (err->error != 0)
    my_syslog(LOG_ERR, _("netlink returns error: %s"), strerror(-(err->error)));
}

/* We arrange to receive netlink multicast messages whenever the network route is added.
   If this happens and we still have a DNS packet in the buffer, we re-send it.
   This helps on DoD links, where frequently the packet which triggers dialling is
   a DNS query, which then gets lost. By re-sending, we can avoid the lookup
   failing. Note that we only accept these messages from the kernel (pid == 0) */ 
static void nl_routechange(struct nlmsghdr *h)
{
  if (h->nlmsg_pid == 0)
    {
      struct rtmsg *rtm = NLMSG_DATA(h);
      int fd;

      if (rtm->rtm_type != RTN_UNICAST || rtm->rtm_scope != RT_SCOPE_LINK)
	return;

      /* Force re-reading resolv file right now, for luck. */
      daemon->last_resolv = 0;
      
#ifdef HAVE_DHCP6
      /* force RAs to sync new network and pick up new interfaces.  */
      if (daemon->ra_contexts)
	{
	  schedule_subnet_map();
	  ra_start_unsolicted(dnsmasq_time(), NULL);
	  /* cause lease_update_file to run after we return, in case we were called from
	     iface_enumerate and can't re-enter it now */
	  send_alarm(0, 0);
	}
#endif

      if (daemon->srv_save)
	{
	  if (daemon->srv_save->sfd)
	    fd = daemon->srv_save->sfd->fd;
	  else if (daemon->rfd_save && daemon->rfd_save->refcount != 0)
	    fd = daemon->rfd_save->fd;
	  else
	    return;
	  
	  while(sendto(fd, daemon->packet, daemon->packet_len, 0,
		       &daemon->srv_save->addr.sa, sa_len(&daemon->srv_save->addr)) == -1 && retry_send()); 
	}
    }
}

static void nl_newaddr(struct nlmsghdr* h)
{
   struct ifaddrmsg* _ifaddrmsg;
   int ifaddrmsg_len;
   struct rtattr* _rtattr;

   _ifaddrmsg = (struct ifaddrmsg *) NLMSG_DATA(h);
   ifaddrmsg_len = IFA_PAYLOAD(h);
   _rtattr = IFA_RTA(_ifaddrmsg);
   char buff[IFNAMSIZ];


   char *ifs_name = if_indextoname(_ifaddrmsg->ifa_index, buff);

   const char *interfaces[] = {"bt-pan", "rndis0", "wlan0", "swlan0"};
   int ifs_size = 4;
   int i;
   int iil = 0;

   for( i = 0; i < ifs_size; ++i)
       if(strcmp(interfaces[i], ifs_name) == 0)
           iil = 1;
   if(!iil)
       return;

    for (_rtattr = IFA_RTA(_ifaddrmsg); RTA_OK(_rtattr, ifaddrmsg_len); _rtattr = RTA_NEXT(_rtattr, ifaddrmsg_len)) {
        if (_rtattr->rta_type == IFA_ADDRESS && _ifaddrmsg->ifa_family == AF_INET6) {
            struct in6_addr* ipv6 = (struct in6_addr*) RTA_DATA(_rtattr);
            if (RTA_PAYLOAD(_rtattr) < sizeof(*ipv6))
                continue;
            else if (RTA_PAYLOAD(_rtattr) == sizeof(struct in6_addr)) {

                if (iface_check_multicast_group(ifs_name, ALL_RELAY_AGENTS_AND_SERVERS) == 0)
                    iface_join_multicast(buff, ALL_RELAY_AGENTS_AND_SERVERS, 1);
                }
        }
    }
}
#endif

      
