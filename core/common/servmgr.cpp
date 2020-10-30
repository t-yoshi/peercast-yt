// ------------------------------------------------
// File : servmgr.cpp
// Date: 4-apr-2002
// Author: giles
// Desc:
//      Management class for handling multiple servent connections.
//
// (c) 2002 peercast.org
// ------------------------------------------------
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// ------------------------------------------------

#include <memory>

#include "servent.h"
#include "servmgr.h"
#include "inifile.h"
#include "stats.h"
#include "peercast.h"
#include "pcp.h"
#include "atom.h"
#include "version2.h"
#include "rtmpmonit.h"
#include "chandir.h"
#include "uptest.h"

// -----------------------------------
ServMgr::ServMgr()
    : relayBroadcast(30) // オリジナルでは未初期化。
    , channelDirectory(new ChannelDirectory())
    , uptestServiceRegistry(new UptestServiceRegistry())
    , rtmpServerMonitor(std::string(peercastApp->getPath()) + "rtmp-server")
{
    authType = AUTH_COOKIE;
    cookieList.init();

    serventNum = 0;

    startTime = sys->getTime();

    allowServer1 = Servent::ALLOW_ALL;

    clearHostCache(ServHost::T_NONE);
    password[0]=0;

    useFlowControl = true;

    maxServIn = 50;

    lastIncoming = 0;

    maxBitrateOut = 0;
    maxRelays = MIN_RELAYS;
    maxDirect = 0;
    refreshHTML = 5;

    networkID.clear();

    notifyMask = 0xffff;

    tryoutDelay = 10;

    sessionID.generate();

    isDisabled = false;
    isRoot = false;

    forceIP.clear();

    strcpy(connectHost, "connect1.peercast.org");
    strcpy(htmlPath, "html/en");

    rootHost = "yp.pcgw.pgw.jp:7146";

    serverHost.fromStrIP("127.0.0.1", DEFAULT_PORT);

    firewalled = FW_UNKNOWN;
    allowDirect = true;
    autoConnect = true;
    forceLookup = true;
    autoServe = true;
    forceNormal = false;

    maxControl = 3;

    totalStreams = 0;
    firewallTimeout = 30;
    pauseLog = false;
    m_logLevel = LogBuffer::T_INFO;

    shutdownTimer = 0;

    downloadURL[0] = 0;
    rootMsg.clear();

    restartServer=false;

    setFilterDefaults();

    servents = NULL;

    chanLog="";

    serverName = "";

    transcodingEnabled = false;
    preset = "veryfast";
    audioCodec = "mp3";

    wmvProtocol = "http";

    rtmpPort = 1935;

    channelDirectory->addFeed("http://yp.pcgw.pgw.jp/index.txt");

    uptestServiceRegistry->addURL("http://bayonet.ddo.jp/sp/yp4g.xml");
    uptestServiceRegistry->addURL("http://temp.orz.hm/yp/yp4g.xml");

    chat = true;
}

// -----------------------------------
ServMgr::~ServMgr()
{
    while (servents)
    {
        auto next = servents->next;
        delete servents;
        servents = next;
    }
}

// -----------------------------------
void    ServMgr::connectBroadcaster()
{
    if (!rootHost.isEmpty())
    {
        if (!numUsed(Servent::T_COUT))
        {
            Servent *sv = allocServent();
            if (sv)
            {
                sv->initOutgoing(Servent::T_COUT);
                sys->sleep(3000);
            }
        }
    }
}

// -----------------------------------
void ServMgr::setFilterDefaults()
{
    numFilters = 0;

    filters[numFilters].setPattern("255.255.255.255");
    filters[numFilters].flags = ServFilter::F_NETWORK|ServFilter::F_DIRECT;
    numFilters++;
}

// -----------------------------------
void    ServMgr::setPassiveSearch(unsigned int t)
{
//  if ((t > 0) && (t < 60))
//      t = 60;
//  passiveSearch = t;
}

// -----------------------------------
bool ServMgr::seenHost(Host &h, ServHost::TYPE type, unsigned int time)
{
    time = sys->getTime()-time;

    for (int i=0; i<MAX_HOSTCACHE; i++)
        if (hostCache[i].type == type)
            if (hostCache[i].host.ip == h.ip)
                if (hostCache[i].time >= time)
                    return true;
    return false;
}

// -----------------------------------
void ServMgr::addHost(Host &h, ServHost::TYPE type, unsigned int time)
{
    if (!h.isValid())
        return;

    ServHost *sh=NULL;

    for (int i=0; i<MAX_HOSTCACHE; i++)
        if (hostCache[i].type == type)
            if (hostCache[i].host.isSame(h))
            {
                sh = &hostCache[i];
                break;
            }

    char str[64];
    h.toStr(str);

    if (!sh)
        LOG_DEBUG("New host: %s - %s", str, ServHost::getTypeStr(type));
    else
        LOG_DEBUG("Old host: %s - %s", str, ServHost::getTypeStr(type));

    if (!sh)
    {
        // find empty slot
        for (int i=0; i<MAX_HOSTCACHE; i++)
            if (hostCache[i].type == ServHost::T_NONE)
            {
                sh = &hostCache[i];
                break;
            }

        // otherwise, find oldest host and replace
        if (!sh)
            for (int i=0; i<MAX_HOSTCACHE; i++)
                if (hostCache[i].type != ServHost::T_NONE)
                {
                    if (sh)
                    {
                        if (hostCache[i].time < sh->time)
                            sh = &hostCache[i];
                    }else{
                        sh = &hostCache[i];
                    }
                }
    }

    if (sh)
        sh->init(h, type, time);
}

// -----------------------------------
void ServMgr::deadHost(Host &h, ServHost::TYPE t)
{
    for (int i=0; i<MAX_HOSTCACHE; i++)
        if (hostCache[i].type == t)
            if (hostCache[i].host.ip == h.ip)
                if (hostCache[i].host.port == h.port)
                    hostCache[i].init();
}

// -----------------------------------
void ServMgr::clearHostCache(ServHost::TYPE type)
{
    for (int i=0; i<MAX_HOSTCACHE; i++)
        if ((hostCache[i].type == type) || (type == ServHost::T_NONE))
            hostCache[i].init();
}

// -----------------------------------
unsigned int ServMgr::numHosts(ServHost::TYPE type)
{
    unsigned int cnt = 0;
    for (int i=0; i<MAX_HOSTCACHE; i++)
        if ((hostCache[i].type == type) || (type == ServHost::T_NONE))
            cnt++;
    return cnt;
}

// -----------------------------------
int ServMgr::getNewestServents(Host *hl, int max, Host &rh)
{
    int cnt=0;
    for (int i=0; i<max; i++)
    {
        // find newest host not in list
        ServHost *sh=NULL;
        for (int j=0; j<MAX_HOSTCACHE; j++)
        {
            // find newest servent
            if (hostCache[j].type == ServHost::T_SERVENT)
                if (!(rh.globalIP() && !hostCache[j].host.globalIP()))
                {
                    // and not in list already
                    bool found=false;
                    for (int k=0; k<cnt; k++)
                        if (hl[k].isSame(hostCache[j].host))
                        {
                            found=true;
                            break;
                        }

                    if (!found)
                    {
                        if (!sh)
                        {
                            sh = &hostCache[j];
                        }else{
                            if (hostCache[j].time > sh->time)
                                sh = &hostCache[j];
                        }
                    }
                }
        }

        // add to list
        if (sh)
            hl[cnt++]=sh->host;
    }

    return cnt;
}

