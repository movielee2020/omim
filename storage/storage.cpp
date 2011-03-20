#include "storage.hpp"

#include "../base/logging.hpp"
#include "../base/string_utils.hpp"

#include "../coding/file_writer.hpp"
#include "../coding/file_reader.hpp"
#include "../coding/strutil.hpp"

#include "../version/version.hpp"

#include "../std/set.hpp"
#include "../std/algorithm.hpp"

#include <boost/bind.hpp>

#include "../base/start_mem_debug.hpp"

namespace storage
{
  static string ErrorString(DownloadResult res)
  {
    switch (res)
    {
    case EHttpDownloadCantCreateFile:
      return "File can't be created. Probably, you have no disk space available or "
                         "using read-only file system.";
    case EHttpDownloadFailed:
      return "Download failed due to missing or poor connection. "
                         "Please, try again later.";
    case EHttpDownloadFileIsLocked:
      return "Download can't be finished because file is locked. "
                         "Please, try again after restarting application.";
    case EHttpDownloadFileNotFound:
      return "Requested file is absent on the server.";
    case EHttpDownloadNoConnectionAvailable:
      return "No network connection is available.";
    case EHttpDownloadOk:
      return "Download finished successfully.";
    }
    return "Unknown error";
  }

  ////////////////////////////////////////////////////////////////////////////
  void Storage::Init(TAddMapFunction addFunc, TRemoveMapFunction removeFunc)
  {
    m_currentVersion = static_cast<uint32_t>(Version::BUILD);

    m_addMap = addFunc;
    m_removeMap = removeFunc;

    // activate all downloaded maps
    Platform & p = GetPlatform();
    Platform::FilesList filesList;
    string const dataPath = p.WritableDir();
    p.GetFilesInDir(dataPath, "*" DATA_FILE_EXTENSION, filesList);
    for (Platform::FilesList::iterator it = filesList.begin(); it != filesList.end(); ++it)
    { // simple way to avoid continuous crashes with invalid data files
      try {
        m_addMap(dataPath + *it);
      } catch (std::exception const & e)
      {
        FileWriter::DeleteFileX(dataPath + *it);
        LOG(LWARNING, (e.what(), "while adding file", *it, "so this file is deleted"));
      }
    }
  }

  string Storage::UpdateBaseUrl() const
  {
    return UPDATE_BASE_URL + utils::to_string(m_currentVersion) + "/";
  }

  TCountriesContainer const & NodeFromIndex(TCountriesContainer const & root, TIndex const & index)
  {
    // complex logic to avoid [] out_of_bounds exceptions
    if (index.m_group == TIndex::INVALID || index.m_group >= static_cast<int>(root.SiblingsCount()))
      return root;
    else
    {
      if (index.m_country == TIndex::INVALID || index.m_country >= static_cast<int>(root[index.m_group].SiblingsCount()))
        return root[index.m_group];
      if (index.m_region == TIndex::INVALID || index.m_region >= static_cast<int>(root[index.m_group][index.m_country].SiblingsCount()))
        return root[index.m_group][index.m_country];
      return root[index.m_group][index.m_country][index.m_region];
    }
  }

  Country const & Storage::CountryByIndex(TIndex const & index) const
  {
    return NodeFromIndex(m_countries, index).Value();
  }

  size_t Storage::CountriesCount(TIndex const & index) const
  {
    return NodeFromIndex(m_countries, index).SiblingsCount();
  }

  string Storage::CountryName(TIndex const & index) const
  {
    return NodeFromIndex(m_countries, index).Value().Name();
  }

  TLocalAndRemoteSize Storage::CountrySizeInBytes(TIndex const & index) const
  {
    return CountryByIndex(index).Size();
  }

  TStatus Storage::CountryStatus(TIndex const & index) const
  {
    // first, check if we already downloading this country or have in in the queue
    TQueue::const_iterator found = std::find(m_queue.begin(), m_queue.end(), index);
    if (found != m_queue.end())
    {
      if (found == m_queue.begin())
        return EDownloading;
      else
        return EInQueue;
    }

    // second, check if this country has failed while downloading
    if (m_failedCountries.find(index) != m_failedCountries.end())
      return EDownloadFailed;

    TLocalAndRemoteSize size = CountryByIndex(index).Size();
    if (size.first == size.second)
    {
      if (size.second == 0)
        return EUnknown;
      else
        return EOnDisk;
    }

    return ENotDownloaded;
  }

