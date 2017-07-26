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
#endif

#include <algorithm>
#include "p8-platform/util/util.h"
#include "kodi/xbmc_addon_cpp_dll.h"

#include "libXBMC_pvr.h"
#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "ott_pvr_client.h"
#include "helpers.h"
#include "ott_player.h"


using namespace std;
using namespace ADDON;

static const char* c_playlist_setting = "ott_playlist_url";
static const char* c_key_setting = "ott_key";

ADDON_STATUS OttPVRClient::Init(CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                               PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    if(ADDON_STATUS_OK != (retVal = PVRClientBase::Init(addonHelper, pvrHelper, pvrprops)))
       return retVal;
    
    char buffer[1024];
    
    if (m_addonHelper->GetSetting(c_playlist_setting, &buffer))
        m_playlistUrl = buffer;
    if (m_addonHelper->GetSetting(c_key_setting, &buffer))
        m_key = buffer;
    
    try
    {
        CreateCore();
    }
    catch (AuthFailedException &)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Login to Sovok.TV failed.");
    }
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;

}

OttPVRClient::~OttPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    if(m_core != NULL)
        SAFE_DELETE(m_core);

}

void OttPVRClient::CreateCore()
{
    if(m_core != NULL)
        SAFE_DELETE(m_core);
    m_core = new OttPlayer(m_addonHelper, m_playlistUrl, m_key);
}

ADDON_STATUS OttPVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    if (strcmp(settingName,  c_playlist_setting) == 0 && strcmp((const char*) settingValue, m_playlistUrl.c_str()) != 0)
    {
        m_playlistUrl = (const char*) settingValue;
        if (!m_playlistUrl.empty() && !m_key.empty() && m_core == NULL)
        {
            try {
                CreateCore();
            }catch (AuthFailedException &) {
                m_addonHelper->QueueNotification(QUEUE_ERROR, "Login to Sovok.TV failed.");
            }
        }
    }
    else if (strcmp(settingName,  c_key_setting) == 0  && strcmp((const char*) settingValue, m_key.c_str()) != 0)
    {
        m_key = (const char*) settingValue;
        if (!m_playlistUrl.empty() && !m_key.empty() && m_core == NULL)
        {
            try {
                CreateCore();
            }catch (AuthFailedException &) {
                m_addonHelper->QueueNotification(QUEUE_ERROR, "Login to Sovok.TV failed.");
            }
        }
    }
    else {
        return PVRClientBase::SetSetting(settingName, settingValue);
    }
    return ADDON_STATUS_NEED_RESTART;
}

PVR_ERROR OttPVRClient::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    pCapabilities->bSupportsEPG = false;
    pCapabilities->bSupportsTV = true;
    pCapabilities->bSupportsRadio = true;
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


PVR_ERROR OttPVRClient::GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd)
{
    return PVR_ERROR_NOT_IMPLEMENTED;
    
    if(NULL == m_core)
        return PVR_ERROR_SERVER_ERROR;
    
    EpgEntryList epgEntries;
    m_core->GetEpg(channel.iUniqueId, iStart, iEnd, epgEntries);
    EpgEntryList::const_iterator itEpgEntry = epgEntries.begin();
    for (int i = 0; itEpgEntry != epgEntries.end(); ++itEpgEntry, ++i)
    {
        EPG_TAG tag = { 0 };
        tag.iUniqueBroadcastId = itEpgEntry->first;
        tag.iChannelNumber = channel.iUniqueId;
        tag.strTitle = itEpgEntry->second.Title.c_str();
        tag.strPlot = itEpgEntry->second.Description.c_str();
        tag.startTime = itEpgEntry->second.StartTime;
        tag.endTime = itEpgEntry->second.EndTime;
        m_pvrHelper->TransferEpgEntry(handle, &tag);
    }
    return PVR_ERROR_NO_ERROR;
}
PVR_ERROR  OttPVRClient::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
//    SovokEpgEntry epgEntry;
//    m_core->FindEpg(item.data.iEpgUid, epgEntry);
    return PVR_ERROR_NOT_IMPLEMENTED;
    
}
int OttPVRClient::GetChannelGroupsAmount()
{
    if(NULL == m_core)
        return -1;

    size_t numberOfGroups = m_core->GetGroupList().size();
    return numberOfGroups;
}

PVR_ERROR OttPVRClient::GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
    if(NULL == m_core)
        return PVR_ERROR_SERVER_ERROR;

    if (!bRadio)
    {
        PVR_CHANNEL_GROUP pvrGroup = { 0 };
        pvrGroup.bIsRadio = false;

        GroupList groups = m_core->GetGroupList();
        GroupList::const_iterator itGroup = groups.begin();
        for (; itGroup != groups.end(); ++itGroup)
        {
            strncpy(pvrGroup.strGroupName, itGroup->first.c_str(), sizeof(pvrGroup.strGroupName));
            m_pvrHelper->TransferChannelGroup(handle, &pvrGroup);
        }
    }

    return PVR_ERROR_NO_ERROR;
}

PVR_ERROR OttPVRClient::GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP& group)
{
    if(NULL == m_core)
        return PVR_ERROR_SERVER_ERROR;

    const GroupList &groups = m_core->GetGroupList();
    GroupList::const_iterator itGroup = groups.find(group.strGroupName);
    if (itGroup != groups.end())
    {
        std::set<OttChannelId>::const_iterator itChannel = itGroup->second.Channels.begin();
        for (; itChannel != itGroup->second.Channels.end(); ++itChannel)
        {
            if (group.strGroupName == itGroup->first)
            {
                PVR_CHANNEL_GROUP_MEMBER pvrGroupMember = { 0 };
                strncpy(pvrGroupMember.strGroupName, itGroup->first.c_str(), sizeof(pvrGroupMember.strGroupName));
                pvrGroupMember.iChannelUniqueId = *itChannel;
                m_pvrHelper->TransferChannelGroupMember(handle, &pvrGroupMember);
            }
        }
    }

    return PVR_ERROR_NO_ERROR;
}