// -----------------------------------
Servent *ServMgr::findOldestServent(Servent::TYPE type, bool priv)
{
    Servent *oldest=NULL;

    Servent *s = servents;
    while (s)
    {
        if (s->type == type)
            if (s->thread.active())
                if (s->isOlderThan(oldest))
                    if (s->isPrivate() == priv)
                        oldest = s;
        s=s->next;
    }
    return oldest;
}

// -----------------------------------
Servent *ServMgr::findServent(Servent::TYPE type, Host &host, const GnuID &netid)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    Servent *s = servents;
    while (s)
    {
        if (s->type == type)
        {
            Host h = s->getHost();
            if (h.isSame(host) && s->networkID.isSame(netid))
            {
                return s;
            }
        }
        s = s->next;
    }

    return NULL;
}

// -----------------------------------
Servent *ServMgr::findServent(unsigned int ip, unsigned short port, const GnuID &netid)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    Servent *s = servents;
    while (s)
    {
        if (s->type != Servent::T_NONE)
        {
            Host h = s->getHost();
            if ((h.ip == ip) && (h.port == port) && (s->networkID.isSame(netid)))
            {
                return s;
            }
        }
        s = s->next;
    }

    return NULL;
}

// -----------------------------------
Servent *ServMgr::findServent(Servent::TYPE t)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    Servent *s = servents;
    while (s)
    {
        if (s->type == t)
            return s;
        s = s->next;
    }
    return NULL;
}

// -----------------------------------
Servent *ServMgr::findServentByIndex(int id)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    Servent *s = servents;
    int cnt = 0;

    while (s)
    {
        if (cnt == id)
            return s;
        cnt++;
        s = s->next;
    }

    return NULL;
}

// -----------------------------------
Servent *ServMgr::findServentByID(int id)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    Servent *s = servents;

    while (s)
    {
        if (s->serventIndex == id)
            return s;
        s = s->next;
    }

    return NULL;
}

// -----------------------------------
Servent *ServMgr::allocServent()
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    Servent *s = servents;
    while (s)
    {
        if (s->status == Servent::S_FREE)
            break;
        s = s->next;
    }

    if (!s)
    {
        int num = ++serventNum;
        s = new Servent(num);
        s->next = servents;
        servents = s;

        LOG_TRACE("allocated servent %d", num);
    }else
        LOG_TRACE("reused servent %d", s->serventIndex);

    s->reset();

    return s;
}

// --------------------------------------------------
void    ServMgr::closeConnections(Servent::TYPE type)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    Servent *sv = servents;
    while (sv)
    {
        if (sv->isConnected())
            if (sv->type == type)
                sv->thread.shutdown();
        sv = sv->next;
    }
}

// -----------------------------------
unsigned int ServMgr::numConnected(int type, bool priv, unsigned int uptime)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    unsigned int cnt=0;

    unsigned int ctime=sys->getTime();
    Servent *s = servents;
    while (s)
    {
        if (s->thread.active())
            if (s->isConnected())
                if (s->type == type)
                    if (s->isPrivate()==priv)
                        if ((ctime-s->lastConnect) >= uptime)
                            cnt++;

        s = s->next;
    }
    return cnt;
}

// -----------------------------------
unsigned int ServMgr::numConnected()
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    unsigned int cnt=0;

    Servent *s = servents;
    while (s)
    {
        if (s->thread.active())
            if (s->isConnected())
                cnt++;

        s = s->next;
    }
    return cnt;
}

// -----------------------------------
unsigned int ServMgr::numServents()
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    unsigned int cnt = 0;

    Servent *s = servents;
    while (s)
    {
        cnt++;
        s = s->next;
    }
    return cnt;
}

// -----------------------------------
unsigned int ServMgr::numUsed(int type)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    unsigned int cnt=0;

    Servent *s = servents;
    while (s)
    {
        std::lock_guard<std::recursive_mutex> cs(s->lock);
        if (s->type == type)
            cnt++;
        s = s->next;
    }
    return cnt;
}

// -----------------------------------
unsigned int ServMgr::numActiveOnPort(int port)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    unsigned int cnt=0;

    Servent *s = servents;
    while (s)
    {
        std::lock_guard<std::recursive_mutex> cs(s->lock);
        if (s->thread.active() && s->sock && (s->servPort == port))
            cnt++;
        s = s->next;
    }
    return cnt;
}

// -----------------------------------
unsigned int ServMgr::numActive(Servent::TYPE tp)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    unsigned int cnt=0;

    Servent *s = servents;
    while (s)
    {
        std::lock_guard<std::recursive_mutex> cs(s->lock);

        if (s->thread.active() && s->sock && (s->type == tp))
            cnt++;
        s = s->next;
    }
    return cnt;
}

// -----------------------------------
unsigned int ServMgr::totalOutput(bool all)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    unsigned int tot = 0;
    Servent *s = servents;
    while (s)
    {
        if (s->isConnected())
            if (all || !s->isPrivate())
                if (s->sock)
                    tot += s->sock->bytesOutPerSec();
        s = s->next;
    }

    return tot;
}

// -----------------------------------
void ServMgr::quit()
{
    LOG_DEBUG("ServMgr is quitting..");

    serverThread.shutdown();
    idleThread.shutdown();

    LOG_DEBUG("Disabling RMTP server..");
    rtmpServerMonitor.disable();

    Servent *s = servents;
    while (s)
    {
        if (s->thread.active())
            s->thread.shutdown();

        s = s->next;
    }
}

// -----------------------------------
bool ServMgr::checkForceIP()
{
    if (!forceIP.isEmpty())
    {
        unsigned int newIP = ClientSocket::getIP(forceIP.cstr());
        if (serverHost.ip != newIP)
        {
            serverHost.ip = newIP;
            char ipstr[64];
            serverHost.IPtoStr(ipstr);
            LOG_DEBUG("Server IP changed to %s", ipstr);
            return true;
        }
    }
    return false;
}

// -----------------------------------
void ServMgr::checkFirewall()
{
    if ((getFirewall() == FW_UNKNOWN) && !servMgr->rootHost.isEmpty())
    {
        LOG_DEBUG("Checking firewall..");
        Host host;
        host.fromStrName(servMgr->rootHost.cstr(), DEFAULT_PORT);

        ClientSocket *sock = sys->createSocket();
        if (!sock)
            throw StreamException("Unable to create socket");
        sock->setReadTimeout(30000);
        sock->open(host);
        sock->connect();

        AtomStream atom(*sock);

        atom.writeInt(PCP_CONNECT, 1);

        GnuID remoteID;
        String agent;
        Servent::handshakeOutgoingPCP(atom, sock->host, remoteID, agent, true);

        atom.writeInt(PCP_QUIT, PCP_ERROR_QUIT);

        sock->close();
        delete sock;
    }
}

// -----------------------------------
ServMgr::FW_STATE ServMgr::getFirewall()
{
    std::lock_guard<std::recursive_mutex> cs(lock);
    return firewalled;
}

// -----------------------------------
void ServMgr::setFirewall(FW_STATE state)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    if (firewalled != state)
    {
        const char *str;
        switch (state)
        {
            case FW_ON:
                str = "ON";
                break;
            case FW_OFF:
                str = "OFF";
                break;
            case FW_UNKNOWN:
            default:
                str = "UNKNOWN";
                break;
        }

        LOG_DEBUG("Firewall is set to %s", str);
        firewalled = state;
    }
}

// -----------------------------------
bool ServMgr::isFiltered(int fl, Host &h)
{
    for (int i=0; i<numFilters; i++)
        if (filters[i].matches(fl, h))
            return true;

    return false;
}

