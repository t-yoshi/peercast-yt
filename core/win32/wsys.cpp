// ------------------------------------------------
// File : wsys.cpp
// Date: 4-apr-2002
// Author: giles
// Desc:
//      WSys derives from Sys to provide basic win32 functions such as starting threads.
//
// (c) 2002 peercast.org
// ------------------------------------------------
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// ------------------------------------------------

#include <process.h>
#include <windows.h>
#include <time.h>
#include "win32/wsys.h"
#include "win32/wsocket.h"
#include "stats.h"
#include "peercast.h"
#include <sys/timeb.h>
#include <time.h>

// ---------------------------------
WSys::WSys(HWND w)
{
    stats.clear();

    rndGen.setSeed(getTime());

    mainWindow = w;
    WSAClientSocket::init();
}

// ---------------------------------
double WSys::getDTime()
{
   struct _timeb timebuffer;

   _ftime( &timebuffer );

   return (double)timebuffer.time+(((double)timebuffer.millitm)/1000);
}

// ---------------------------------
ClientSocket *WSys::createSocket()
{
    return new WSAClientSocket();
}

// --------------------------------------------------
void WSys::callLocalURL(const char *str,int port)
{
    char cmd[512];
    sprintf(cmd,"http://localhost:%d/%s",port,str);
    ShellExecuteA(mainWindow, NULL, cmd, NULL, NULL, SW_SHOWNORMAL);
}

// ---------------------------------
void WSys::getURL(const char *url)
{
    if (mainWindow)
        if (Sys::strnicmp(url,"http://",7) || Sys::strnicmp(url,"mailto:",7)) // XXX: ==0 が抜けてる？
            ShellExecuteA(mainWindow, NULL, url, NULL, NULL, SW_SHOWNORMAL);
}

// ---------------------------------
void WSys::exit()
{
    if (mainWindow)
        PostMessage(mainWindow,WM_CLOSE,0,0);
    else
        ::exit(0);
}

// --------------------------------------------------
void WSys::executeFile(const char *file)
{
    ShellExecuteA(NULL,"open",file,NULL,NULL,SW_SHOWNORMAL);
}
