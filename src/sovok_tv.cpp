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

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include <assert.h>
#include <algorithm>
#include <sstream>
#include <ctime>
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/timeutils.h"
#include "p8-platform/util/util.h"
#include "helpers.h"
#include "sovok_tv.h"
#include "HttpEngine.hpp"

using namespace std;
using namespace ADDON;
using namespace rapidjson;
using namespace PvrClient;

#define CATCH_API_CALL(msg) \
    catch (ServerErrorException& ex) { \
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32009), ex.reason.c_str() ); \
    } catch(CurlErrorException& ex) { \
        m_addonHelper->QueueNotification(QUEUE_ERROR, "CURL fatal error: %s", ex.reason.c_str() ); \
    } catch (...) { \
        Log(msg); \
    }

static const int secondsPerHour = 60 * 60;

//
static const char* c_EpgCacheDirPath = "special://temp/pvr-puzzle-tv";

static const char* c_EpgCacheFilePath = "special://temp/pvr-puzzle-tv/epg_cache.txt";

static void BeutifyUrl(string& url);
struct SovokTV::ApiFunctionData
{
    enum API_Version
    {
        API_2_2,
        API_2_3
    };
    
//    ApiFunctionData(const char* _name)
//    : name(_name) , params(s_EmptyParams), api_ver (API_2_2)
//    {}
//    
    ApiFunctionData(const char* _name, const ParamList& _params = s_EmptyParams, API_Version _version = API_2_2)
    : name(_name) , params(_params), api_ver (_version)
    {}
    std::string name;
    const ParamList params;
    API_Version api_ver;
    static const  ParamList s_EmptyParams;
};

const ParamList SovokTV::ApiFunctionData::s_EmptyParams;


//tatic         P8PLATFORM::CTimeout TEST_LOGIN_FAILED_timeout(30 * 1000);


SovokTV::SovokTV(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                 const string &login, const string &password,
                 const std::function<void(void)>& archiveUpdateDone,  bool cleanEpgCache) :
    m_addonHelper(addonHelper),
    m_pvrHelper(pvrHelper),
    m_login(login),
    m_password(password),
    m_lastEpgRequestEndTime(0),
    m_archiveUpdateDone(archiveUpdateDone)
{
    m_httpEngine = new HttpEngine(m_addonHelper);
    if (/*TEST_LOGIN_FAILED_timeout.TimeLeft() > 0 ||*/  !Login(true)) {
        Cleanup();
        throw AuthFailedException();
    }
    if(!LoadStreamers()) {
        Cleanup();
        throw MissingApiException("streamers");
    }
    LoadSettings();
    InitArchivesInfo();
    if(cleanEpgCache)
        m_addonHelper->DeleteFile(c_EpgCacheFilePath);
    else
        LoadEpgCache();
}

SovokTV::~SovokTV()
{
    Cleanup();
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    m_epgEntries.clear();
}

void SovokTV::Cleanup()
{
    m_addonHelper->Log(LOG_NOTICE, "SovokTV stopping...");

    if(m_httpEngine)
        m_httpEngine->CancelAllRequests();
    
    Logout();
    
    if(m_httpEngine)
        SAFE_DELETE(m_httpEngine);
    //m_addonHelper->Log(LOG_NOTICE, "HTTP engine stopped.");

    m_addonHelper->Log(LOG_NOTICE, "SovokTV stopped.");
}