  void Storage::DownloadCountry(TIndex const & index)
  {
    // check if we already downloading this country
    TQueue::const_iterator found = find(m_queue.begin(), m_queue.end(), index);
    if (found != m_queue.end())
    { // do nothing
      return;
    }
    // remove it from failed list
    m_failedCountries.erase(index);
    // add it into the queue
    m_queue.push_back(index);
    // and start download if necessary
    if (m_queue.size() == 1)
    {
      // reset total country's download progress
      TLocalAndRemoteSize size = CountryByIndex(index).Size();
      m_countryProgress = TDownloadProgress(0, size.second);

      DownloadNextCountryFromQueue();
    }
    else
    { // notify about "In Queue" status
      if (m_observerChange)
        m_observerChange(index);
    }
  }

  template <class TRemoveFn>
  class DeactivateMap
  {
    string m_workingDir;
    TRemoveFn & m_removeFn;
  public:
    DeactivateMap(TRemoveFn & removeFn) : m_removeFn(removeFn)
    {
      m_workingDir = GetPlatform().WritableDir();
    }
    void operator()(TTile const & tile)
    {
      string const file = m_workingDir + tile.first;
      m_removeFn(file);
    }
  };

  void Storage::DownloadNextCountryFromQueue()
  {
    while (!m_queue.empty())
    {
      TIndex index = m_queue.front();
      TTilesContainer const & tiles = CountryByIndex(index).Tiles();
      for (TTilesContainer::const_iterator it = tiles.begin(); it != tiles.end(); ++it)
      {
        if (!IsTileDownloaded(*it))
        {
          GetDownloadManager().DownloadFile(
              (UpdateBaseUrl() + UrlEncode(it->first)).c_str(),
              (GetPlatform().WritablePathForFile(it->first).c_str()),
              bind(&Storage::OnMapDownloadFinished, this, _1, _2),
              bind(&Storage::OnMapDownloadProgress, this, _1, _2),
              true);  // enabled resume support by default
          // notify GUI - new status for country, "Downloading"
          if (m_observerChange)
            m_observerChange(index);
          return;
        }
      }
      // continue with next country
      m_queue.pop_front();
      // reset total country's download progress
      if (!m_queue.empty())
        m_countryProgress = TDownloadProgress(0, CountryByIndex(m_queue.front()).Size().second);
      // and notify GUI - new status for country, "OnDisk"
      if (m_observerChange)
        m_observerChange(index);
    }
  }

  struct CancelDownloading
  {
    string const m_baseUrl;
    CancelDownloading(string const & baseUrl) : m_baseUrl(baseUrl) {}
    void operator()(TTile const & tile)
    {
      GetDownloadManager().CancelDownload((m_baseUrl + UrlEncode(tile.first)).c_str());
    }
  };

  class DeleteMap
  {
    string m_workingDir;
  public:
    DeleteMap()
    {
		  m_workingDir = GetPlatform().WritableDir();
    }
    /// @TODO do not delete other countries cells
    void operator()(TTile const & tile)
    {
      FileWriter::DeleteFileX(m_workingDir + tile.first);
    }
  };

  template <typename TRemoveFunc>
  void DeactivateAndDeleteCountry(Country const & country, TRemoveFunc removeFunc)
  {
    // deactivate from multiindex
    for_each(country.Tiles().begin(), country.Tiles().end(), DeactivateMap<TRemoveFunc>(removeFunc));
    // delete from disk
    for_each(country.Tiles().begin(), country.Tiles().end(), DeleteMap());
  }

  void Storage::DeleteCountry(TIndex const & index)
  {
    Country const & country = CountryByIndex(index);

    // check if we already downloading this country
    TQueue::iterator found = find(m_queue.begin(), m_queue.end(), index);
    if (found != m_queue.end())
    {
      if (found == m_queue.begin())
      { // stop download
        for_each(country.Tiles().begin(), country.Tiles().end(), CancelDownloading(UpdateBaseUrl()));
        // remove from the queue
        m_queue.erase(found);
        // start another download if the queue is not empty
        DownloadNextCountryFromQueue();
      }
      else
      { // remove from the queue
        m_queue.erase(found);
      }
    }

    // @TODO: Do not delete pieces which are used by other countries
    DeactivateAndDeleteCountry(country, m_removeMap);
    if (m_observerChange)
      m_observerChange(index);
  }

  void Storage::ReInitCountries(bool forceReload)
  {
    if (forceReload)
      m_countries.Clear();

    if (m_countries.SiblingsCount() == 0)
    {
      TTilesContainer tiles;
      if (LoadTiles(tiles, GetPlatform().ReadPathForFile(DATA_UPDATE_FILE), m_currentVersion))
      {
        if (!LoadCountries(GetPlatform().ReadPathForFile(COUNTRIES_FILE), tiles, m_countries))
        {
          LOG(LWARNING, ("Can't load countries file", COUNTRIES_FILE));
        }
      }
      else
      {
        LOG(LWARNING, ("Can't load update file", DATA_UPDATE_FILE));
      }
    }
  }