// --------------------------------------------------
void writeServerSettings(IniFileBase &iniFile, unsigned int a)
{
    iniFile.writeBoolValue("allowHTML", a & Servent::ALLOW_HTML);
    iniFile.writeBoolValue("allowBroadcast", a & Servent::ALLOW_BROADCAST);
    iniFile.writeBoolValue("allowNetwork", a & Servent::ALLOW_NETWORK);
    iniFile.writeBoolValue("allowDirect", a & Servent::ALLOW_DIRECT);
}

// --------------------------------------------------
void writeFilterSettings(IniFileBase &iniFile, ServFilter &f)
{
    iniFile.writeStrValue("ip", f.getPattern());
    iniFile.writeBoolValue("private", f.flags & ServFilter::F_PRIVATE);
    iniFile.writeBoolValue("ban", f.flags & ServFilter::F_BAN);
    iniFile.writeBoolValue("network", f.flags & ServFilter::F_NETWORK);
    iniFile.writeBoolValue("direct", f.flags & ServFilter::F_DIRECT);
}

// --------------------------------------------------
static void  writeServHost(IniFileBase &iniFile, ServHost &sh)
{
    iniFile.writeSection("Host");

    iniFile.writeStrValue("type", ServHost::getTypeStr(sh.type));
    iniFile.writeStrValue("address", sh.host.str().c_str());
    iniFile.writeIntValue("time", sh.time);

    iniFile.writeLine("[End]");
}

// --------------------------------------------------
static void  writeRelayChannel(IniFileBase &iniFile, std::shared_ptr<Channel> c)
{
    iniFile.writeSection("RelayChannel");

    iniFile.writeStrValue("name", c->getName());
    iniFile.writeStrValue("desc", c->info.desc);
    iniFile.writeStrValue("genre", c->info.genre);
    iniFile.writeStrValue("contactURL", c->info.url);
    iniFile.writeStrValue("comment", c->info.comment);
    if (!c->sourceURL.isEmpty())
        iniFile.writeStrValue("sourceURL", c->sourceURL);
    iniFile.writeStrValue("sourceProtocol", ChanInfo::getProtocolStr(c->info.srcProtocol));
    iniFile.writeStrValue("contentType", c->info.getTypeStr());
    iniFile.writeStrValue("MIMEType", c->info.MIMEType);
    iniFile.writeStrValue("streamExt", c->info.streamExt);
    iniFile.writeIntValue("bitrate", c->info.bitrate);
    iniFile.writeStrValue("id", c->info.id.str());
    iniFile.writeBoolValue("stayConnected", c->stayConnected);

    // トラッカーIPの書き出し。
    ChanHitList *chl = chanMgr->findHitListByID(c->info.id);
    if (chl)
    {
        ChanHitSearch chs;
        chs.trackersOnly = true;
        if (chl->pickHits(chs))
        {
            iniFile.writeStrValue("tracker", chs.best[0].host.str().c_str());
        }
    }

    // トラック情報の書き出し。
    iniFile.writeStrValue("trackContact", c->info.track.contact);
    iniFile.writeStrValue("trackTitle", c->info.track.title);
    iniFile.writeStrValue("trackArtist", c->info.track.artist);
    iniFile.writeStrValue("trackAlbum", c->info.track.album);
    iniFile.writeStrValue("trackGenre", c->info.track.genre);

    iniFile.writeLine("[End]");
}

// --------------------------------------------------
void ServMgr::saveSettings(const char *fn)
{
    IniFile iniFile;
    if (!iniFile.openWriteReplace(fn))
    {
        LOG_ERROR("Unable to open ini file");
    }else{
        LOG_DEBUG("Saving settings to: %s", fn);

        doSaveSettings(iniFile);
        iniFile.close();
    }
}

// --------------------------------------------------
void ServMgr::doSaveSettings(IniFileBase& iniFile)
{
    std::lock_guard<std::recursive_mutex> cs1(lock);
    std::lock_guard<std::recursive_mutex> cs2(chanMgr->lock);

    iniFile.writeSection("Server");
    iniFile.writeStrValue("serverName", this->serverName);
    iniFile.writeIntValue("serverPort", this->serverHost.port);
    iniFile.writeBoolValue("autoServe", this->autoServe);
    iniFile.writeStrValue("forceIP", this->forceIP);
    iniFile.writeBoolValue("isRoot", this->isRoot);
    iniFile.writeIntValue("maxBitrateOut", this->maxBitrateOut);
    iniFile.writeIntValue("maxRelays", this->maxRelays);
    iniFile.writeIntValue("maxDirect", this->maxDirect);
    iniFile.writeIntValue("maxRelaysPerChannel", chanMgr->maxRelaysPerChannel);
    iniFile.writeIntValue("firewallTimeout", firewallTimeout);
    iniFile.writeBoolValue("forceNormal", forceNormal);
    iniFile.writeStrValue("rootMsg", rootMsg);
    iniFile.writeStrValue("authType", (this->authType == ServMgr::AUTH_COOKIE) ? "cookie" : "http-basic");
    iniFile.writeStrValue("cookiesExpire", (this->cookieList.neverExpire == true) ? "never": "session");
    iniFile.writeStrValue("htmlPath", this->htmlPath);
    iniFile.writeIntValue("maxServIn", this->maxServIn);
    iniFile.writeStrValue("chanLog", this->chanLog);

    iniFile.writeStrValue("networkID", networkID.str());

    iniFile.writeSection("Broadcast");
    iniFile.writeIntValue("broadcastMsgInterval", chanMgr->broadcastMsgInterval);
    iniFile.writeStrValue("broadcastMsg", chanMgr->broadcastMsg);
    iniFile.writeIntValue("icyMetaInterval", chanMgr->icyMetaInterval);
    iniFile.writeStrValue("broadcastID", chanMgr->broadcastID.str());
    iniFile.writeIntValue("hostUpdateInterval", chanMgr->hostUpdateInterval);
    iniFile.writeIntValue("maxControlConnections", this->maxControl);
    iniFile.writeStrValue("rootHost", this->rootHost);

    iniFile.writeSection("Client");
    iniFile.writeIntValue("refreshHTML", refreshHTML);
    iniFile.writeBoolValue("chat", chat);
    iniFile.writeIntValue("relayBroadcast", this->relayBroadcast);
    iniFile.writeIntValue("minBroadcastTTL", chanMgr->minBroadcastTTL);
    iniFile.writeIntValue("maxBroadcastTTL", chanMgr->maxBroadcastTTL);
    iniFile.writeIntValue("pushTries", chanMgr->pushTries);
    iniFile.writeIntValue("pushTimeout", chanMgr->pushTimeout);
    iniFile.writeIntValue("maxPushHops", chanMgr->maxPushHops);
    iniFile.writeBoolValue("transcodingEnabled", this->transcodingEnabled);
    iniFile.writeStrValue("preset", this->preset);
    iniFile.writeStrValue("audioCodec", this->audioCodec);
    iniFile.writeStrValue("wmvProtocol", this->wmvProtocol);

    iniFile.writeSection("Privacy");
    iniFile.writeStrValue("password", this->password);
    iniFile.writeIntValue("maxUptime", chanMgr->maxUptime);

    for (int i = 0; i < this->numFilters; i++)
    {
        iniFile.writeSection("Filter");
            writeFilterSettings(iniFile, this->filters[i]);
        iniFile.writeLine("[End]");
    }

    // チャンネルフィード
    for (auto feed : this->channelDirectory->feeds())
    {
        iniFile.writeSection("Feed");
        iniFile.writeStrValue("url", feed.url);
        iniFile.writeLine("[End]");
    }

    // 帯域チェック
    for (auto url : this->uptestServiceRegistry->getURLs())
    {
        iniFile.writeSection("Uptest");
        iniFile.writeStrValue("url", url);
        iniFile.writeLine("[End]");
    }

    iniFile.writeSection("Notify");
        iniFile.writeBoolValue("PeerCast", notifyMask & NT_PEERCAST);
        iniFile.writeBoolValue("Broadcasters", notifyMask & NT_BROADCASTERS);
        iniFile.writeBoolValue("TrackInfo", notifyMask & NT_TRACKINFO);
    iniFile.writeLine("[End]");

    iniFile.writeSection("Server1");
        writeServerSettings(iniFile, allowServer1);
    iniFile.writeLine("[End]");

    iniFile.writeSection("Debug");
    iniFile.writeIntValue("logLevel", logLevel());
    iniFile.writeBoolValue("pauseLog", pauseLog);
    iniFile.writeIntValue("idleSleepTime", sys->idleSleepTime);

    std::shared_ptr<Channel> c = chanMgr->channel;
    while (c)
    {
        if (c->isActive() && c->stayConnected)
            writeRelayChannel(iniFile, c);

        c = c->next;
    }

    for (int i = 0; i < ServMgr::MAX_HOSTCACHE; i++)
    {
        ServHost *sh = &this->hostCache[i];
        if (sh->type != ServHost::T_NONE)
            writeServHost(iniFile, *sh);
    }
}