template< typename ContainerT, typename PredicateT >
void erase_if( ContainerT& items, const PredicateT& predicate ) {
    for( auto it = items.begin(); it != items.end(); ) {
        if( predicate(*it) ) it = items.erase(it);
        else ++it;
    }
};
void SovokTV::SaveEpgCache()
{
    // Leave epg entries not older then 2 weeks from now
    time_t now = time(nullptr);
    auto oldest =  now - 14*24*60*60; //m_lastEpgRequestStartTime = max(m_lastEpgRequestStartTime, now - 14*24*60*60);
    erase_if(m_epgEntries,  [oldest] (const EpgEntryList::value_type& i)
             {
                 return i.second.StartTime < oldest;
             });

    StringBuffer s;
    Writer<StringBuffer> writer(s);
    
    writer.StartObject();               // Between StartObject()/EndObject(),

    writer.Key("m_epgEntries");
    writer.StartArray();                // Between StartArray()/EndArray(),
    for_each(m_epgEntries.begin(), m_epgEntries.end(),[&](const EpgEntryList::value_type& i) {
        writer.StartObject();               // Between StartObject()/EndObject(),
        writer.Key("k");
        writer.Int64(i.first);
        writer.Key("v");
        i.second.Serialize(writer);
        writer.EndObject();
    });
    writer.EndArray();
             
    writer.EndObject();

    m_addonHelper->CreateDirectory(c_EpgCacheDirPath);
    
    void* file = m_addonHelper->OpenFileForWrite(c_EpgCacheFilePath, true);
    if(NULL == file)
        return;
    auto buf = s.GetString();
    m_addonHelper->WriteFile(file, buf, s.GetSize());
    m_addonHelper->CloseFile(file);
    
}
void SovokTV::LoadEpgCache()
{
    void* file = m_addonHelper->OpenFile(c_EpgCacheFilePath, 0);
    if(NULL == file)
        return;
    int64_t fSize = m_addonHelper->GetFileLength(file);
    
    char* rawBuf = new char[fSize + 1];
    if(0 == rawBuf)
        return;
    m_addonHelper->ReadFile(file, rawBuf, fSize);
    m_addonHelper->CloseFile(file);
    file = NULL;
    
    rawBuf[fSize] = 0;
    
    string ss(rawBuf);
    delete[] rawBuf;
    try {
        ParseJson(ss, [&] (Document& jsonRoot) {
            
            const Value& v = jsonRoot["m_epgEntries"];
            Value::ConstValueIterator it = v.Begin();
            for(; it != v.End(); ++it)
            {
                EpgEntryList::key_type k = (*it)["k"].GetInt64();
                EpgEntryList::mapped_type e;
                e.Deserialize((*it)["v"]);
                AddEpgEntry(k, e);
            }
        });
        OnEpgUpdateDone();

    } catch (...) {
        Log(" >>>>  FAILED load EPG cache <<<<<");
        m_epgEntries.clear();
    }
}

void SovokTV::OnEpgUpdateDone()
{
    m_addonHelper->Log(LOG_NOTICE, "Archive thread iteraton started");
    // Localize EPG lock
    {
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);
        m_addonHelper->Log(LOG_DEBUG, "Archive thread: EPG size %d", m_epgEntries.size());
        int recCounter = 0;
        for(auto& i : m_epgEntries) {
            UpdateHasArchive(i.second);
            if(i.second.HasArchive)
                ++recCounter;
        }
        m_addonHelper->Log(LOG_DEBUG, "Archive thread: Recordings size %d", recCounter);
        SaveEpgCache();
    }
    if(m_archiveUpdateDone)
        m_archiveUpdateDone();
    m_addonHelper->Log(LOG_NOTICE, "Archive thread iteraton done");
}

const ChannelList &SovokTV::GetChannelList()
{
    if (m_channelList.empty())
    {
        BuildChannelAndGroupList();
    }

    return m_channelList;
}

const EpgEntryList& SovokTV::GetEpgList() const
{
    return  m_epgEntries;
}
const StreamerNamesList& SovokTV::GetStreamersList() const
{
    return  m_streamerNames;
}

template <typename TParser>
void SovokTV::ParseJson(const std::string& response, TParser parser)
{
    Document jsonRoot;
    jsonRoot.Parse(response.c_str());
    if(jsonRoot.HasParseError()) {
        
        ParseErrorCode error = jsonRoot.GetParseError();
        auto strError = string("Rapid JSON parse error: ");
        strError += GetParseError_En(error);
        strError += " (" ;
        strError += error;
        strError += ").";
        Log(strError.c_str());
        throw JsonParserException(strError);
    }
    parser(jsonRoot);
    return;

}

