// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <module/module.h>

#include <net/types.h>

int ret_net_errno(EmuEnvState &emuenv, int thread_id, int ret);

DECL_EXPORT(int, sceNetAccept, int sid, SceNetSockaddr *addr, unsigned int *addrlen);
DECL_EXPORT(int, sceNetBind, int sid, const SceNetSockaddr *addr, unsigned int addrlen);
DECL_EXPORT(int, sceNetConnect, int sid, const SceNetSockaddr *addr, unsigned int addrlen);
DECL_EXPORT(int, sceNetEpollControl, int eid, SceNetEpollControlFlag op, int id, SceNetEpollEvent *ev);
DECL_EXPORT(int, sceNetEpollCreate, const char *name, int flags);
DECL_EXPORT(int, sceNetEpollDestroy, int eid);
DECL_EXPORT(int, sceNetEpollWait, int eid, SceNetEpollEvent *events, int maxevents, int timeout);
DECL_EXPORT(int, sceNetGetMacAddress, SceNetEtherAddr *addr, int flags);
DECL_EXPORT(int, sceNetGetpeername, int sid, SceNetSockaddr *addr, unsigned int *addrlen);
DECL_EXPORT(int, sceNetGetsockname, int sid, SceNetSockaddr *addr, unsigned int *addrlen);
DECL_EXPORT(int, sceNetGetsockopt, int sid, int level, int optname, void *optval, unsigned int *optlen);
DECL_EXPORT(int, sceNetInetPton, int af, const char *src, void *dst);
DECL_EXPORT(int, sceNetListen, int sid, int backlog);
DECL_EXPORT(int, sceNetRecvfrom, int sid, void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen);
DECL_EXPORT(int, sceNetRecvmsg, int sid, SceNetMsghdr *msg, int flags);
DECL_EXPORT(int, sceNetSendmsg, int sid, const SceNetMsghdr *msg, int flags);
DECL_EXPORT(int, sceNetSendto, int sid, const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen);
DECL_EXPORT(int, sceNetSetsockopt, int sid, SceNetProtocol level, SceNetSocketOption optname, const void *optval, unsigned int optlen);
DECL_EXPORT(int, sceNetShutdown, int sid, int how);
DECL_EXPORT(int, sceNetSocket, const char *name, int domain, SceNetSocketType type, SceNetProtocol protocol);
DECL_EXPORT(int, sceNetSocketAbort, int sid, int flags);
DECL_EXPORT(int, sceNetSocketClose, int sid);
