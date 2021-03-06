/*
 *  Copyright (C) 2020 Team Kodi
 *  https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Recordings.h"
#include "utilities/XMLUtils.h"

#include <kodi/General.h>
#include "pvrclient-nextpvr.h"

#include <regex>

#include <p8-platform/util/StringUtils.h>

using namespace NextPVR;
using namespace NextPVR::utilities;

/************************************************************/
/** Record handling **/

PVR_ERROR Recordings::GetRecordingsAmount(bool deleted, int& amount)
{
  // need something more optimal, but this will do for now...
  // Return -1 on error.

  if (m_iRecordingCount != 0)
  {
    amount = m_iRecordingCount;
    return PVR_ERROR_NO_ERROR;
  }

  std::string response;
  if (m_request.DoRequest("/service?method=recording.list&filter=ready", response) == HTTP_OK)
  {
    TiXmlDocument doc;
    if (doc.Parse(response.c_str()) != nullptr)
    {
      TiXmlElement* recordingsNode = doc.RootElement()->FirstChildElement("recordings");
      if (recordingsNode != nullptr)
      {
        TiXmlElement* pRecordingNode;
        m_iRecordingCount = 0;
        for (pRecordingNode = recordingsNode->FirstChildElement("recording"); pRecordingNode; pRecordingNode = pRecordingNode->NextSiblingElement())
        {
          m_iRecordingCount++;
        }
      }
    }
  }
  amount = m_iRecordingCount;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Recordings::GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results)
{
  // include already-completed recordings
  PVR_ERROR returnValue = PVR_ERROR_NO_ERROR;
  m_hostFilenames.clear();
  int recordingCount = 0;
  std::string response;
  if (m_request.DoRequest("/service?method=recording.list&filter=all", response) == HTTP_OK)
  {
    TiXmlDocument doc;
    if (doc.Parse(response.c_str()) != nullptr)
    {
      TiXmlElement* recordingsNode = doc.RootElement()->FirstChildElement("recordings");
      TiXmlElement* pRecordingNode;
      std::map<std::string, int> names;
      std::map<std::string, int> seasons;
      if ((m_settings.m_flattenRecording && m_settings.m_kodiLook) || m_settings.m_separateSeasons)
      {
        kodi::addon::PVRRecording mytag;
        int season;
        for (pRecordingNode = recordingsNode->FirstChildElement("recording"); pRecordingNode; pRecordingNode = pRecordingNode->NextSiblingElement())
        {
          std::string status;
          XMLUtils::GetString(pRecordingNode, "status", status);
          if (status != "Ready" && status != "Recording")
            continue;
          std::string title;
          XMLUtils::GetString(pRecordingNode, "name", title);
          if ((m_settings.m_flattenRecording && m_settings.m_kodiLook))
            names[title]++;

          if (ParseNextPVRSubtitle(pRecordingNode, mytag))
            season = mytag.GetSeriesNumber();
          else
            season = PVR_RECORDING_INVALID_SERIES_EPISODE;

          if (seasons[title])
          {
            if (seasons[title] != std::numeric_limits<int>::max())
            {
              if (season != seasons[title])
                seasons[title] = std::numeric_limits<int>::max();
            }
          }
          else
          {
            seasons[title] = season;
          }
        }
      }
      for (pRecordingNode = recordingsNode->FirstChildElement("recording"); pRecordingNode; pRecordingNode = pRecordingNode->NextSiblingElement())
      {
        kodi::addon::PVRRecording tag;
        std::string title;
        XMLUtils::GetString(pRecordingNode, "name", title);
        if (UpdatePvrRecording(pRecordingNode, tag, title, names[title] == 1, seasons[title] == std::numeric_limits<int>::max()))
        {
          recordingCount++;
          results.Add(tag);
        }
      }
    }
    m_iRecordingCount = recordingCount;
    kodi::Log(ADDON_LOG_DEBUG, "Updated recordings %lld", g_pvrclient->m_lastRecordingUpdateTime);
  }
  else
  {
    returnValue = PVR_ERROR_SERVER_ERROR;
  }
  g_pvrclient->m_lastRecordingUpdateTime = time(0);
  return returnValue;
}

