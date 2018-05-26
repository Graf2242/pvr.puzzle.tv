/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
 *
 *  Copyright (C) 2013 Alex Deryskyba (alex@codesnake.com)
 *  https://bitbucket.org/codesnake/pvr.sovok.tv_xbmc_addon
 *
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#if (defined(_WIN32) || defined(__WIN32__))
#include <windows.h>
#ifdef GetObject
#undef GetObject
#endif
#endif

#include <algorithm>
#include <ctime>
#include "p8-platform/util/util.h"
#include "kodi/xbmc_addon_cpp_dll.h"

#include "libXBMC_pvr.h"
#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "edem_pvr_client.h"
#include "helpers.h"
#include "edem_player.h"
#include "plist_buffer.h"

using namespace std;
using namespace ADDON;
using namespace EdemEngine;
using namespace PvrClient;

static const char* c_playlist_setting = "edem_playlist_url";
static const char* c_epg_setting = "edem_epg_url";
static const char* c_seek_archives = "edem_seek_archives";

ADDON_STATUS EdemPVRClient::Init(CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,                               PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    if(ADDON_STATUS_OK != (retVal = PVRClientBase::Init(addonHelper, pvrHelper, pvrprops)))
        return retVal;
    
    char buffer[1024];
    
    if (m_addonHelper->GetSetting(c_playlist_setting, &buffer))
        m_playlistUrl = buffer;
    if (m_addonHelper->GetSetting(c_epg_setting, &buffer))
        m_epgUrl = buffer;
    
    m_supportSeek = false;
    m_addonHelper->GetSetting(c_seek_archives, &m_supportSeek);
    
    try
    {
        CreateCore(false);
    }
    catch (AuthFailedException &)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32003));
    }
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;
    
}

EdemPVRClient::~EdemPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    if(m_core != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_core);
    }
    
}

void EdemPVRClient::CreateCore(bool clearEpgCache)
{
    if(m_core != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_core);
    }
    if(CheckPlaylistUrl())
        m_clientCore = m_core = new EdemEngine::Core(m_addonHelper, m_pvrHelper, m_playlistUrl, m_epgUrl, clearEpgCache);
}

bool EdemPVRClient::CheckPlaylistUrl()
{
    if (m_playlistUrl.empty() || m_playlistUrl.find("***") != string::npos) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32010));
        return false;
    }
    return true;
}

ADDON_STATUS EdemPVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;
    
    if (strcmp(settingName,  c_playlist_setting) == 0 && strcmp((const char*) settingValue, m_playlistUrl.c_str()) != 0) {
        m_playlistUrl= (const char*) settingValue;
        if(!CheckPlaylistUrl()) {
            return result;
        } else {
            try {
                CreateCore(false);
                result = ADDON_STATUS_NEED_RESTART;
            }catch (AuthFailedException &) {
                m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32011));
            }
        }
    }
    else if(strcmp(settingName,  c_epg_setting) == 0 && strcmp((const char*) settingValue, m_epgUrl.c_str()) != 0) {
        m_epgUrl = (const char*) settingValue;
        try {
            CreateCore(false);
            result = ADDON_STATUS_NEED_RESTART;
        }catch (AuthFailedException &) {
            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32011));
        }
    }
    else if(strcmp(settingName,  c_seek_archives) == 0) {
        m_supportSeek = *(const bool*) settingValue;
    }
    else {
        result = PVRClientBase::SetSetting(settingName, settingValue);
    }
    return result;
}

PVR_ERROR EdemPVRClient::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    pCapabilities->bSupportsRadio = false;
    pCapabilities->bSupportsChannelGroups = true;
    pCapabilities->bHandlesInputStream = true;
    pCapabilities->bSupportsRecordings = true;
    
    pCapabilities->bSupportsTimers = false;
    pCapabilities->bSupportsChannelScan = false;
    pCapabilities->bHandlesDemuxing = false;
    pCapabilities->bSupportsRecordingPlayCount = false;
    pCapabilities->bSupportsLastPlayedPosition = false;
    pCapabilities->bSupportsRecordingEdl = false;
    
    return PVRClientBase::GetAddonCapabilities(pCapabilities);
}

PVR_ERROR  EdemPVRClient::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    return PVRClientBase::MenuHook(menuhook, item);
}

ADDON_STATUS EdemPVRClient::OnReloadEpg()
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    try
    {
        CreateCore(true);
    }
    catch (AuthFailedException &)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32007));
    }
    catch(...)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Edem TV: unhandeled exception on reload EPG.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    
    if(ADDON_STATUS_OK == retVal && nullptr != m_core){
        std::time_t startTime = std::time(nullptr);
        startTime = std::mktime(std::gmtime(&startTime));
        // Request EPG for all channels from -7 to +1 days
        time_t endTime = startTime + 1 * 24 * 60 * 60;
        startTime -= 7 * 24 * 60 * 60;
        
        m_core->UpdateEpgForAllChannels(startTime, endTime);
    }
    
    return retVal;
}


bool EdemPVRClient::OpenLiveStream(const PVR_CHANNEL& channel)
{
    if(NULL == m_core)
        return false;
    
    string url = m_core->GetUrl(channel.iUniqueId);
    return PVRClientBase::OpenLiveStream(url);
}

bool EdemPVRClient::SwitchChannel(const PVR_CHANNEL& channel)
{
    if(NULL == m_core)
        return false;
    
    string url = m_core->GetUrl(channel.iUniqueId);
    return PVRClientBase::SwitchChannel(url);
}

class EdemArchiveDelegate : public Buffers::IPlaylistBufferDelegate
{
public:
    EdemArchiveDelegate(EdemEngine::Core* core, const PVR_RECORDING &recording)
    : _duration(recording.iDuration)
    , _recordingTime(recording.recordingTime)
    , _core(core)
    {
        _channelId = 1;
        
        // NOTE: Kodi does NOT provide recording.iChannelUid for unknown reason
        // Worrkaround: use EPG entry
        EpgEntry epgTag;
        int recId = stoi(recording.strRecordingId);
        if(!_core->GetEpgEpgEntry(recId, epgTag)){
            _core->LogError("Failed to obtain EPG tag for record ID %d. First channel ID will be used", recId);
            return;
        }
        
        _channelId =  epgTag.ChannelId;
    }
    virtual time_t Duration() const
    {
        time_t fromNow = time(nullptr) - _recordingTime;
        return std::min(_duration, fromNow) ;
    }
    virtual std::string UrlForTimeshift(time_t timeshiftReqested, time_t* timeshiftAdjusted = nullptr) const
    {
        //auto startTime = std::min(_recordingTime + timeshiftReqested, _recordingTime + Duration());
        auto startTime = _recordingTime + timeshiftReqested;
        if(startTime < _recordingTime)
            startTime = _recordingTime;
        if(timeshiftAdjusted)
            *timeshiftAdjusted = startTime - _recordingTime;
        return  _core->GetArchiveUrl(_channelId, startTime);
    }
    
private:
    const time_t _duration;
    const time_t _recordingTime;
    PvrClient::ChannelId _channelId;
    EdemEngine::Core* _core;
};

bool EdemPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(NULL == m_core)
        return false;
    
    auto delegate = new EdemArchiveDelegate(m_core, recording);
    string url = delegate->UrlForTimeshift(0);
    if(m_supportSeek)
        return PVRClientBase::OpenRecordedStream(url, delegate);
    return PVRClientBase::OpenRecordedStream(url, nullptr);
}

PVR_ERROR EdemPVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Edem TV");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_core == NULL) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}