// --------------------------------------------------
unsigned int readServerSettings(IniFileBase &iniFile, unsigned int a)
{
    while (iniFile.readNext())
    {
        if (iniFile.isName("[End]"))
            break;
        else if (iniFile.isName("allowHTML"))
            a = iniFile.getBoolValue()?a|Servent::ALLOW_HTML:a&~Servent::ALLOW_HTML;
        else if (iniFile.isName("allowDirect"))
            a = iniFile.getBoolValue()?a|Servent::ALLOW_DIRECT:a&~Servent::ALLOW_DIRECT;
        else if (iniFile.isName("allowNetwork"))
            a = iniFile.getBoolValue()?a|Servent::ALLOW_NETWORK:a&~Servent::ALLOW_NETWORK;
        else if (iniFile.isName("allowBroadcast"))
            a = iniFile.getBoolValue()?a|Servent::ALLOW_BROADCAST:a&~Servent::ALLOW_BROADCAST;
    }
    return a;
}

// --------------------------------------------------
void readFilterSettings(IniFileBase &iniFile, ServFilter &sv)
{
    sv.init();

    while (iniFile.readNext())
    {
        if (iniFile.isName("[End]"))
            break;
        else if (iniFile.isName("ip"))
            sv.setPattern(iniFile.getStrValue());
        else if (iniFile.isName("private"))
            sv.flags = (sv.flags & ~ServFilter::F_PRIVATE) | (iniFile.getBoolValue()?ServFilter::F_PRIVATE:0);
        else if (iniFile.isName("ban"))
            sv.flags = (sv.flags & ~ServFilter::F_BAN) | (iniFile.getBoolValue()?ServFilter::F_BAN:0);
        else if (iniFile.isName("allow") || iniFile.isName("network"))
            sv.flags = (sv.flags & ~ServFilter::F_NETWORK) | (iniFile.getBoolValue()?ServFilter::F_NETWORK:0);
        else if (iniFile.isName("direct"))
            sv.flags = (sv.flags & ~ServFilter::F_DIRECT) | (iniFile.getBoolValue()?ServFilter::F_DIRECT:0);
    }
}