bool Recordings::UpdatePvrRecording(TiXmlElement* pRecordingNode, kodi::addon::PVRRecording& tag, const std::string& title, bool flatten, bool multipleSeasons)
{
  std::string buffer;
  tag.SetTitle(title);

  XMLUtils::GetString(pRecordingNode, "start_time_ticks", buffer);
  tag.SetRecordingTime(stol(buffer));

  std::string status;
  XMLUtils::GetString(pRecordingNode, "status", status);
  if (status == "Pending" && tag.GetRecordingTime() > time(nullptr) + m_settings.m_serverTimeOffset)
  {
    // skip timers
    return false;
  }
  buffer.clear();
  XMLUtils::GetString(pRecordingNode, "duration_seconds", buffer);
  tag.SetDuration(stoi(buffer));

  if (status == "Ready" || status == "Pending" || status == "Recording")
  {
    if (!flatten)
    {
      buffer = StringUtils::Format("/%s", title.c_str());
      tag.SetDirectory(buffer);
    }
    buffer.clear();
    if (XMLUtils::GetString(pRecordingNode, "desc", buffer))
    {
      tag.SetPlot(buffer);
    }
  }
  else if (status == "Failed")
  {
    buffer = StringUtils::Format("/%s/%s", kodi::GetLocalizedString(30166).c_str(), title.c_str());
    tag.SetDirectory(buffer);
    if (XMLUtils::GetString(pRecordingNode, "reason", buffer))
    {
      tag.SetPlot(buffer);
    }
    if (tag.GetDuration() < 0)
    {
      tag.SetDuration(0);
    }
  }
  else if (status == "Conflict")
  {
    // shouldn't happen;
    return false;
  }
  else
  {
    kodi::Log(ADDON_LOG_ERROR, "Unknown status %s", status.c_str());
    return false;
  }

  // v4 users won't see recently concluded recordings on the EPG
  int endEpgTime = XMLUtils::GetIntValue(pRecordingNode, "epg_end_time_ticks");
  if (endEpgTime > time(nullptr) - 24 * 3600)
  {
    tag.SetEPGEventId(endEpgTime);
  }
  else if (status == "Recording" || status == "Pending")
  {
    // check for EPG based recording
    tag.SetEPGEventId(XMLUtils::GetIntValue(pRecordingNode, "epg_event_oid", PVR_TIMER_NO_EPG_UID));
    if (tag.GetEPGEventId() != PVR_TIMER_NO_EPG_UID)
    {
      tag.SetEPGEventId(tag.GetRecordingTime() + tag.GetDuration());
    }
  }

  buffer.clear();
  XMLUtils::GetString(pRecordingNode, "id", buffer);
  tag.SetRecordingId(buffer);

  tag.SetSeriesNumber(PVR_RECORDING_INVALID_SERIES_EPISODE);
  tag.SetEpisodeNumber(PVR_RECORDING_INVALID_SERIES_EPISODE);

  if (XMLUtils::GetString(pRecordingNode, "subtitle", buffer))
  {
    if (!m_settings.m_kodiLook)
    {
      tag.SetTitle(buffer);
    }
    if (ParseNextPVRSubtitle(pRecordingNode, tag))
    {
      if (m_settings.m_separateSeasons && multipleSeasons && tag.GetSeriesNumber() != PVR_RECORDING_INVALID_SERIES_EPISODE)
      {
        if (!m_settings.m_kodiLook)
        {
          tag.SetDirectory(StringUtils::Format("/%s/%s %d", title.c_str(), kodi::GetLocalizedString(20373).c_str(), tag.GetSeriesNumber()));
          tag.SetSeriesNumber(PVR_RECORDING_INVALID_SERIES_EPISODE);
          tag.SetEpisodeNumber(PVR_RECORDING_INVALID_SERIES_EPISODE);
        }
        else
        {
          tag.SetDirectory(StringUtils::Format("/%s/%s %d", tag.GetTitle().c_str(), kodi::GetLocalizedString(20373).c_str(), tag.GetSeriesNumber()));
        }
      }
    }
  }

  int lookup = 0;
  tag.SetLastPlayedPosition(XMLUtils::GetIntValue(pRecordingNode, "playback_position", lookup));
  if (XMLUtils::GetInt(pRecordingNode, "channel_id", lookup))
  {
    if (lookup == 0)
    {
      tag.SetChannelUid(PVR_CHANNEL_INVALID_UID);
    }
    else
    {
      tag.SetChannelUid(lookup);
      tag.SetIconPath(g_pvrclient->m_channels.GetChannelIconFileName(tag.GetChannelUid()));
    }
  }
  else
  {
    tag.SetChannelUid(PVR_CHANNEL_INVALID_UID);
  }

  buffer.clear();
  if (XMLUtils::GetString(pRecordingNode, "channel", buffer))
  {
    tag.SetChannelName(buffer);
  }
  tag.SetSizeInBytes(0);
  std::string recordingFile;
  if (XMLUtils::GetString(pRecordingNode, "file", recordingFile))
  {
    if (m_settings.m_showRecordingSize)
    {
      StringUtils::Replace(recordingFile, '\\', '/');
      if (StringUtils::StartsWith(recordingFile, "//"))
      {
        recordingFile = "smb:" + recordingFile;
      }
      if (kodi::vfs::FileExists(recordingFile, ADDON_READ_NO_CACHE))
      {
        kodi::vfs::CFile fileSize;
        if (fileSize.OpenFile(recordingFile, ADDON_READ_NO_CACHE))
        {
          tag.SetSizeInBytes(fileSize.GetLength());
          fileSize.Close();
        }
      }
      else
      {
        // don't play recording as file;
        recordingFile.clear();
      }
    }
  }

  m_hostFilenames[tag.GetRecordingId()] = recordingFile;

  // if we use unknown Kodi logs warning and turns it to TV so save some steps
  tag.SetChannelType(g_pvrclient->m_channels.GetChannelType(tag.GetChannelUid()));
  if (tag.GetChannelType() != PVR_RECORDING_CHANNEL_TYPE_RADIO)
  {
    std::string artworkPath;
    if (m_settings.m_backendVersion < 50000)
    {
      artworkPath = StringUtils::Format("http://%s:%d/service?method=recording.artwork&sid=%s&recording_id=%s", m_settings.m_hostname.c_str(), m_settings.m_port, g_pvrclient->m_sid, tag.GetRecordingId().c_str());
      tag.SetThumbnailPath(artworkPath);
      artworkPath = StringUtils::Format("http://%s:%d/service?method=recording.fanart&sid=%s&recording_id=%s", m_settings.m_hostname.c_str(), m_settings.m_port, g_pvrclient->m_sid, tag.GetRecordingId().c_str());
      tag.SetFanartPath(artworkPath);
    }
    else
    {
      if (m_settings.m_sendSidWithMetadata)
        artworkPath = StringUtils::Format("http://%s:%d/service?method=channel.show.artwork&sid=%s&name=%s", m_settings.m_hostname.c_str(), m_settings.m_port, g_pvrclient->m_sid, UriEncode(title).c_str());
      else
        artworkPath = StringUtils::Format("http://%s:%d/service?method=channel.show.artwork&name=%s", m_settings.m_hostname.c_str(), m_settings.m_port, UriEncode(title).c_str());
      tag.SetFanartPath(artworkPath);
      artworkPath += "&prefer=poster";
      tag.SetThumbnailPath(artworkPath);
    }
  }
  if (XMLUtils::GetAdditiveString(pRecordingNode->FirstChildElement("genres"), "genre", EPG_STRING_TOKEN_SEPARATOR, buffer, true))
  {
    tag.SetGenreType(EPG_GENRE_USE_STRING);
    tag.SetGenreSubType(0);
    tag.SetGenreDescription(buffer);
  }

  std::string significance;
  XMLUtils::GetString(pRecordingNode, "significance", significance);
  if (significance.find("Premiere") != std::string::npos)
  {
    tag.SetFlags(PVR_RECORDING_FLAG_IS_PREMIERE);
  }
  else if (significance.find("Finale") != std::string::npos)
  {
    tag.SetFlags(PVR_RECORDING_FLAG_IS_FINALE);
  }

  bool played = false;
  if (XMLUtils::GetBoolean(pRecordingNode, "played", played))
  {
    tag.SetPlayCount(1);
  }

  return true;
}