void SovokTV::SetCountryFilter(const CountryFilter& filter)
{
    m_channelList.clear();
    m_groupList.clear();
    m_countryFilter = filter;
}
void SovokTV::BuildChannelAndGroupList()
{
    m_channelList.clear();
    m_groupList.clear();

    const bool adultContentDisabled = m_pinCode.empty();
    try {

        ApiFunctionData params("channel_list2");
        CallApiFunction(params, [&] (Document& jsonRoot)
        {
            int maxGroupId = 0;
            for(auto& gr : jsonRoot["groups"].GetArray())
            {
                auto id = atoi(gr["id"].GetString());
                if(maxGroupId < id)
                    maxGroupId = id;
                Group group;
                group.Name = gr["name"].GetString();
                m_groupList[id] = group;
            }
            if(m_countryFilter.IsOn) {
                for(int i = 0; i < m_countryFilter.Filters.size(); ++i){
                    auto& f = m_countryFilter.Filters[i];
                    if(f.Hidden)
                        continue;
                    Group group;
                    group.Name = f.GroupName;
                    m_groupList[++maxGroupId] = group;
                    m_countryFilter.Groups[i] = maxGroupId;
                }
            }
            for(auto& ch : jsonRoot["channels"].GetArray())
            {
                
                bool isProtected  = atoi(ch["protected"].GetString()) !=0;
                if(adultContentDisabled && isProtected)
                    continue;
                
                Channel channel;
                channel.Id = atoi(ch["id"].GetString());
                channel.Number = channel.Id;
                channel.Name = ch["name"].GetString();
                channel.IconPath = string("http://sovok.tv" )+ ch["icon"].GetString();
                channel.IsRadio = ch["is_video"].GetInt() == 0;//channel.Id > 1000;
                channel.HasArchive = atoi(ch["have_archive"].GetString()) != 0;
                
                bool hideChannel = false;
                if(m_countryFilter.IsOn) {
                    for(int i = 0; i < m_countryFilter.Filters.size(); ++i){
                        auto& f = m_countryFilter.Filters[i];
                        if(channel.Name.find(f.FilterPattern) != string::npos) {
                            hideChannel = f.Hidden;
                            if(hideChannel)
                                break;
                            m_groupList[m_countryFilter.Groups[i]].Channels.insert(channel.Id);
                        }
                    }
                }
                if(hideChannel)
                    continue;
                
                m_channelList[channel.Id] = channel;
                string groups = ch["groups"].GetString();
                while(!groups.empty()) {
                    auto pos = groups.find(',');
                    int id = stoi(groups.substr(0,pos));
                    if(string::npos == pos) {
                        groups = "";
                    } else {
                        groups = groups.substr(pos + 1);
                    }
                    Group &group = m_groupList[id];
                    group.Channels.insert(channel.Id);
                }
            }
        });
    }
    CATCH_API_CALL(">>>>  FAILED to build channel list <<<<<")
}


std::string SovokTV::GetArchiveUrl(ChannelId channelId, time_t startTime)
{
    string url;
    ParamList params;
    params["cid"] = n_to_string(channelId);
    params["time"] = n_to_string(startTime);
     try {
         ApiFunctionData apiParams("archive_next", params);
         CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value & archive = jsonRoot["archive"];
            
            url = archive["url"].GetString();
            BeutifyUrl(url);
            //Log((string(" >>>>  URL: ") + url +  "<<<<<").c_str());
        });
     }
    CATCH_API_CALL(" >>>>  FAILED receive archive <<<<<")

    return url;
}

void SovokTV::Log(const char* massage) const
{
    //char* msg = m_addonHelper->UnknownToUTF8(massage);
    m_addonHelper->Log(LOG_DEBUG, massage);
    //m_addonHelper->FreeString(msg);
    
}

void SovokTV::GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries)
{
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    
    bool needMore = true;
    time_t moreStartTime = startTime;
    for (const auto& i  : m_epgEntries)  {
        auto entryStartTime = i.second.StartTime;
        if (i.second.ChannelId == channelId  &&
            entryStartTime >= startTime &&
            entryStartTime < endTime)
        {
            moreStartTime = i.second.EndTime;
            needMore = moreStartTime < endTime;
            epgEntries.insert(i);
        }
    }
    
    if(!needMore)
        return;
    
    auto epgRequestStart = max(moreStartTime, m_lastEpgRequestEndTime);

    if(endTime > epgRequestStart) {
        m_lastEpgRequestEndTime = endTime;
        GetEpgForAllChannels(epgRequestStart, endTime);
    }
}


