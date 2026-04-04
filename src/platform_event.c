/* platform_event.c: portable event loop (epoll on Linux, kqueue on macOS) */
#include "platform_event.h"
#include <errno.h>
#include <unistd.h>

#ifdef AIMEE_LINUX
/* ---- Linux: epoll ---- */

#include <sys/epoll.h>

int platform_evloop_create(platform_evloop_t *loop)
{
   loop->epoll_fd = epoll_create1(0);
   return (loop->epoll_fd < 0) ? -1 : 0;
}

void platform_evloop_destroy(platform_evloop_t *loop)
{
   if (loop->epoll_fd >= 0)
   {
      close(loop->epoll_fd);
      loop->epoll_fd = -1;
   }
}

static uint32_t to_epoll(uint32_t flags)
{
   uint32_t ev = 0;
   if (flags & PLAT_EV_IN)
      ev |= EPOLLIN;
   if (flags & PLAT_EV_OUT)
      ev |= EPOLLOUT;
   return ev;
}

static uint32_t from_epoll(uint32_t ev)
{
   uint32_t flags = 0;
   if (ev & EPOLLIN)
      flags |= PLAT_EV_IN;
   if (ev & EPOLLOUT)
      flags |= PLAT_EV_OUT;
   if (ev & EPOLLERR)
      flags |= PLAT_EV_ERR;
   if (ev & EPOLLHUP)
      flags |= PLAT_EV_HUP;
   return flags;
}

int platform_evloop_add(platform_evloop_t *loop, int fd, uint32_t events)
{
   struct epoll_event ev = {.events = to_epoll(events), .data.fd = fd};
   return epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

int platform_evloop_mod(platform_evloop_t *loop, int fd, uint32_t events)
{
   struct epoll_event ev = {.events = to_epoll(events), .data.fd = fd};
   return epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

int platform_evloop_del(platform_evloop_t *loop, int fd)
{
   return epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

int platform_evloop_wait(platform_evloop_t *loop, platform_event_t *out, int max_events,
                         int timeout_ms)
{
   struct epoll_event raw[64];
   if (max_events > 64)
      max_events = 64;

   int n = epoll_wait(loop->epoll_fd, raw, max_events, timeout_ms);
   if (n < 0)
      return (errno == EINTR) ? 0 : -1;

   for (int i = 0; i < n; i++)
   {
      out[i].fd = raw[i].data.fd;
      out[i].events = from_epoll(raw[i].events);
   }
   return n;
}

#elif defined(AIMEE_MACOS)
/* ---- macOS: kqueue ---- */

#include <sys/event.h>
#include <sys/time.h>
#include <stdlib.h>

int platform_evloop_create(platform_evloop_t *loop)
{
   loop->kqueue_fd = kqueue();
   return (loop->kqueue_fd < 0) ? -1 : 0;
}

void platform_evloop_destroy(platform_evloop_t *loop)
{
   if (loop->kqueue_fd >= 0)
   {
      close(loop->kqueue_fd);
      loop->kqueue_fd = -1;
   }
}

static int kq_register(int kqfd, int fd, int16_t filter, uint16_t flags)
{
   struct kevent kev;
   EV_SET(&kev, fd, filter, flags, 0, 0, NULL);
   return kevent(kqfd, &kev, 1, NULL, 0, NULL);
}

int platform_evloop_add(platform_evloop_t *loop, int fd, uint32_t events)
{
   int rc = 0;
   if (events & PLAT_EV_IN)
      rc |= kq_register(loop->kqueue_fd, fd, EVFILT_READ, EV_ADD | EV_ENABLE);
   if (events & PLAT_EV_OUT)
      rc |= kq_register(loop->kqueue_fd, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE);
   return rc;
}

int platform_evloop_mod(platform_evloop_t *loop, int fd, uint32_t events)
{
   /* kqueue: disable both, then re-enable what we want */
   kq_register(loop->kqueue_fd, fd, EVFILT_READ, EV_DELETE);
   kq_register(loop->kqueue_fd, fd, EVFILT_WRITE, EV_DELETE);
   return platform_evloop_add(loop, fd, events);
}

int platform_evloop_del(platform_evloop_t *loop, int fd)
{
   kq_register(loop->kqueue_fd, fd, EVFILT_READ, EV_DELETE);
   kq_register(loop->kqueue_fd, fd, EVFILT_WRITE, EV_DELETE);
   return 0;
}

int platform_evloop_wait(platform_evloop_t *loop, platform_event_t *out, int max_events,
                         int timeout_ms)
{
   struct kevent raw[64];
   if (max_events > 64)
      max_events = 64;

   struct timespec ts;
   struct timespec *tsp = NULL;
   if (timeout_ms >= 0)
   {
      ts.tv_sec = timeout_ms / 1000;
      ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
      tsp = &ts;
   }

   int n = kevent(loop->kqueue_fd, NULL, 0, raw, max_events, tsp);
   if (n < 0)
      return (errno == EINTR) ? 0 : -1;

   /* Coalesce kqueue events by fd (read+write may come as separate events) */
   int count = 0;
   for (int i = 0; i < n; i++)
   {
      int fd = (int)raw[i].ident;
      uint32_t flags = 0;

      if (raw[i].filter == EVFILT_READ)
         flags |= PLAT_EV_IN;
      if (raw[i].filter == EVFILT_WRITE)
         flags |= PLAT_EV_OUT;
      if (raw[i].flags & EV_EOF)
         flags |= PLAT_EV_HUP;
      if (raw[i].flags & EV_ERROR)
         flags |= PLAT_EV_ERR;

      /* Try to merge with existing entry for same fd */
      int merged = 0;
      for (int j = 0; j < count; j++)
      {
         if (out[j].fd == fd)
         {
            out[j].events |= flags;
            merged = 1;
            break;
         }
      }
      if (!merged && count < max_events)
      {
         out[count].fd = fd;
         out[count].events = flags;
         count++;
      }
   }
   return count;
}

#elif defined(AIMEE_WINDOWS)
/* ---- Windows: WSAPoll stub (to be implemented in Phase 2) ---- */

int platform_evloop_create(platform_evloop_t *loop)
{
   (void)loop;
   return -1; /* Not yet implemented */
}

void platform_evloop_destroy(platform_evloop_t *loop)
{
   (void)loop;
}

int platform_evloop_add(platform_evloop_t *loop, int fd, uint32_t events)
{
   (void)loop;
   (void)fd;
   (void)events;
   return -1;
}

int platform_evloop_mod(platform_evloop_t *loop, int fd, uint32_t events)
{
   (void)loop;
   (void)fd;
   (void)events;
   return -1;
}

int platform_evloop_del(platform_evloop_t *loop, int fd)
{
   (void)loop;
   (void)fd;
   return -1;
}

int platform_evloop_wait(platform_evloop_t *loop, platform_event_t *out, int max_events,
                         int timeout_ms)
{
   (void)loop;
   (void)out;
   (void)max_events;
   (void)timeout_ms;
   return -1;
}

#endif