// --------------------------------------------------
void ServMgr::loadSettings(const char *fn)
{
    int feedIndex = 0;
    IniFile iniFile;

    if (!iniFile.openReadOnly(fn))
        saveSettings(fn);

    servMgr->numFilters = 0;

    std::lock_guard<std::recursive_mutex> cs(servMgr->uptestServiceRegistry->m_lock);
    servMgr->uptestServiceRegistry->clear();

    std::lock_guard<std::recursive_mutex> cs1(servMgr->channelDirectory->m_lock);
    servMgr->channelDirectory->clearFeeds();

    if (iniFile.openReadOnly(fn))
    {
        while (iniFile.readNext())
        {
            // server settings
            if (iniFile.isName("serverName"))
                servMgr->serverName = iniFile.getStrValue();
            else if (iniFile.isName("serverPort"))
                servMgr->serverHost.port = iniFile.getIntValue();
            else if (iniFile.isName("autoServe"))
                servMgr->autoServe = iniFile.getBoolValue();
            else if (iniFile.isName("autoConnect"))
                servMgr->autoConnect = iniFile.getBoolValue();
            else if (iniFile.isName("icyPassword"))     // depreciated
                strcpy(servMgr->password, iniFile.getStrValue());
            else if (iniFile.isName("forceIP"))
                servMgr->forceIP = iniFile.getStrValue();
            else if (iniFile.isName("isRoot"))
                servMgr->isRoot = iniFile.getBoolValue();
            else if (iniFile.isName("broadcastID"))
            {
                chanMgr->broadcastID.fromStr(iniFile.getStrValue());
            }else if (iniFile.isName("htmlPath"))
                strcpy(servMgr->htmlPath, iniFile.getStrValue());
            else if (iniFile.isName("maxControlConnections"))
            {
                servMgr->maxControl = iniFile.getIntValue();
            }
            else if (iniFile.isName("maxBitrateOut"))
                servMgr->maxBitrateOut = iniFile.getIntValue();

            else if (iniFile.isName("maxStreamsOut"))       // depreciated
                servMgr->setMaxRelays(iniFile.getIntValue());
            else if (iniFile.isName("maxRelays"))
                servMgr->setMaxRelays(iniFile.getIntValue());
            else if (iniFile.isName("maxDirect"))
                servMgr->maxDirect = iniFile.getIntValue();

            else if (iniFile.isName("maxStreamsPerChannel"))        // depreciated
                chanMgr->maxRelaysPerChannel = iniFile.getIntValue();
            else if (iniFile.isName("maxRelaysPerChannel"))
                chanMgr->maxRelaysPerChannel = iniFile.getIntValue();

            else if (iniFile.isName("firewallTimeout"))
                firewallTimeout = iniFile.getIntValue();
            else if (iniFile.isName("forceNormal"))
                forceNormal = iniFile.getBoolValue();
            else if (iniFile.isName("broadcastMsgInterval"))
                chanMgr->broadcastMsgInterval = iniFile.getIntValue();
            else if (iniFile.isName("broadcastMsg"))
                chanMgr->broadcastMsg.set(iniFile.getStrValue(), String::T_ASCII);
            else if (iniFile.isName("hostUpdateInterval"))
                chanMgr->hostUpdateInterval = iniFile.getIntValue();
            else if (iniFile.isName("icyMetaInterval"))
                chanMgr->icyMetaInterval = iniFile.getIntValue();
            else if (iniFile.isName("maxServIn"))
                servMgr->maxServIn = iniFile.getIntValue();
            else if (iniFile.isName("chanLog"))
                servMgr->chanLog.set(iniFile.getStrValue(), String::T_ASCII);

            else if (iniFile.isName("rootMsg"))
                rootMsg.set(iniFile.getStrValue());
            else if (iniFile.isName("networkID"))
                networkID.fromStr(iniFile.getStrValue());
            else if (iniFile.isName("authType"))
            {
                const char *t = iniFile.getStrValue();
                if (Sys::stricmp(t, "cookie")==0)
                    servMgr->authType = ServMgr::AUTH_COOKIE;
                else if (Sys::stricmp(t, "http-basic")==0)
                    servMgr->authType = ServMgr::AUTH_HTTPBASIC;
            }else if (iniFile.isName("cookiesExpire"))
            {
                const char *t = iniFile.getStrValue();
                if (Sys::stricmp(t, "never")==0)
                    servMgr->cookieList.neverExpire = true;
                else if (Sys::stricmp(t, "session")==0)
                    servMgr->cookieList.neverExpire = false;
            }

            // privacy settings
            else if (iniFile.isName("password"))
                strcpy(servMgr->password, iniFile.getStrValue());
            else if (iniFile.isName("maxUptime"))
                chanMgr->maxUptime = iniFile.getIntValue();

            // client settings

            else if (iniFile.isName("rootHost"))
            {
                servMgr->rootHost = iniFile.getStrValue();
            }else if (iniFile.isName("deadHitAge"))
                chanMgr->deadHitAge = iniFile.getIntValue();
            else if (iniFile.isName("tryoutDelay"))
                servMgr->tryoutDelay = iniFile.getIntValue();
            else if (iniFile.isName("refreshHTML"))
                refreshHTML = iniFile.getIntValue();
            else if (iniFile.isName("chat"))
                servMgr->chat = iniFile.getBoolValue();
            else if (iniFile.isName("relayBroadcast"))
            {
                servMgr->relayBroadcast = iniFile.getIntValue();
                if (servMgr->relayBroadcast < 30)
                    servMgr->relayBroadcast = 30;
            }
            else if (iniFile.isName("minBroadcastTTL"))
                chanMgr->minBroadcastTTL = iniFile.getIntValue();
            else if (iniFile.isName("maxBroadcastTTL"))
                chanMgr->maxBroadcastTTL = iniFile.getIntValue();
            else if (iniFile.isName("pushTimeout"))
                chanMgr->pushTimeout = iniFile.getIntValue();
            else if (iniFile.isName("pushTries"))
                chanMgr->pushTries = iniFile.getIntValue();
            else if (iniFile.isName("maxPushHops"))
                chanMgr->maxPushHops = iniFile.getIntValue();
            else if (iniFile.isName("transcodingEnabled"))
                servMgr->transcodingEnabled = iniFile.getBoolValue();
            else if (iniFile.isName("preset"))
                servMgr->preset = iniFile.getStrValue();
            else if (iniFile.isName("audioCodec"))
                servMgr->audioCodec = iniFile.getStrValue();
            else if (iniFile.isName("wmvProtocol"))
                servMgr->wmvProtocol = iniFile.getStrValue();

            // debug
            else if (iniFile.isName("logLevel"))
                logLevel(iniFile.getIntValue());
            else if (iniFile.isName("pauseLog"))
                pauseLog = iniFile.getBoolValue();
            else if (iniFile.isName("idleSleepTime"))
                sys->idleSleepTime = iniFile.getIntValue();
            else if (iniFile.isName("[Server1]"))
                allowServer1 = readServerSettings(iniFile, allowServer1);
            else if (iniFile.isName("[Filter]"))
            {
                readFilterSettings(iniFile, filters[numFilters]);

                if (numFilters < (MAX_FILTERS-1))
                    numFilters++;
            }
            else if (iniFile.isName("[Feed]"))
            {
                while (iniFile.readNext())
                {
                    if (iniFile.isName("[End]"))
                        break;
                    else if (iniFile.isName("url"))
                        servMgr->channelDirectory->addFeed(iniFile.getStrValue());
                }
                feedIndex++;
            }
            else if (iniFile.isName("[Uptest]"))
            {
                while (iniFile.readNext())
                {
                    if (iniFile.isName("[End]"))
                        break;
                    else if (iniFile.isName("url"))
                        servMgr->uptestServiceRegistry->addURL(iniFile.getStrValue());
                }
            }else if (iniFile.isName("[Notify]"))
            {
                notifyMask = NT_UPGRADE;
                while (iniFile.readNext())
                {
                    if (iniFile.isName("[End]"))
                        break;
                    else if (iniFile.isName("PeerCast"))
                        notifyMask |= iniFile.getBoolValue()?NT_PEERCAST:0;
                    else if (iniFile.isName("Broadcasters"))
                        notifyMask |= iniFile.getBoolValue()?NT_BROADCASTERS:0;
                    else if (iniFile.isName("TrackInfo"))
                        notifyMask |= iniFile.getBoolValue()?NT_TRACKINFO:0;
                }
            }
            else if (iniFile.isName("[RelayChannel]"))
            {
                ChanInfo info;
                bool stayConnected=false;
                String sourceURL;
                while (iniFile.readNext())
                {
                    if (iniFile.isName("[End]"))
                        break;
                    else if (iniFile.isName("name"))
                        info.name.set(iniFile.getStrValue());
                    else if (iniFile.isName("desc"))
                        info.desc.set(iniFile.getStrValue());
                    else if (iniFile.isName("genre"))
                        info.genre.set(iniFile.getStrValue());
                    else if (iniFile.isName("contactURL"))
                        info.url.set(iniFile.getStrValue());
                    else if (iniFile.isName("comment"))
                        info.comment.set(iniFile.getStrValue());
                    else if (iniFile.isName("id"))
                        info.id.fromStr(iniFile.getStrValue());
                    else if (iniFile.isName("sourceType"))
                        info.srcProtocol = ChanInfo::getProtocolFromStr(iniFile.getStrValue());
                    else if (iniFile.isName("contentType"))
                        info.contentType = iniFile.getStrValue();
                    else if (iniFile.isName("MIMEType"))
                        info.MIMEType = iniFile.getStrValue();
                    else if (iniFile.isName("streamExt"))
                        info.streamExt = iniFile.getStrValue();
                    else if (iniFile.isName("stayConnected"))
                        stayConnected = iniFile.getBoolValue();
                    else if (iniFile.isName("sourceURL"))
                        sourceURL.set(iniFile.getStrValue());
                    else if (iniFile.isName("bitrate"))
                        info.bitrate = atoi(iniFile.getStrValue());
                    else if (iniFile.isName("tracker"))
                    {
                        ChanHit hit;
                        hit.init();
                        hit.tracker = true;
                        hit.host.fromStrName(iniFile.getStrValue(), DEFAULT_PORT);
                        hit.rhost[0] = hit.host;
                        hit.rhost[1] = hit.host;
                        hit.chanID = info.id;
                        hit.recv = true;
                        chanMgr->addHit(hit);
                    }
                    else if (iniFile.isName("trackContact"))
                        info.track.contact = iniFile.getStrValue();
                    else if (iniFile.isName("trackTitle"))
                        info.track.title = iniFile.getStrValue();
                    else if (iniFile.isName("trackArtist"))
                        info.track.artist = iniFile.getStrValue();
                    else if (iniFile.isName("trackAlbum"))
                        info.track.album = iniFile.getStrValue();
                    else if (iniFile.isName("trackGenre"))
                        info.track.genre = iniFile.getStrValue();
                }
                if (sourceURL.isEmpty())
                {
                    chanMgr->createRelay(info, stayConnected);
                }else
                {
                    info.bcID = chanMgr->broadcastID;
                    auto c = chanMgr->createChannel(info, NULL);
                    if (c)
                        c->startURL(sourceURL.cstr());
                }
            } else if (iniFile.isName("[Host]"))
            {
                Host h;
                ServHost::TYPE type = ServHost::T_NONE;
                unsigned int time = 0;

                while (iniFile.readNext())
                {
                    if (iniFile.isName("[End]"))
                        break;
                    else if (iniFile.isName("address"))
                        h.fromStrIP(iniFile.getStrValue(), DEFAULT_PORT);
                    else if (iniFile.isName("type"))
                        type = ServHost::getTypeFromStr(iniFile.getStrValue());
                    else if (iniFile.isName("time"))
                        time = iniFile.getIntValue();
                }
                servMgr->addHost(h, type, time);
            }
        }
    }

    if (!numFilters)
        setFilterDefaults();
}

