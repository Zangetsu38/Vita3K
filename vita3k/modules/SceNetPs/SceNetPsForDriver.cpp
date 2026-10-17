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

#include <module/module.h>

#include "../SceNet/SceNet.h"

#include <util/tracy.h>

#include <cstdint>

TRACY_MODULE_NAME(SceNetPsForDriver);

EXPORT(int, SceNetPsForDriver_sceNetListen, int socket, int backlog) {
    TRACY_FUNC(SceNetPsForDriver_sceNetListen, socket, backlog);
    LOG_INFO("SceNetPsForDriver sceNetListen socket={} backlog={}", socket, backlog);
    return CALL_EXPORT(sceNetListen, socket, backlog);
}

EXPORT(int, SceNetPsForDriver_sceNetConnect, int socket, const SceNetSockaddr *address, unsigned int address_length) {
    TRACY_FUNC(SceNetPsForDriver_sceNetConnect, socket, address, address_length);
    LOG_INFO("SceNetPsForDriver sceNetConnect socket={} address={} address_length={}",
        socket, log_hex(reinterpret_cast<uintptr_t>(address)), address_length);
    return CALL_EXPORT(sceNetConnect, socket, address, address_length);
}

EXPORT(int, SceNetPsForDriver_1ABF937D, Ptr<void> driver) {
    TRACY_FUNC(SceNetPsForDriver_1ABF937D, driver);
    LOG_INFO("SceNetPsForDriver_1ABF937D driver={}", log_hex(driver.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetPsForDriver_21F4428D, int socket) {
    TRACY_FUNC(SceNetPsForDriver_21F4428D, socket);
    LOG_INFO("SceNetPsForDriver_21F4428D socket={}", socket);
    return CALL_EXPORT(sceNetSocketClose, socket);
}

EXPORT(int, SceNetKernelGateway, Ptr<void> interface) {
    TRACY_FUNC(SceNetKernelGateway, interface);
    LOG_INFO("SceNetKernelGateway interface={}", log_hex(interface.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetPsForDriver_sceNetRecvfrom, int socket, void *buffer, unsigned int length, int flags,
    SceNetSockaddr *address, unsigned int *address_length) {
    TRACY_FUNC(SceNetPsForDriver_sceNetRecvfrom, socket, buffer, length, flags, address, address_length);
    LOG_INFO("SceNetPsForDriver sceNetRecvfrom socket={} buffer={} length={} flags={} address={} address_length={}",
        socket, log_hex(reinterpret_cast<uintptr_t>(buffer)), length, flags,
        log_hex(reinterpret_cast<uintptr_t>(address)), log_hex(reinterpret_cast<uintptr_t>(address_length)));
    return CALL_EXPORT(sceNetRecvfrom, socket, buffer, length, flags, address, address_length);
}

EXPORT(int, SceNetPsForDriver_sceNetSetsockopt, int socket, int level, int option, const void *value, unsigned int length) {
    TRACY_FUNC(SceNetPsForDriver_sceNetSetsockopt, socket, level, option, value, length);
    LOG_INFO("SceNetPsForDriver sceNetSetsockopt socket={} level={} option={} value={} length={}",
        socket, level, option, log_hex(reinterpret_cast<uintptr_t>(value)), length);
    return CALL_EXPORT(sceNetSetsockopt, socket, static_cast<SceNetProtocol>(level),
        static_cast<SceNetSocketOption>(option), value, length);
}

EXPORT(int, SceNetPsForDriver_83A12CCC, Ptr<void> client) {
    TRACY_FUNC(SceNetPsForDriver_83A12CCC, client);
    LOG_INFO("SceNetPsForDriver_83A12CCC client={}", log_hex(client.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetPsForDriver_sceNetBind, int socket, const SceNetSockaddr *address, unsigned int address_length) {
    TRACY_FUNC(SceNetPsForDriver_sceNetBind, socket, address, address_length);
    LOG_INFO("SceNetPsForDriver sceNetBind socket={} address={} address_length={}",
        socket, log_hex(reinterpret_cast<uintptr_t>(address)), address_length);
    return CALL_EXPORT(sceNetBind, socket, address, address_length);
}

EXPORT(int, SceNetPsForDriver_sceNetAccept, int socket, SceNetSockaddr *address, unsigned int *address_length) {
    TRACY_FUNC(SceNetPsForDriver_sceNetAccept, socket, address, address_length);
    LOG_INFO("SceNetPsForDriver sceNetAccept socket={} address={} address_length={}", socket,
        log_hex(reinterpret_cast<uintptr_t>(address)), log_hex(reinterpret_cast<uintptr_t>(address_length)));
    return CALL_EXPORT(sceNetAccept, socket, address, address_length);
}

EXPORT(int, sceNetPsUnregisterGateway, Ptr<void> interface) {
    TRACY_FUNC(sceNetPsUnregisterGateway, interface);
    LOG_INFO("sceNetPsUnregisterGateway interface={}", log_hex(interface.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetPsForDriver_sceNetGetsockname, int socket, SceNetSockaddr *address, unsigned int *address_length) {
    TRACY_FUNC(SceNetPsForDriver_sceNetGetsockname, socket, address, address_length);
    LOG_INFO("SceNetPsForDriver sceNetGetsockname socket={} address={} address_length={}", socket,
        log_hex(reinterpret_cast<uintptr_t>(address)), log_hex(reinterpret_cast<uintptr_t>(address_length)));
    return CALL_EXPORT(sceNetGetsockname, socket, address, address_length);
}

EXPORT(int, SceNetPsForDriver_sceNetGetsockopt, int socket, int level, int option, void *value, unsigned int *length) {
    TRACY_FUNC(SceNetPsForDriver_sceNetGetsockopt, socket, level, option, value, length);
    LOG_INFO("SceNetPsForDriver sceNetGetsockopt socket={} level={} option={} value={} length={}",
        socket, level, option, log_hex(reinterpret_cast<uintptr_t>(value)), log_hex(reinterpret_cast<uintptr_t>(length)));
    return CALL_EXPORT(sceNetGetsockopt, socket, level, option, value, length);
}

EXPORT(int, SceNetPsForDriver_sceNetSendto, int socket, const void *buffer, unsigned int length, int flags,
    const SceNetSockaddr *address, unsigned int address_length) {
    TRACY_FUNC(SceNetPsForDriver_sceNetSendto, socket, buffer, length, flags, address, address_length);
    LOG_INFO("SceNetPsForDriver sceNetSendto socket={} buffer={} length={} flags={} address={} address_length={}",
        socket, log_hex(reinterpret_cast<uintptr_t>(buffer)), length, flags,
        log_hex(reinterpret_cast<uintptr_t>(address)), address_length);
    return CALL_EXPORT(sceNetSendto, socket, buffer, length, flags, address, address_length);
}

EXPORT(int, SceNetPsForDriver_sceNetGetpeername, int socket, SceNetSockaddr *address, unsigned int *address_length) {
    TRACY_FUNC(SceNetPsForDriver_sceNetGetpeername, socket, address, address_length);
    LOG_INFO("SceNetPsForDriver sceNetGetpeername socket={} address={} address_length={}", socket,
        log_hex(reinterpret_cast<uintptr_t>(address)), log_hex(reinterpret_cast<uintptr_t>(address_length)));
    return CALL_EXPORT(sceNetGetpeername, socket, address, address_length);
}

EXPORT(int, SceNetPsForDriver_C000CA8F, Ptr<void> client) {
    TRACY_FUNC(SceNetPsForDriver_C000CA8F, client);
    LOG_INFO("SceNetPsForDriver_C000CA8F client={}", log_hex(client.address()));
    return UNIMPLEMENTED();
}

EXPORT(int, SceNetPsForDriver_E41C0794) {
    TRACY_FUNC(SceNetPsForDriver_E41C0794);
    LOG_INFO("SceNetPsForDriver_E41C0794");
    return 0;
}

EXPORT(int, SceNetPsForDriver_sceNetSocket, const char *name, int domain, int type, int protocol) {
    TRACY_FUNC(SceNetPsForDriver_sceNetSocket, name, domain, type, protocol);
    LOG_INFO("SceNetPsForDriver sceNetSocket name={} domain={} type={} protocol={}",
        log_hex(reinterpret_cast<uintptr_t>(name)), domain, type, protocol);
    return CALL_EXPORT(sceNetSocket, name, domain, static_cast<SceNetSocketType>(type),
        static_cast<SceNetProtocol>(protocol));
}

EXPORT(int, SceNetPsForDriver_sceNetShutdown, int socket, int how) {
    TRACY_FUNC(SceNetPsForDriver_sceNetShutdown, socket, how);
    LOG_INFO("SceNetPsForDriver sceNetShutdown socket={} how={}", socket, how);
    return CALL_EXPORT(sceNetShutdown, socket, how);
}

EXPORT(int, SceNetPsForDriver_F5788FF8, Ptr<void> driver) {
    TRACY_FUNC(SceNetPsForDriver_F5788FF8, driver);
    LOG_INFO("SceNetPsForDriver_F5788FF8 driver={}", log_hex(driver.address()));
    return UNIMPLEMENTED();
}
