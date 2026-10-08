/*
    Copyright 2019 natinusala
    Copyright 2019 WerWolv
    Copyright 2019 p-sam

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <stdio.h>
#include <switch.h>
#include <unistd.h>
#include "switch_runtime.h"
#include "../core/util.h"

static int nxlink_sock = -1;
static SocketInitConfig socket_config;
static Result socket_result;
static Result fast_socket_result;
static bool socket_fallback;

void pipensx_log_socket_config(void)
{
    log_msg("[net] socket init result=0x%08x fast_result=0x%08x "
            "fallback=%d rx_initial=%u rx_max=%u sessions=%u efficiency=%u\n",
            socket_result, fast_socket_result, socket_fallback,
            socket_config.tcp_rx_buf_size, socket_config.tcp_rx_buf_max_size,
            socket_config.num_bsd_sessions, socket_config.sb_efficiency);
}

void userAppInit()
{
    printf("userAppInit\n");
    appletLockExit();

    // Init network
    SocketInitConfig cfg = *(socketGetDefaultInitConfig());
    AppletType at        = appletGetAppletType();
    if (at == AppletType_Application || at == AppletType_SystemApplication)
    {
        cfg.num_bsd_sessions = 12; // default is 3
        cfg.sb_efficiency    = 8; // default is 4
        // Grow only the full-application profile. libnx sizes the socket
        // transfer-memory pool from these maxima, so the larger receive
        // window has backing memory rather than just a setsockopt request.
        cfg.tcp_rx_buf_size = 256 * 1024;
        cfg.tcp_rx_buf_max_size = 1024 * 1024;
        socket_result = socketInitialize(&cfg);
        fast_socket_result = socket_result;
        if (R_FAILED(socket_result)) {
            socket_fallback = true;
            cfg = *socketGetDefaultInitConfig();
            cfg.num_bsd_sessions = 12;
            cfg.sb_efficiency = 8;
            socket_result = socketInitialize(&cfg);
        }
    }
    else
    {
        cfg.num_bsd_sessions = 2;
        cfg.sb_efficiency    = 1;
        socket_result = socketInitialize(&cfg);
    }
    socket_config = cfg;

#ifdef DEBUG
    nxlink_sock = nxlinkStdio();
#endif

    romfsInit();
    hidsysInitialize();
    inssInitialize();
    plInitialize(PlServiceType_User);
    setsysInitialize();
    setInitialize();
    psmInitialize();
    nifmInitialize(NifmServiceType_User);
    lblInitialize();
}

void userAppExit()
{
    printf("userAppExit\n");

    // backlight
    lblExit();
    // network state
    nifmExit();
    // power state
    psmExit();
    // system language...
    setExit();
    // system theme, system version...
    setsysExit();
    // system font
    plExit();

    inssExit();
    hidsysExit();
    romfsExit();

    if (nxlink_sock != -1)
        close(nxlink_sock);

    socketExit();

    appletUnlockExit();
}