void SovokTV::GetEpgForAllChannels(time_t startTime, time_t endTime)
{

    int64_t totalNumberOfHours = (endTime - startTime) / secondsPerHour;
    int64_t hoursRemaining = totalNumberOfHours;
    const int64_t hours24 = 24;
    
    while (hoursRemaining > 0)
    {
        // Query EPG for max 24 hours per single request.
        const int64_t requestNumberOfHours = min(hours24, hoursRemaining);

        GetEpgForAllChannelsForNHours(startTime, requestNumberOfHours);
        
        hoursRemaining -= requestNumberOfHours;
        startTime += requestNumberOfHours * secondsPerHour;
    }

}

void SovokTV::GetEpgForAllChannelsForNHours(time_t startTime, short numberOfHours)
{
    // For queries over 24 hours Sovok.TV returns incomplete results.
    assert(numberOfHours > 0 && numberOfHours <= 24);

    ParamList params;
    params["dtime"] = n_to_string(startTime + m_serverTimeShift);
    params["period"] = n_to_string(numberOfHours);

    
    char       buf[80];
    struct tm  tstruct;
    tstruct = *localtime(&startTime);
    // Visit http://en.cppreference.com/w/cpp/chrono/c/strftime
    // for more information about date/time format
    strftime(buf, sizeof(buf), "%Y-%m-%d.%X", &tstruct);

    m_addonHelper->Log(LOG_DEBUG, "Scheduled EPG upfdate (all channels) from %s for %d hours.", buf, numberOfHours);

    ApiFunctionData apiParams("epg3", params);
    unsigned int epgActivityCounter = ++m_epgActivityCounter;
    CallApiAsync(apiParams, [this, numberOfHours, startTime] (Document& jsonRoot)
    {
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);
        const Value &channels = jsonRoot["epg3"];
        Value::ConstValueIterator itChannel = channels.Begin();
        for (; itChannel != channels.End(); ++itChannel)
        {
            const auto currentChannelId = stoul((*itChannel)["id"].GetString());
            // Check last EPG entrie for missing end time
            auto lastEpgForChannel = m_epgEntries.begin();
            for(auto runner = m_epgEntries.begin(), end = m_epgEntries.end(); runner != end; ++runner) {
                if(lastEpgForChannel != end &&
                   runner->second.ChannelId == currentChannelId &&
                   lastEpgForChannel->second.StartTime <= runner->second.StartTime)
            
                lastEpgForChannel = runner;
            }
            
            const Value& jsonChannelEpg = (*itChannel)["epg"];
            Value::ConstValueIterator itJsonEpgEntry1 = jsonChannelEpg.Begin();
            Value::ConstValueIterator itJsonEpgEntry2  = itJsonEpgEntry1;
            itJsonEpgEntry2++;
            // Fix end time of last enrty for the channel
            // It can't be calculated during previous iteation.
            if(lastEpgForChannel != m_epgEntries.end()) {
                lastEpgForChannel->second.EndTime = stol((*itJsonEpgEntry1)["ut_start"].GetString()) - m_serverTimeShift;
            }
            for (; itJsonEpgEntry2 != jsonChannelEpg.End(); ++itJsonEpgEntry1, ++itJsonEpgEntry2)
            {
                EpgEntry epgEntry;
                epgEntry.ChannelId = currentChannelId;
                epgEntry.Title = (*itJsonEpgEntry1)["progname"].GetString();
                epgEntry.Description = (*itJsonEpgEntry1)["description"].GetString();
                epgEntry.StartTime = stol((*itJsonEpgEntry1)["ut_start"].GetString()) - m_serverTimeShift;
                epgEntry.EndTime = stol((*itJsonEpgEntry2)["ut_start"].GetString()) - m_serverTimeShift;
                
                UniqueBroadcastIdType id = epgEntry.StartTime;
                AddEpgEntry(id, epgEntry);
            }
            // Last EPG entrie  missing end time.
            // Put end of requested interval
            EpgEntry epgEntry;
            epgEntry.ChannelId = currentChannelId;
            epgEntry.Title = (*itJsonEpgEntry1)["progname"].GetString();
            epgEntry.Description = (*itJsonEpgEntry1)["description"].GetString();
            epgEntry.StartTime = stol((*itJsonEpgEntry1)["ut_start"].GetString()) - m_serverTimeShift;
            epgEntry.EndTime = startTime + numberOfHours * 60 * 60;
            
            UniqueBroadcastIdType id = epgEntry.StartTime;
            AddEpgEntry(id, epgEntry);
            m_pvrHelper->TriggerEpgUpdate(currentChannelId);
        }
    },
     [this, epgActivityCounter](const CActionQueue::ActionResult& s)
     {
         if(epgActivityCounter == m_epgActivityCounter){
             OnEpgUpdateDone();
         }

         try {
             if(s.exception)
                rethrow_exception(s.exception);
         } catch (ServerErrorException& ex) {
             m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32009), ex.reason.c_str() );
         } catch (...) {
             Log(" >>>>  FAILED receive EPG for N hours<<<<<");
         }
     });
}