// --------------------------------------------------
unsigned int ServMgr::numStreams(const GnuID &cid, Servent::TYPE tp, bool all)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    int cnt = 0;
    Servent *sv = servents;
    while (sv)
    {
        std::lock_guard<std::recursive_mutex> cs(sv->lock);
        if (sv->isConnected())
            if (sv->type == tp)
                if (sv->chanID.isSame(cid))
                    if (all || !sv->isPrivate())
                        cnt++;
        sv=sv->next;
    }
    return cnt;
}

// --------------------------------------------------
unsigned int ServMgr::numStreams(Servent::TYPE tp, bool all)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    int cnt = 0;
    Servent *sv = servents;
    while (sv)
    {
        if (sv->isConnected())
            if (sv->type == tp)
                if (all || !sv->isPrivate())
                    cnt++;
        sv=sv->next;
    }
    return cnt;
}

// --------------------------------------------------
bool ServMgr::getChannel(char *str, ChanInfo &info, bool relay)
{
    procConnectArgs(str, info);

    auto ch = chanMgr->findChannelByNameID(info);
    if (ch)
    {
        if (!ch->isPlaying())
        {
            if (relay)
            {
                ch->info.lastPlayStart = 0; // force reconnect
                ch->info.lastPlayEnd = 0;
                for (int i = 0; i < 100; i++)    // wait til it's playing for 10 seconds
                {
                    ch = chanMgr->findChannelByNameID(ch->info);

                    if (!ch)
                        return false;

                    if (ch->isPlaying())
                        break;

                    sys->sleep(100);
                }
            }else
                return false;
        }

        info = ch->info;    // get updated channel info

        return true;
    }else
    {
        if (relay)
        {
            ch = chanMgr->findAndRelay(info);
            if (ch)
            {
                info = ch->info; //get updated channel info
                return true;
            }
        }
    }

    return false;
}

// --------------------------------------------------
Servent *ServMgr::findConnection(Servent::TYPE t, const GnuID &sid)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    Servent *sv = servents;
    while (sv)
    {
        if (sv->isConnected())
            if (sv->type == t)
                if (sv->remoteID.isSame(sid))
                    return sv;
        sv=sv->next;
    }
    return NULL;
}

// --------------------------------------------------
void ServMgr::procConnectArgs(char *str, ChanInfo &info)
{
    char arg[MAX_CGI_LEN];
    char curr[MAX_CGI_LEN];

    const char *args = strstr(str, "?");
    if (args)
    {
        *strstr(str, "?") = '\0';
        args++;
    }

    info.initNameID(str);

    if (args)
    {
        while ((args = nextCGIarg(args, curr, arg)) != nullptr)
        {
            LOG_DEBUG("cmd: %s, arg: %s", curr, arg);

            if (strcmp(curr, "ip")==0)
            // ip - add hit
            {
                Host h;
                h.fromStrName(arg, DEFAULT_PORT);
                ChanHit hit;
                hit.init();
                hit.host = h;
                hit.rhost[0] = h;
                hit.rhost[1].init();
                hit.chanID = info.id;
                hit.recv = true;

                chanMgr->addHit(hit);
            }else if (strcmp(curr, "tip")==0)
            // tip - add tracker hit
            {
                Host h;
                h.fromStrName(arg, DEFAULT_PORT);
                chanMgr->addHit(h, info.id, true);
            }
        }
    }
}

// --------------------------------------------------
bool ServMgr::start()
{
    LOG_INFO("Peercast %s, %s", PCX_VERSTRING, peercastApp->getClientTypeOS());

    LOG_INFO("SessionID: %s", sessionID.str().c_str());


    LOG_INFO("BroadcastID: %s", chanMgr->broadcastID.str().c_str());

    checkForceIP();

    serverThread.func = ServMgr::serverProc;
    if (!sys->startThread(&serverThread))
        return false;

    idleThread.func = ServMgr::idleProc;
    if (!sys->startThread(&idleThread))
        return false;

    return true;
}

// --------------------------------------------------
int ServMgr::clientProc(ThreadInfo *thread)
{
#if 0
    thread->lock();

    GnuID netID;
    netID = servMgr->networkID;

    while (thread->active)
    {
        if (servMgr->autoConnect)
        {
            if (servMgr->needConnections() || servMgr->forceLookup)
            {
                if (servMgr->needHosts() || servMgr->forceLookup)
                {
                    // do lookup to find some hosts

                    Host lh;
                    lh.fromStrName(servMgr->connectHost, DEFAULT_PORT);

                    if (!servMgr->findServent(lh.ip, lh.port, netID))
                    {
                        Servent *sv = servMgr->allocServent();
                        if (sv)
                        {
                            LOG_DEBUG("Lookup: %s", servMgr->connectHost);
                            sv->networkID = netID;
                            sv->initOutgoing(lh, Servent::T_LOOKUP);
                            servMgr->forceLookup = false;
                        }
                    }
                }

                for (int i=0; i<MAX_TRYOUT; i++)
                {
                    if (servMgr->outUsedFull())
                        break;
                    if (servMgr->tryFull())
                        break;

                    ServHost sh = servMgr->getOutgoingServent(netID);

                    if (!servMgr->addOutgoing(sh.host, netID, false))
                        servMgr->deadHost(sh.host, ServHost::T_SERVENT);
                    sys->sleep(servMgr->tryoutDelay);
                    break;
                }
            }
        }else{
#if 0
            Servent *s = servMgr->servents;
            while (s)
            {
                if (s->type == Servent::T_OUTGOING)
                    s->thread.shutdown();
                s=s->next;
            }
#endif
        }
        sys->sleepIdle();
    }
    thread->unlock();
#endif
    return 0;
}

// -----------------------------------
bool    ServMgr::acceptGIV(ClientSocket *sock)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    Servent *sv = servents;
    while (sv)
    {
        if (sv->type == Servent::T_COUT)
        {
            if (sv->acceptGIV(sock))
                return true;
        }
        sv = sv->next;
    }
    return false;
}

// -----------------------------------
int ServMgr::broadcastPushRequest(ChanHit &hit, Host &to, const GnuID &chanID, Servent::TYPE type)
{
    ChanPacket pack;
    MemoryStream pmem(pack.data, sizeof(pack.data));
    AtomStream atom(pmem);

    atom.writeParent(PCP_BCST, 10);
        atom.writeChar(PCP_BCST_GROUP, PCP_BCST_GROUP_ALL);
        atom.writeChar(PCP_BCST_HOPS, 0);
        atom.writeChar(PCP_BCST_TTL, 7);
        atom.writeBytes(PCP_BCST_DEST, hit.sessionID.id, 16);
        atom.writeBytes(PCP_BCST_FROM, servMgr->sessionID.id, 16);
        atom.writeInt(PCP_BCST_VERSION, PCP_CLIENT_VERSION);
        atom.writeInt(PCP_BCST_VERSION_VP, PCP_CLIENT_VERSION_VP);
        atom.writeBytes(PCP_BCST_VERSION_EX_PREFIX, PCP_CLIENT_VERSION_EX_PREFIX, 2);
        atom.writeShort(PCP_BCST_VERSION_EX_NUMBER, PCP_CLIENT_VERSION_EX_NUMBER);
        atom.writeParent(PCP_PUSH, 3);
            atom.writeInt(PCP_PUSH_IP, to.ip);
            atom.writeShort(PCP_PUSH_PORT, to.port);
            atom.writeBytes(PCP_PUSH_CHANID, chanID.id, 16);

    pack.len = pmem.pos;
    pack.type = ChanPacket::T_PCP;

    return servMgr->broadcastPacket(pack, GnuID(), servMgr->sessionID, hit.sessionID, type);
}

