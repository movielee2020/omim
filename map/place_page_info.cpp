#include "place_page_info.hpp"

#include "indexer/osm_editor.hpp"

namespace place_page
{
char const * Info::kSubtitleSeparator = " • ";
char const * Info::kStarSymbol = "★";
char const * Info::kMountainSymbol = "▲";

bool Info::IsFeature() const { return m_featureID.IsValid(); }
bool Info::IsBookmark() const { return m_bac != MakeEmptyBookmarkAndCategory(); }
bool Info::IsMyPosition() const { return m_isMyPosition; }
bool Info::HasApiUrl() const { return !m_apiUrl.empty(); }
bool Info::IsEditable() const { return m_isEditable; }
bool Info::HasWifi() const { return GetInternet() == osm::Internet::Wlan; }

string Info::FormatNewBookmarkName() const
{
  string const title = GetTitle();
  if (title.empty())
    return GetLocalizedType();
  return title;
}

string Info::GetTitle() const
{
  string const defaultName = GetDefaultName();
  if (m_customName.empty())
    return defaultName;
  if (defaultName.empty())
    return m_customName;
  if (m_customName == defaultName)
    return m_customName;
  return m_customName + "(" + defaultName + ")";
}

string Info::GetSubtitle() const
{
  vector<string> values;

  // Type.
  values.push_back(GetLocalizedType());

  // Cuisines.
  for (string const & cuisine : GetCuisines())
    values.push_back(cuisine);

  // Stars.
  string const stars = FormatStars();
  if (!stars.empty())
    values.push_back(stars);

  // Operator.
  string const op = GetOperator();
  if (!op.empty())
    values.push_back(op);

  // Elevation.
  string const eleStr = GetElevation();
  if (!eleStr.empty())
    values.push_back(kMountainSymbol + eleStr);
  if (HasWifi())
    values.push_back(m_localizedWifiString);

  return strings::JoinStrings(values, kSubtitleSeparator);
}

string Info::FormatStars() const
{
  string stars;
  for (int i = 0; i < GetStars(); ++i)
    stars.append(kStarSymbol);
  return stars;
}

string Info::GetCustomName() const { return m_customName; }
BookmarkAndCategory Info::GetBookmarkAndCategory() const { return m_bac; }
string const & Info::GetApiUrl() const { return m_apiUrl; }
void Info::SetMercator(m2::PointD const & mercator) { m_mercator = mercator; }
}  // namespace place_page