void SovokTV::UpdateHasArchive(EpgEntry& entry) const
{
    auto channel = m_channelList.find(entry.ChannelId);
    entry.HasArchive = channel != m_channelList.end() &&  channel->second.HasArchive;
    
    if(!entry.HasArchive)
        return;
    
    time_t to = time(nullptr);
    time_t from = to - m_archivesInfo.at(entry.ChannelId) * 60 * 60;
    entry.HasArchive = entry.StartTime >= from && entry.StartTime < to;
}

void SovokTV::AddEpgEntry(UniqueBroadcastIdType id, EpgEntry& entry)
{
    UpdateHasArchive(entry);
    while( m_epgEntries.count(id) != 0) {
        // Check duplicates.
        if(m_epgEntries[id].ChannelId == entry.ChannelId) return;
        ++id;
    }
    m_epgEntries[id] =  entry;
}

const GroupList &SovokTV::GetGroupList()
{
    if (m_groupList.empty())
        BuildChannelAndGroupList();

    return m_groupList;
}

string SovokTV::GetUrl(ChannelId channelId)
{
    string url;

    ParamList params;
    params["cid"] = n_to_string(channelId);
    params["protect_code"] = m_pinCode.c_str();
    try {
        ApiFunctionData apiParams("get_url", params);
        CallApiFunction(apiParams, [&] (Document& jsonRoot)
       {
            url = jsonRoot["url"].GetString();
            BeutifyUrl(url);
       });
    }
    CATCH_API_CALL((string(" >>>>  FAILED to get URL for channel ID=" ) + n_to_string(channelId) + " <<<<<") .c_str())

    return url;
}

void BeutifyUrl(string& url)
{
    url.replace(0, 7, "http");  // replace http/ts with http
    url = url.substr(0, url.find(" ")); // trim VLC params at the end
}

FavoriteList SovokTV::GetFavorites()
{
    FavoriteList favorites;
    try {
        ApiFunctionData apiParams("favorites");
        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &jsonFavorites = jsonRoot["favorites"];
            Value::ConstValueIterator itFavorite = jsonFavorites.Begin();
            for(; itFavorite != jsonFavorites.End(); ++itFavorite)
                favorites.insert((*itFavorite)["channel_id"].GetInt());
        });
    }
    CATCH_API_CALL(" >>>>  FAILED to get favorites <<<<<")

    return favorites;
}