// --------------------------------------------------
void ServMgr::writeRootAtoms(AtomStream &atom, bool getUpdate)
{
    atom.writeParent(PCP_ROOT, 5 + (getUpdate?1:0));
        atom.writeInt(PCP_ROOT_UPDINT, chanMgr->hostUpdateInterval);
        atom.writeString(PCP_ROOT_URL, "download.php");
        atom.writeInt(PCP_ROOT_CHECKVER, PCP_ROOT_VERSION);
        atom.writeInt(PCP_ROOT_NEXT, chanMgr->hostUpdateInterval);
        atom.writeString(PCP_MESG_ASCII, rootMsg.cstr());
        if (getUpdate)
            atom.writeParent(PCP_ROOT_UPDATE, 0);
}

// --------------------------------------------------
void ServMgr::broadcastRootSettings(bool getUpdate)
{
    if (isRoot)
    {
        ChanPacket pack;
        MemoryStream mem(pack.data, sizeof(pack.data));
        AtomStream atom(mem);
        atom.writeParent(PCP_BCST, 9);
            atom.writeChar(PCP_BCST_GROUP, PCP_BCST_GROUP_TRACKERS);
            atom.writeChar(PCP_BCST_HOPS, 0);
            atom.writeChar(PCP_BCST_TTL, 7);
            atom.writeBytes(PCP_BCST_FROM, sessionID.id, 16);
            atom.writeInt(PCP_BCST_VERSION, PCP_CLIENT_VERSION);
            atom.writeInt(PCP_BCST_VERSION_VP, PCP_CLIENT_VERSION_VP);
            atom.writeBytes(PCP_BCST_VERSION_EX_PREFIX, PCP_CLIENT_VERSION_EX_PREFIX, 2);
            atom.writeShort(PCP_BCST_VERSION_EX_NUMBER, PCP_CLIENT_VERSION_EX_NUMBER);
            writeRootAtoms(atom, getUpdate);

        mem.len = mem.pos;
        mem.rewind();
        pack.len = mem.len;

        broadcastPacket(pack, GnuID(), servMgr->sessionID, GnuID(), Servent::T_CIN);
    }
}

// --------------------------------------------------
int ServMgr::broadcastPacket(ChanPacket &pack, const GnuID &chanID, const GnuID &srcID, const GnuID &destID, Servent::TYPE type)
{
    std::lock_guard<std::recursive_mutex> cs(lock);

    int cnt=0;

    Servent *sv = servents;
    while (sv)
    {
        if (sv->sendPacket(pack, chanID, srcID, destID, type))
            cnt++;
        sv=sv->next;
    }
    return cnt;
}

// --------------------------------------------------
int ServMgr::idleProc(ThreadInfo *thread)
{
    sys->setThreadName("IDLE");

    unsigned int lastBroadcastConnect = 0;
    unsigned int lastRootBroadcast = 0;

    unsigned int lastForceIPCheck = 0;

    while (thread->active())
    {
        stats.update();

        unsigned int ctime = sys->getTime();

        if (!servMgr->forceIP.isEmpty())
        {
            if ((ctime - lastForceIPCheck) > 60)
            {
                if (servMgr->checkForceIP())
                {
                    chanMgr->broadcastTrackerUpdate(GnuID(), true);
                }
                lastForceIPCheck = ctime;
            }
        }

        if (chanMgr->isBroadcasting())
        {
            if ((ctime - lastBroadcastConnect) > 30)
            {
                servMgr->connectBroadcaster();
                lastBroadcastConnect = ctime;
            }
        }

        if (servMgr->isRoot)
        {
            if ((ctime - lastRootBroadcast) > chanMgr->hostUpdateInterval)
            {
                servMgr->broadcastRootSettings(true);
                lastRootBroadcast = ctime;
            }
        }

        chanMgr->clearDeadHits(true);

        if (servMgr->shutdownTimer)
        {
            if (--servMgr->shutdownTimer <= 0)
            {
                peercastInst->saveSettings();
                peercastInst->quit();
                sys->exit();
            }
        }

        // shutdown idle channels
        if (chanMgr->numIdleChannels() > ChanMgr::MAX_IDLE_CHANNELS)
            chanMgr->closeOldestIdle();

        // チャンネル一覧を取得する。
        servMgr->channelDirectory->update();

        servMgr->rtmpServerMonitor.update();

        servMgr->uptestServiceRegistry->update();

        sys->sleep(500);
    }

    return 0;
}

// --------------------------------------------------
int ServMgr::serverProc(ThreadInfo *thread)
{
    sys->setThreadName("SERVER");

    Servent *serv = servMgr->allocServent();

    //unsigned int lastLookupTime=0;

    std::unique_lock<std::recursive_mutex> cs(servMgr->lock, std::defer_lock);
    while (thread->active())
    {
        cs.lock();
        if (servMgr->restartServer)
        {
            serv->abort();      // force close

            servMgr->restartServer = false;
        }

        if (servMgr->autoServe)
        {
            std::lock_guard<std::recursive_mutex> cs1(serv->lock);

            // サーバーが既に起動している最中に allow を書き換え続ける
            // の気持ち悪いな。
            serv->allow = servMgr->allowServer1;

            if (!serv->sock)
            {
                LOG_DEBUG("Starting servers");

                if (servMgr->forceNormal)
                    servMgr->setFirewall(ServMgr::FW_OFF);
                else
                    servMgr->setFirewall(ServMgr::FW_UNKNOWN);

                Host h = servMgr->serverHost;

                if (!serv->sock)
                    if (!serv->initServer(h))
                    {
                        LOG_ERROR("Failed to start server on port %d. Exitting...", h.port);
                        peercastInst->quit();
                        sys->exit();
                    }
            }
        }else{
            // stop server
            serv->abort();      // force close

            // cancel incoming connectuions
            Servent *s = servMgr->servents;
            while (s)
            {
                if (s->type == Servent::T_INCOMING)
                    s->thread.shutdown();
                s = s->next;
            }

            servMgr->setFirewall(ServMgr::FW_ON);
        }

        cs.unlock();
        sys->sleepIdle();
    }

    return 0;
}

// -----------------------------------
void    ServMgr::setMaxRelays(int max)
{
    if (max < MIN_RELAYS)
        max = MIN_RELAYS;
    maxRelays = max;
}

// -----------------------------------
XML::Node *ServMgr::createServentXML()
{
    return new XML::Node("servent agent=\"%s\" ", PCX_AGENT);
}

// --------------------------------------------------
const char *ServHost::getTypeStr(TYPE t)
{
    switch(t)
    {
        case T_NONE: return "NONE";
        case T_STREAM: return "STREAM";
        case T_CHANNEL: return "CHANNEL";
        case T_SERVENT: return "SERVENT";
        case T_TRACKER: return "TRACKER";
    }
    return "UNKNOWN";
}