bool Recordings::ParseNextPVRSubtitle(TiXmlElement *pRecordingNode, kodi::addon::PVRRecording& tag)
{
  std::string buffer;
  bool hasSeasonEpisode = false;
  if (XMLUtils::GetString(pRecordingNode, "subtitle", buffer))
  {
    std::regex base_regex("S(\\d\\d)E(\\d+) - ?(.+)?");
    std::smatch base_match;
    // note NextPVR does not support S0 for specials
    if (std::regex_match(buffer, base_match, base_regex))
    {
      if (base_match.size() == 3 || base_match.size() == 4)
      {

        std::ssub_match base_sub_match = base_match[1];
        tag.SetSeriesNumber(std::stoi(base_sub_match.str()));
        base_sub_match = base_match[2];
        tag.SetEpisodeNumber(std::stoi(base_sub_match.str()));
        if (m_settings.m_kodiLook)
        {
          if (base_match.size() == 4)
          {
            base_sub_match = base_match[3];
            tag.SetEpisodeName(base_sub_match.str());
          }
        }
        hasSeasonEpisode = true;
      }
    }
    else if (m_settings.m_kodiLook)
    {
      tag.SetEpisodeName(buffer);
    }
  }

  if (!hasSeasonEpisode)
  {
    std::string recordingFile;
    if (XMLUtils::GetString(pRecordingNode, "file", recordingFile))
    {
      std::regex base_regex("S(\\d\\d)E(\\d+)");
      std::smatch base_match;
      if (std::regex_search(recordingFile, base_match, base_regex))
      {
        if (base_match.size() == 3)
        {
          std::ssub_match base_sub_match = base_match[1];
          tag.SetSeriesNumber(std::stoi(base_sub_match.str()));
          base_sub_match = base_match[2];
          tag.SetEpisodeNumber(std::stoi(base_sub_match.str()));
          if (!m_settings.m_kodiLook)
          {
            tag.SetTitle(StringUtils::Format("S%2.2dE%2.2d - %s", tag.GetSeriesNumber(), tag.GetEpisodeNumber(), buffer.c_str()));
          }
          hasSeasonEpisode = true;
        }
      }
    }
  }
  return hasSeasonEpisode;
}