bool SovokTV::Login(bool wait)
{

    if (m_login.empty() || m_password.empty())
        return false;

    ParamList params;
    params["login"] = m_login;
    params["pass"] = m_password;

    auto parser = [=] (Document& jsonRoot)
    {
        m_httpEngine->m_sessionCookie.clear();
        
        string sid = jsonRoot["sid"].GetString();
        string sidName = jsonRoot["sid_name"].GetString();
        
        if (sid.empty() || sidName.empty())
            throw BadSessionIdException();
        m_httpEngine->m_sessionCookie[sidName] = sid;
    };
    
    if(wait) {
        try {
            ApiFunctionData apiParams("login", params);
            CallApiFunction(apiParams, parser);
        } catch (ServerErrorException& ex) {
            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32009), ex.reason.c_str() );
            return false;
        } catch(CurlErrorException& ex) {
            m_addonHelper->QueueNotification(QUEUE_ERROR, "CURL fatal error: %s", ex.reason.c_str() );
            return false;
        } catch (...) {
            Log(" >>>>  FAILED to LOGIN!!! <<<<<");
            return false;
            
        }
        return true;
    } else {
        ApiFunctionData apiParams("login", params);
        CallApiAsync(apiParams, parser, [=] (const CActionQueue::ActionResult& s){
            if(s.exception) {
                try {
                    std::rethrow_exception(s.exception);
                }
                CATCH_API_CALL(" >>>>  FAILED to LOGIN!!! <<<<<")
               
            }
            
        });
        return false;
    }
    
}

void SovokTV::Logout()
{
    ApiFunctionData apiParams("logout");

    try {CallApiFunction(apiParams, [&] (Document& jsonRoot){});}catch (...) {}
}

template <typename TParser>
void SovokTV::CallApiFunction(const ApiFunctionData& data, TParser parser)
{
    P8PLATFORM::CEvent event;
    std::exception_ptr ex = nullptr;
    CallApiAsync(data, parser, [&](const CActionQueue::ActionResult& s) {
        ex = s.exception;
        event.Signal();
    });
    event.Wait();
    if(ex)
        std::rethrow_exception(ex);
}

template <typename TParser>
void SovokTV::CallApiAsync(const ApiFunctionData& data, TParser parser, TApiCallCompletion completion)
{
    
    // Build HTTP request
    string query;
    ParamList::const_iterator runner = data.params.begin();
    ParamList::const_iterator first = runner;
    ParamList::const_iterator end = data.params.end();
    
    for (; runner != end; ++runner)
    {
        query += runner == first ? "?" : "&";
        query += runner->first + '=' + runner->second;
    }
    std::string strRequest = "http://api.sovok.tv/v2.3";
    //strRequest += (data.api_ver == ApiFunctionData::API_2_2) ? "v2.2" : "v2.3";
    strRequest += "/json/";
    strRequest += data.name + query;
    auto start = P8PLATFORM::GetTimeMs();
    const bool isLoginCommand = data.name == "login" || data.name == "logout";
    m_addonHelper->Log(LOG_DEBUG, "Calling '%s'.",  data.name.c_str());

    std::function<void(const std::string&)> parserWrapper = [=](const std::string& response) {
        m_addonHelper->Log(LOG_DEBUG, "Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);
        
        //            if(data.name.compare( "get_url") == 0)
        //                m_addonHelper->Log(LOG_DEBUG, response.substr(0, 16380).c_str());
        
        ParseJson(response, [&] (Document& jsonRoot)
                  {
                      if (!jsonRoot.HasMember("error"))
                      {
                          parser(jsonRoot);
                          return;
                      }
                      const Value & errObj = jsonRoot["error"];
                      auto err = errObj["message"].GetString();
                      const Value & errCode = errObj["code"];
                      auto code = errCode.IsInt() ? errCode.GetInt() : errCode.IsString() ? atoi(errCode.GetString()) : -100;
                      m_addonHelper->Log(LOG_ERROR, "Sovok TV server responses error:");
                      m_addonHelper->Log(LOG_ERROR, err);
                      throw ServerErrorException(err,code);
                  });
    };
    m_httpEngine->CallApiAsync(strRequest, parserWrapper, [=](const CActionQueue::ActionResult& s)
                 {
                     // Do not re-login within login/logout command.
                     if(s.status == CActionQueue::kActionCompleted || isLoginCommand) {
                         completion(s);
                         return;
                     }
                     // In case of error try to re-login and repeat the API call.
                     Login(false);
                    m_httpEngine->CallApiAsync(strRequest, parserWrapper,  [=](const CActionQueue::ActionResult& ss){
                                     completion(ss);
                                 });
                 });
}

bool SovokTV::LoadStreamers()
{
    try {
        ApiFunctionData apiParams("streamers");

        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            
            const Value &jsonStreamers = jsonRoot["streamers"];
            if(!jsonStreamers.IsArray())
                throw  JsonParserException("'streamers' is not array");
            m_streamerIds.clear();
            m_streamerNames.clear();
            Value::ConstValueIterator runner = jsonStreamers.Begin();
            Value::ConstValueIterator end = jsonStreamers.End();
            while(runner != end)
            {
                m_streamerNames.push_back((*runner)["name"].GetString());
                m_streamerIds.push_back((*runner)["id"].GetString());
                ++runner;
            }
            m_addonHelper->Log(LOG_DEBUG,"Loaded %d streamers.", m_streamerNames.size());
        });
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32009), ex.reason.c_str() );
    } catch(CurlErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "CURL fatal error: %s", ex.reason.c_str() );
        return false;
    } catch (...) {
        Log(" >>>>  FAILED to load streamers <<<<<");
        return false;
    }
    return true;
}