// --------------------------------------------------
ServHost::TYPE ServHost::getTypeFromStr(const char *s)
{
    if (Sys::stricmp(s, "NONE")==0)
        return T_NONE;
    else if (Sys::stricmp(s, "SERVENT")==0)
        return T_SERVENT;
    else if (Sys::stricmp(s, "STREAM")==0)
        return T_STREAM;
    else if (Sys::stricmp(s, "CHANNEL")==0)
        return T_CHANNEL;
    else if (Sys::stricmp(s, "TRACKER")==0)
        return T_TRACKER;

    return T_NONE;
}

// --------------------------------------------------
bool ServMgr::writeVariable(Stream &out, const String &var)
{
    using namespace std;

    string buf;

    if (var == "version")
        buf = PCX_VERSTRING;
    else if (var == "uptime")
    {
        String str;
        str.setFromStopwatch(getUptime());
        buf = str.c_str();
    }else if (var == "numRelays")
        buf = to_string(numStreams(Servent::T_RELAY, true));
    else if (var == "numDirect")
        buf = to_string(numStreams(Servent::T_DIRECT, true));
    else if (var == "totalConnected")
        buf = to_string(totalConnected());
    else if (var == "numServHosts")
        buf = to_string(numHosts(ServHost::T_SERVENT));
    else if (var == "numServents")
        buf = to_string(numServents());
    else if (var == "serverName")
        buf = serverName.c_str();
    else if (var == "serverPort")
        buf = to_string(serverHost.port);
    else if (var == "serverIP")
        buf = serverHost.str(false);
    else if (var == "ypAddress")
        buf = rootHost.c_str();
    else if (var == "password")
        buf = password;
    else if (var == "isFirewalled")
        buf = getFirewall()==FW_ON ? "1" : "0";
    else if (var == "firewallKnown")
        buf = getFirewall()==FW_UNKNOWN ? "0" : "1";
    else if (var == "rootMsg")
        buf = rootMsg.c_str();
    else if (var == "isRoot")
        buf = to_string(isRoot ? 1 : 0);
    else if (var == "isPrivate")
        buf = "0";
    else if (var == "forceYP")
        buf = "0";
    else if (var == "refreshHTML")
        buf = to_string(refreshHTML ? refreshHTML : 0x0fffffff);
    else if (var == "maxRelays")
        buf = to_string(maxRelays);
    else if (var == "maxDirect")
        buf = to_string(maxDirect);
    else if (var == "maxBitrateOut")
        buf = to_string(maxBitrateOut);
    else if (var == "maxControlsIn")
        buf = to_string(maxControl);
    else if (var == "maxServIn")
        buf = to_string(maxServIn);
    else if (var == "numFilters")
        buf = to_string(numFilters+1); // 入力用の空欄を生成する為に+1する。
    else if (var == "numActive1")
        buf = to_string(numActiveOnPort(serverHost.port));
    else if (var == "numCIN")
        buf = to_string(numConnected(Servent::T_CIN));
    else if (var == "numCOUT")
        buf = to_string(numConnected(Servent::T_COUT));
    else if (var == "numIncoming")
        buf = to_string(numActive(Servent::T_INCOMING));
    else if (var == "disabled")
        buf = to_string(isDisabled);
    else if (var == "serverPort1")
        buf = to_string(serverHost.port);
    else if (var == "serverLocalIP")
    {
        Host lh(ClientSocket::getIP(NULL), 0);
        buf = lh.str(false);
    }else if (var == "upgradeURL")
        buf = servMgr->downloadURL;
    else if (var.startsWith("allow."))
    {
        if (var == "allow.HTML1")
            buf = (allowServer1 & Servent::ALLOW_HTML) ? "1" : "0";
        else if (var == "allow.broadcasting1")
            buf = (allowServer1 & Servent::ALLOW_BROADCAST) ? "1" : "0";
        else if (var == "allow.network1")
            buf = (allowServer1 & Servent::ALLOW_NETWORK) ? "1" : "0";
        else if (var == "allow.direct1")
            buf = (allowServer1 & Servent::ALLOW_DIRECT) ? "1" : "0";
    }else if (var.startsWith("auth."))
    {
        if (var == "auth.useCookies")
            buf = (authType==AUTH_COOKIE) ? "1" : "0";
        else if (var == "auth.useHTTP")
            buf = (authType==AUTH_HTTPBASIC) ? "1" : "0";
        else if (var == "auth.useSessionCookies")
            buf = (cookieList.neverExpire==false) ? "1" : "0";
    }else if (var.startsWith("log."))
    {
        if (var == "log.level")
            buf = std::to_string(logLevel());
        else
            return false;
    }else if (var.startsWith("lang."))
    {
        const char* lang = var.c_str() + 5;

        if (strrchr(htmlPath, '/') &&
            strcmp(strrchr(htmlPath, '/') + 1, lang) == 0)
            buf = "1";
        else
            buf = "0";
    }else if (var == "numExternalChannels")
    {
        buf = to_string(channelDirectory->numChannels());
    }else if (var == "numChannelFeedsPlusOne")
    {
        buf = to_string(channelDirectory->numFeeds() + 1);
    }else if (var == "numChannelFeeds")
    {
        buf = to_string(channelDirectory->numFeeds());
    }else if (var.startsWith("channelDirectory."))
    {
        return channelDirectory->writeVariable(out, var + strlen("channelDirectory."));
    }else if (var.startsWith("uptestServiceRegistry."))
    {
        return uptestServiceRegistry->writeVariable(out, var + strlen("uptestServiceRegistry."));
    }else if (var == "transcodingEnabled")
    {
        buf = to_string(servMgr->transcodingEnabled);
    }else if (var == "preset")
    {
        buf = servMgr->preset;
    }else if (var == "audioCodec")
    {
        buf = servMgr->audioCodec;
    }else if (var == "wmvProtocol")
    {
        buf = servMgr->wmvProtocol;
    }else if (var.startsWith("defaultChannelInfo."))
    {
        return servMgr->defaultChannelInfo.writeVariable(out, var + strlen("defaultChannelInfo."));
    }else if (var.startsWith("rtmpServerMonitor."))
    {
        return servMgr->rtmpServerMonitor.writeVariable(out, var + strlen("rtmpServerMonitor."));
    }else if (var == "rtmpPort")
    {
        buf = std::to_string(servMgr->rtmpPort);
    }else if (var == "hasUnsafeFilterSettings")
    {
        buf = std::to_string(servMgr->hasUnsafeFilterSettings());
    }else if (var == "chat")
    {
        buf = to_string(servMgr->chat);
    }else if (var == "test")
    {
        out.writeUTF8(0x304b);
        out.writeUTF8(0x304d);
        out.writeUTF8(0x304f);
        out.writeUTF8(0x3051);
        out.writeUTF8(0x3053);

        out.writeUTF8(0x0041);
        out.writeUTF8(0x0042);
        out.writeUTF8(0x0043);
        out.writeUTF8(0x0044);

        out.writeChar('a');
        out.writeChar('b');
        out.writeChar('c');
        out.writeChar('d');
        return true;
    }else
        return false;

    out.writeString(buf);
    return true;
}

// --------------------------------------------------
void ServMgr::logLevel(int newLevel)
{
    if (1 <= newLevel && newLevel <= 7) // (T_TRACE, T_OFF)
    {
        m_logLevel = newLevel;
    }else
        LOG_ERROR("Trying to set log level outside valid range. Ignored");
}

// --------------------------------------------------
bool ServMgr::hasUnsafeFilterSettings()
{
    for (int i = 0; i < this->numFilters; ++i)
    {
        if (filters[i].isGlobal() && (filters[i].flags & ServFilter::F_PRIVATE) != 0)
            return true;
    }
    return false;
}