  void Storage::Subscribe(TObserverChangeCountryFunction change, TObserverProgressFunction progress,
                          TUpdateRequestFunction updateRequest)
  {
    m_observerChange = change;
    m_observerProgress = progress;
    m_observerUpdateRequest = updateRequest;

    ReInitCountries(false);
  }

  void Storage::Unsubscribe()
  {
    m_observerChange.clear();
    m_observerProgress.clear();
    m_observerUpdateRequest.clear();
  }

  string FileFromUrl(string const & url)
  {
    return UrlDecode(url.substr(url.find_last_of('/') + 1, string::npos));
  }

  void Storage::OnMapDownloadFinished(char const * url, DownloadResult result)
  {
    if (m_queue.empty())
    {
      ASSERT(false, ("Invalid url?", url));
      return;
    }

    if (result != EHttpDownloadOk)
    {
      // remove failed country from the queue
      TIndex failedIndex = m_queue.front();
      m_queue.pop_front();
      m_failedCountries.insert(failedIndex);
      // notify GUI about failed country
      if (m_observerChange)
        m_observerChange(failedIndex);
    }
    else
    {
      TLocalAndRemoteSize size = CountryByIndex(m_queue.front()).Size();
      if (size.second != 0)
        m_countryProgress.first = size.first;
      // activate downloaded map piece
      string const datFile = GetPlatform().ReadPathForFile(FileFromUrl(url));
      m_addMap(datFile);
    }
    DownloadNextCountryFromQueue();
  }

  void Storage::OnMapDownloadProgress(char const * /*url*/, TDownloadProgress progress)
  {
    if (m_queue.empty())
    {
      ASSERT(false, ("queue can't be empty"));
      return;
    }

    if (m_observerProgress)
      m_observerProgress(m_queue.front(),
          TDownloadProgress(m_countryProgress.first + progress.first, m_countryProgress.second));
  }

  void Storage::CheckForUpdate()
  {
    // at this moment we support only binary update checks
    string const update = UpdateBaseUrl() + BINARY_UPDATE_FILE/*DATA_UPDATE_FILE*/;
    GetDownloadManager().CancelDownload(update.c_str());
    GetDownloadManager().DownloadFile(
        update.c_str(),
        (GetPlatform().WritablePathForFile(DATA_UPDATE_FILE)).c_str(),
        bind(&Storage::OnBinaryUpdateCheckFinished, this, _1, _2),
        TDownloadProgressFunction(), false);
  }

  void Storage::OnDataUpdateCheckFinished(char const * url, DownloadResult result)
  {
    if (result != EHttpDownloadOk)
    {
      LOG(LWARNING, ("Update check failed for url:", url));
      if (m_observerUpdateRequest)
        m_observerUpdateRequest(EDataCheckFailed, ErrorString(result));
    }
    else
    { // @TODO parse update file and notify GUI
    }

    // parse update file
//    TCountriesContainer tempCountries;
//    if (!LoadCountries(tempCountries, GetPlatform().WritablePathForFile(DATA_UPDATE_FILE)))
//    {
//      LOG(LWARNING, ("New application version should be downloaded, "
//                     "update file format can't be parsed"));
//      // @TODO: report to GUI
//      return;
//    }
//    // stop any active download, clear the queue, replace countries and notify GUI
//    if (!m_queue.empty())
//    {
//      CancelCountryDownload(CountryByIndex(m_queue.front()));
//      m_queue.clear();
//    }
//    m_countries.swap(tempCountries);
//    // @TODO report to GUI about reloading all countries
//    LOG(LINFO, ("Update check complete"));
  }

  void Storage::OnBinaryUpdateCheckFinished(char const * url, DownloadResult result)
  {
    if (result == EHttpDownloadFileNotFound)
    { // no binary update is available
      if (m_observerUpdateRequest)
        m_observerUpdateRequest(ENoAnyUpdateAvailable, "No update is available");
    }
    else if (result == EHttpDownloadOk)
    { // update is available!
      try
      {
        if (m_observerUpdateRequest)
        {
          string const updateTextFilePath = GetPlatform().ReadPathForFile(FileFromUrl(url));
          FileReader file(updateTextFilePath);
          m_observerUpdateRequest(ENewBinaryAvailable, file.ReadAsText());
        }
      }
      catch (std::exception const & e)
      {
        if (m_observerUpdateRequest)
          m_observerUpdateRequest(EBinaryCheckFailed,
                                    string("Error loading b-update text file ") + e.what());
      }
    }
    else
    { // connection error
      if (m_observerUpdateRequest)
        m_observerUpdateRequest(EBinaryCheckFailed, ErrorString(result));
    }
  }
}