void SovokTV::LoadSettings()
{
    // Load streamers
    try {
        ApiFunctionData apiParams("settings");

        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &jsonSettings = jsonRoot["settings"];
            std::string streamerID(jsonSettings["streamer"].GetString());
           
            auto timeString = jsonSettings["timezone"].GetString();
            int hours, minutes, seconds;
            sscanf(timeString, "%d:%d:%d", &hours, &minutes, &seconds);
            m_serverTimeShift = seconds + 60 * minutes + 60*60*hours;
            
            m_streammerId = 0;
            auto streamer = std::find_if (m_streamerIds.begin(), m_streamerIds.end(), [=](StreamerIdsList::value_type i){
                if(i == streamerID)
                    return true;
                ++m_streammerId;
                return false;
            });
            if(m_streamerIds.end() == streamer)
            {
                Log(" >>>>  Unknown streamer ID <<<<<");
                throw UnknownStreamerIdException();
            }
        });
    }
    CATCH_API_CALL(" >>>>  FAILED to load settings <<<<<")
}

void SovokTV::SetStreamerId(int streamerId)
{
    if (streamerId < 0 ||  streamerId >= m_streamerIds.size())
    {
        Log(" >>>>  Bad streamer ID <<<<<");
        return;
    }
    if(m_streammerId == streamerId )
        return;
    m_streammerId = streamerId;
    auto it = m_streamerIds.begin();
    while (streamerId--) it++;
    ParamList params;
    params["streamer"] = *it;
    try {
        ApiFunctionData apiParams("settings_set", params);

        CallApiFunction(apiParams, [&] (Document& jsonRoot){});
    }
    CATCH_API_CALL(" >>>>  FAILED to set streamer ID <<<<<")
}

void SovokTV::InitArchivesInfo()
{
    m_archivesInfo.clear();
    
    try {
        ApiFunctionData apiParams("archive_channels_list", ApiFunctionData::s_EmptyParams, ApiFunctionData::API_2_3);
        
        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &jsonList = jsonRoot["have_archive_list"];
            if(!jsonList.IsArray())
            throw  JsonParserException("'have_archive_list list' is not array");
            for(auto& i : jsonList.GetArray()) {
                m_archivesInfo[atoi(i["id"].GetString())] = atoi(i["archive_hours"].GetString());
            };
            return true;
        });
        m_addonHelper->Log(LOG_DEBUG,"Received %d channels with archive.", m_archivesInfo.size());
    }
    CATCH_API_CALL(" >>>>  FAILED to obtain archive channel list <<<<<")

}

void SovokTV::ForEach(std::function<void(const SovokArchiveEntry&)>& action) const
{
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    for(const auto& i : m_epgEntries) {
        if(i.second.HasArchive) {
            action(i.first);
        }
    }
}