PVR_ERROR Recordings::DeleteRecording(const kodi::addon::PVRRecording& recording)
{
  if (recording.GetRecordingTime() < time(nullptr) && recording.GetRecordingTime() + recording.GetDuration() > time(nullptr))
    return PVR_ERROR_RECORDING_RUNNING;

  const std::string request = "/service?method=recording.delete&recording_id=" + recording.GetRecordingId();

  std::string response;
  if (m_request.DoRequest(request.c_str(), response) == HTTP_OK)
  {
    if (strstr(response.c_str(), "<rsp stat=\"ok\">"))
    {
      return PVR_ERROR_NO_ERROR;
    }
  }
  else
  {
    kodi::Log(ADDON_LOG_DEBUG, "DeleteRecording failed");
  }
  return PVR_ERROR_FAILED;
}

bool Recordings::ForgetRecording(const kodi::addon::PVRRecording& recording)
{
  // tell backend to forget recording history so it can re recorded.
  std::string request = "/service?method=recording.forget&recording_id=";
  request.append(recording.GetRecordingId());
  std::string response;
  return m_request.DoRequest(request.c_str(), response) == HTTP_OK;
}

PVR_ERROR Recordings::SetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int lastplayedposition)
{
  const std::string request = StringUtils::Format("/service?method=recording.watched.set&recording_id=%s&position=%d", recording.GetRecordingId().c_str(), lastplayedposition);

  std::string response;
  if (m_request.DoRequest(request.c_str(), response) == HTTP_OK)
  {
    if (strstr(response.c_str(), "<rsp stat=\"ok\">") == nullptr)
    {
      kodi::Log(ADDON_LOG_DEBUG, "SetRecordingLastPlayedPosition failed");
      return PVR_ERROR_FAILED;
    }
    g_pvrclient->m_lastRecordingUpdateTime = 0;
  }
  return PVR_ERROR_NO_ERROR;
}


PVR_ERROR Recordings::GetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int& position)
{
  position = recording.GetLastPlayedPosition();
  return PVR_ERROR_NO_ERROR;
}


PVR_ERROR Recordings::GetRecordingEdl(const kodi::addon::PVRRecording& recording, std::vector<kodi::addon::PVREDLEntry>& edl)
{
  const std::string request = "/service?method=recording.edl&recording_id=" + recording.GetRecordingId();
  std::string response;
  if (m_request.DoRequest(request.c_str(), response) == HTTP_OK)
  {
    if (strstr(response.c_str(), "<rsp stat=\"ok\">") != nullptr)
    {
      TiXmlDocument doc;
      if (doc.Parse(response.c_str()) != nullptr)
      {
        int index = 0;
        TiXmlElement* commercialsNode = doc.RootElement()->FirstChildElement("commercials");
        TiXmlElement* pCommercialNode;
        for (pCommercialNode = commercialsNode->FirstChildElement("commercial"); pCommercialNode; pCommercialNode = pCommercialNode->NextSiblingElement())
        {
          kodi::addon::PVREDLEntry entry;
          std::string buffer;
          XMLUtils::GetString(pCommercialNode, "start", buffer);
          entry.SetStart(std::stoll(buffer) * 1000);
          buffer.clear();
          XMLUtils::GetString(pCommercialNode, "end", buffer);
          entry.SetEnd(std::stoll(buffer) * 1000);
          entry.SetType(PVR_EDL_TYPE_COMBREAK);
          edl.emplace_back(entry);
        }
        return PVR_ERROR_NO_ERROR;
      }
    }
  }
  return PVR_ERROR_FAILED;
}