int OttPVRClient::GetChannelsAmount()
{
    if(NULL == m_core)
        return -1;

    return m_core->GetChannelList().size();
}

PVR_ERROR OttPVRClient::GetChannels(ADDON_HANDLE handle, bool bRadio)
{
    if(NULL == m_core)
        return PVR_ERROR_SERVER_ERROR;

    const ChannelList &channels = m_core->GetChannelList();
    ChannelList::const_iterator itChannel = channels.begin();
    for(; itChannel != channels.end(); ++itChannel)
    {
        const OttChannel &channel = itChannel->second;
        if (bRadio == channel.IsRadio)
        {
            PVR_CHANNEL pvrChannel = { 0 };
            pvrChannel.iUniqueId = channel.Id;
            pvrChannel.iChannelNumber = channel.Id;
            pvrChannel.bIsRadio = channel.IsRadio;
            strncpy(pvrChannel.strChannelName, channel.Name.c_str(), sizeof(pvrChannel.strChannelName));

            string iconUrl = channel.IconPath;
            strncpy(pvrChannel.strIconPath, iconUrl.c_str(), sizeof(pvrChannel.strIconPath));;

            m_pvrHelper->TransferChannelEntry(handle, &pvrChannel);
        }
    }

    return PVR_ERROR_NO_ERROR;
}

bool OttPVRClient::OpenLiveStream(const PVR_CHANNEL& channel)
{
    if(NULL == m_core)
        return false;

    string url = m_core->GetUrl(channel.iUniqueId);
    return PVRClientBase::OpenLiveStream(url);
}

bool OttPVRClient::SwitchChannel(const PVR_CHANNEL& channel)
{
    if(NULL == m_core)
        return false;

    string url = m_core->GetUrl(channel.iUniqueId);
    return PVRClientBase::SwitchChannel(url);
}



int OttPVRClient::GetRecordingsAmount(bool deleted)
{
    if(NULL == m_core)
        return -1;

    if(deleted)
        return -1;
    
    int size = 0;
    std::function<void(const ArchiveList&)> f = [&size](const ArchiveList& list){size = list.size();};
    m_core->Apply(f);
    if(size == 0)
    {
        std::function<void(void)> action = [=](){
            m_pvrHelper->TriggerRecordingUpdate();
        };
        m_core->StartArchivePollingWithCompletion(action);
//        m_core->Apply(f);
//        if(size != 0)
//            action();
    
    }
    return size;
    
}
PVR_ERROR OttPVRClient::GetRecordings(ADDON_HANDLE handle, bool deleted)
{
    if(deleted)
        return PVR_ERROR_NOT_IMPLEMENTED;
    
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    OttPlayer& sTV(*m_core);
    CHelper_libXBMC_pvr * pvrHelper = m_pvrHelper;
    ADDON::CHelper_libXBMC_addon * addonHelper = m_addonHelper;
    std::function<void(const ArchiveList&)> f = [&sTV, &handle, pvrHelper, addonHelper ,&result](const ArchiveList& list){
        for(const auto &  i :  list) {
            try {
                const OttEpgEntry& epgTag = sTV.GetEpgList().at(i);

                PVR_RECORDING tag = { 0 };
    //            memset(&tag, 0, sizeof(PVR_RECORDING));
                sprintf(tag.strRecordingId, "%d",  i);
                strncpy(tag.strTitle, epgTag.Title.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
                strncpy(tag.strPlot, epgTag.Description.c_str(), PVR_ADDON_DESC_STRING_LENGTH - 1);
                strncpy(tag.strChannelName, sTV.GetChannelList().at(epgTag.ChannelId).Name.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
                tag.recordingTime = epgTag.StartTime;
                tag.iLifetime = 0; /* not implemented */

                tag.iDuration = epgTag.EndTime - epgTag.StartTime;
                tag.iEpgEventId = i;
                tag.iChannelUid = epgTag.ChannelId;

                string dirName = tag.strChannelName;
                char buff[20];
                strftime(buff, sizeof(buff), "/%d-%m-%y", localtime(&epgTag.StartTime));
                dirName += buff;
                strncpy(tag.strDirectory, dirName.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);

                pvrHelper->TransferRecordingEntry(handle, &tag);
                
            }
            catch (...)  {
                addonHelper->Log(LOG_ERROR, "%s: failed.", __FUNCTION__);
                result = PVR_ERROR_FAILED;
            }

        }
    };
    m_core->Apply(f);
    return result;
}

bool OttPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(NULL == m_core)
        return PVR_ERROR_SERVER_ERROR;

    OttEpgEntry epgTag;
    
    unsigned int epgId = recording.iEpgEventId;
    if( epgId == 0 )
        epgId = strtoi(recording.strRecordingId);
    if(!m_core->FindEpg(epgId, epgTag))
        return false;
    
    string url = m_core->GetArchiveForEpg(epgTag);
    return PVRClientBase::OpenRecordedStream(url);
}

PVR_ERROR OttPVRClient::CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    m_addonHelper->Log(LOG_DEBUG, " >>>> !!!! Menu hook !!!! <<<<<");
    return MenuHook(menuhook, item);
}

PVR_ERROR OttPVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Sovok TV Adapter 1");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_core == NULL) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}

ADDON_STATUS OttPVRClient::GetStatus(){ return  /*(m_core == NULL)? ADDON_STATUS_LOST_CONNECTION : */ADDON_STATUS_OK;}
       



