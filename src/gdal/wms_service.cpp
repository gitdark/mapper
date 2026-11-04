/*
 *    Copyright 2026 Darko Surjan
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "wms_service.h"

#include <algorithm>
#include <iterator>
#include <cmath>
#include <limits>
#include <memory>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_minixml.h>
#include <cpl_string.h>
#include <cpl_vsi.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include <QtGlobal>
#include <QCoreApplication>
#include <QLatin1String>
#include <QLineF>
#include <QPointF>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map_coord.h"
#include "gdal/gdal_manager.h"


namespace OpenOrienteering {

namespace {

QString tr(const char* text)
{
	return QCoreApplication::translate("OpenOrienteering::WmsService", text);
}

QString lastGdalError()
{
	return QString::fromUtf8(CPLGetLastErrorMsg());
}

const QLatin1String wms_prefix("WMS:");

struct DatasetCloser { void operator()(GDALDatasetH h) const { GDALClose(h); } };
using DatasetPtr = std::unique_ptr<std::remove_pointer<GDALDatasetH>::type, DatasetCloser>;

struct SrsDestroyer { void operator()(OGRSpatialReferenceH h) const { OSRDestroySpatialReference(h); } };
using SrsPtr = std::unique_ptr<std::remove_pointer<OGRSpatialReferenceH>::type, SrsDestroyer>;

struct TransformDestroyer { void operator()(OGRCoordinateTransformationH h) const { OCTDestroyCoordinateTransformation(h); } };
using TransformPtr = std::unique_ptr<std::remove_pointer<OGRCoordinateTransformationH>::type, TransformDestroyer>;

struct XmlDestroyer { void operator()(CPLXMLNode* node) const { CPLDestroyXMLNode(node); } };
using XmlPtr = std::unique_ptr<CPLXMLNode, XmlDestroyer>;


QString stripWmsPrefix(const QString& url)
{
	auto result = url.trimmed();
	if (result.startsWith(wms_prefix, Qt::CaseInsensitive))
		result.remove(0, wms_prefix.size());
	return result;
}

QString queryValue(const QUrlQuery& query, const QString& key)
{
	for (auto const& item : query.queryItems(QUrl::FullyDecoded))
	{
		if (item.first.compare(key, Qt::CaseInsensitive) == 0)
			return item.second;
	}
	return {};
}

void removeQueryItem(QUrlQuery& query, const QString& key)
{
	auto items = query.queryItems(QUrl::FullyDecoded);
	items.erase(std::remove_if(items.begin(), items.end(), [&key](auto const& item) {
		return item.first.compare(key, Qt::CaseInsensitive) == 0;
	}), items.end());
	query.setQueryItems(items);
}

/**
 * Returns true if the host is not expected to be reachable from the internet.
 */
bool isInternalHost(const QString& host)
{
	if (host.isEmpty())
		return false;
	if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
		return true;
	
	// IPv4 literals
	auto const parts = host.split(QLatin1Char('.'));
	if (parts.size() == 4)
	{
		int octets[4];
		bool is_ipv4 = true;
		for (int i = 0; i < 4 && is_ipv4; ++i)
		{
			octets[i] = parts[i].toInt(&is_ipv4);
			is_ipv4 = is_ipv4 && octets[i] >= 0 && octets[i] <= 255;
		}
		if (is_ipv4)
		{
			return octets[0] == 10                                         // 10.0.0.0/8
			       || octets[0] == 127                                     // loopback
			       || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31)  // 172.16.0.0/12
			       || (octets[0] == 192 && octets[1] == 168)               // 192.168.0.0/16
			       || (octets[0] == 169 && octets[1] == 254);              // link-local
		}
	}
	
	// IPv6 literals
	if (host.contains(QLatin1Char(':')))
	{
		auto const lower = host.toLower();
		return lower == QLatin1String("::1")
		       || lower.startsWith(QLatin1String("fc"))      // unique local fc00::/7
		       || lower.startsWith(QLatin1String("fd"))
		       || lower.startsWith(QLatin1String("fe80:"));  // link-local
	}
	
	// Host names without a domain, e.g. "geoserver"
	return !host.contains(QLatin1Char('.'));
}

/**
 * Returns true if the query parameter is defined by the WMS protocol
 * for GetCapabilities or GetMap requests.
 */
bool isWmsParameter(const QString& key)
{
	static const char* const wms_parameters[] = {
	    "SERVICE", "REQUEST", "VERSION", "LAYERS", "STYLES", "SRS", "CRS", "BBOX",
	    "WIDTH", "HEIGHT", "FORMAT", "TRANSPARENT", "BGCOLOR", "EXCEPTIONS",
	};
	return std::any_of(std::begin(wms_parameters), std::end(wms_parameters), [&key](auto const* name) {
		return key.compare(QLatin1String(name), Qt::CaseInsensitive) == 0;
	});
}

QByteArray toXmlNumber(double value)
{
	return QByteArray::number(value, 'f', 10);
}

bool isCredentialParameter(const QString& key)
{
	static const char* const names[] = {
	    "token", "access_token", "accesstoken", "auth", "authkey", "auth_key",
	    "apikey", "api_key", "api-key", "key", "password", "passwd", "pwd",
	    "secret", "client_secret", "signature", "sig",
	};
	return std::any_of(std::begin(names), std::end(names), [&key](const char* name) {
		return key.compare(QLatin1String(name), Qt::CaseInsensitive) == 0;
	});
}

}  // namespace


QStringList wmsCredentialParameters(const QString& url)
{
	QStringList result;
	auto const qurl = QUrl(stripWmsPrefix(url));
	if (!qurl.password().isEmpty())
		result.push_back(QStringLiteral("password"));
	auto const items = QUrlQuery(qurl).queryItems();
	for (auto const& item : items)
	{
		if (isCredentialParameter(item.first) && !item.second.isEmpty()
		    && !result.contains(item.first, Qt::CaseInsensitive))
			result.push_back(item.first);
	}
	return result;
}



QString wmsCapabilitiesUrl(const QString& server_url)
{
	auto url = QUrl(stripWmsPrefix(server_url));
	auto query = QUrlQuery(url);
	removeQueryItem(query, QStringLiteral("SERVICE"));
	removeQueryItem(query, QStringLiteral("REQUEST"));
	query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WMS"));
	query.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("GetCapabilities"));
	url.setQuery(query);
	return wms_prefix + url.toString(QUrl::FullyEncoded);
}


QString normalizedWmsUrl(const QString& url)
{
	auto result = stripWmsPrefix(url);
	while (result.endsWith(QLatin1Char('?')) || result.endsWith(QLatin1Char('&')))
		result.chop(1);
	return result;
}


QStringList updateWmsUrlHistory(const QStringList& history, const QString& url, int max_size)
{
	QStringList result;
	auto const add = [&result](const QString& entry) {
		auto const normalized = normalizedWmsUrl(entry);
		if (!normalized.isEmpty() && !result.contains(normalized))
			result.push_back(normalized);
	};
	add(url);
	for (auto const& entry : history)
		add(entry);
	while (result.size() > max_size)
		result.removeLast();
	return result;
}


QString wmsGetMapDatasetName(const QString& advertised_dataset_name, const QString& server_url)
{
	auto const user_url = QUrl(stripWmsPrefix(server_url));
	auto url = QUrl(stripWmsPrefix(advertised_dataset_name));
	if (!url.isValid() || !user_url.isValid())
		return advertised_dataset_name;
	
	if (isInternalHost(url.host()) && !isInternalHost(user_url.host()))
	{
		url.setScheme(user_url.scheme());
		url.setUserInfo(user_url.userInfo());
		url.setHost(user_url.host());
		url.setPort(user_url.port());
		url.setPath(user_url.path());
	}
	
	auto query = QUrlQuery(url);
	auto const user_items = QUrlQuery(user_url).queryItems(QUrl::FullyDecoded);
	for (auto const& item : user_items)
	{
		if (!isWmsParameter(item.first) && queryValue(query, item.first).isEmpty())
			query.addQueryItem(item.first, item.second);
	}
	url.setQuery(query);
	
	return wms_prefix + url.toString(QUrl::FullyEncoded);
}


std::vector<WmsLayer> queryWmsLayers(const QString& server_url, QString& error)
{
	std::vector<WmsLayer> layers;
	error.clear();

	auto const trimmed_url = stripWmsPrefix(server_url);
	auto const url = QUrl(trimmed_url);
	if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty())
	{
		error = tr("Invalid URL: %1").arg(trimmed_url);
		return layers;
	}

	GdalManager();
	CPLErrorReset();

	auto const query = QUrlQuery(url);
	if (queryValue(query, QStringLiteral("REQUEST")).compare(QLatin1String("GetMap"), Qt::CaseInsensitive) == 0)
	{
		// A single layer, given by a GetMap request
		auto dataset_name = QString(wms_prefix + trimmed_url);
		auto dataset = DatasetPtr(GDALOpen(dataset_name.toUtf8(), GA_ReadOnly));
		if (!dataset)
		{
			error = lastGdalError();
			return layers;
		}
		auto title = queryValue(query, QStringLiteral("LAYERS"));
		if (title.isEmpty())
			title = trimmed_url;
		layers.push_back({title, dataset_name});
		return layers;
	}

	auto dataset = DatasetPtr(GDALOpen(wmsCapabilitiesUrl(trimmed_url).toUtf8(), GA_ReadOnly));
	if (!dataset)
	{
		error = lastGdalError();
		if (error.isEmpty())
			error = tr("The server did not return a valid WMS capabilities document.");
		return layers;
	}

	auto** subdatasets = GDALGetMetadata(dataset.get(), "SUBDATASETS");
	for (int i = 1; ; ++i)
	{
		auto name = CSLFetchNameValue(subdatasets, QByteArray("SUBDATASET_" + QByteArray::number(i) + "_NAME").constData());
		if (!name)
			break;
		auto desc = CSLFetchNameValue(subdatasets, QByteArray("SUBDATASET_" + QByteArray::number(i) + "_DESC").constData());
		layers.push_back({ QString::fromUtf8(desc ? desc : name),
		                   wmsGetMapDatasetName(QString::fromUtf8(name), trimmed_url) });
	}

	if (layers.empty())
		error = tr("The server does not offer any layers.");

	return layers;
}


WmsDescription makeWmsDescription(const QString& dataset_name,
                                  const Georeferencing& georef,
                                  const QRectF& map_extent,
                                  double resolution,
                                  bool use_cache,
                                  bool transparent)
{
	WmsDescription result;

	if (georef.getState() != Georeferencing::Geospatial)
	{
		result.error = tr("The map is not georeferenced.");
		return result;
	}
	if (!map_extent.isValid() || map_extent.isEmpty())
	{
		result.error = tr("The area is empty.");
		return result;
	}
	if (!(resolution > 0))
	{
		result.error = tr("Invalid resolution.");
		return result;
	}

	GdalManager();
	CPLErrorReset();

	auto dataset = DatasetPtr(GDALOpen(dataset_name.toUtf8(), GA_ReadOnly));
	if (!dataset)
	{
		result.error = lastGdalError();
		return result;
	}

	// Transformations between geographic coordinates and the WMS SRS
	auto const* wkt = GDALGetProjectionRef(dataset.get());
	if (!wkt || !*wkt)
	{
		result.error = tr("The layer has no spatial reference system.");
		return result;
	}
	auto target_srs = SrsPtr(OSRNewSpatialReference(wkt));
	auto geographic_srs = SrsPtr(OSRNewSpatialReference(nullptr));
	OSRSetWellKnownGeogCS(geographic_srs.get(), "WGS84");
#if GDAL_VERSION_MAJOR >= 3
	OSRSetAxisMappingStrategy(target_srs.get(), OAMS_TRADITIONAL_GIS_ORDER);
	OSRSetAxisMappingStrategy(geographic_srs.get(), OAMS_TRADITIONAL_GIS_ORDER);
#endif
	auto to_target = TransformPtr(OCTNewCoordinateTransformation(geographic_srs.get(), target_srs.get()));
	auto to_geographic = TransformPtr(OCTNewCoordinateTransformation(target_srs.get(), geographic_srs.get()));
	if (!to_target || !to_geographic)
	{
		result.error = lastGdalError();
		return result;
	}

	{
#if GDAL_VERSION_MAJOR >= 3
		auto const* name = OSRGetName(target_srs.get());
#else
		auto const* name = OSRIsProjected(target_srs.get())
		                   ? OSRGetAttrValue(target_srs.get(), "PROJCS", 0)
		                   : OSRGetAttrValue(target_srs.get(), "GEOGCS", 0);
#endif
		auto const* auth_name = OSRGetAuthorityName(target_srs.get(), nullptr);
		auto const* auth_code = OSRGetAuthorityCode(target_srs.get(), nullptr);
		if (auth_name && auth_code)
			result.srs = QString::fromUtf8(auth_name) + QLatin1Char(':') + QString::fromUtf8(auth_code);
		else if (name)
			result.srs = QString::fromUtf8(name);
	}

	// Bounding box of the map extent in the WMS SRS.
	// Sample the edges because the shape is not preserved by the projection.
	constexpr int samples = 8;
	auto min_x = std::numeric_limits<double>::max();
	auto min_y = min_x;
	auto max_x = std::numeric_limits<double>::lowest();
	auto max_y = max_x;
	auto const add_point = [&](QPointF map_pos) -> bool {
		bool ok = false;
		auto const latlon = georef.toGeographicCoords(MapCoordF(map_pos), &ok);
		if (!ok)
			return false;
		double x = latlon.longitude();
		double y = latlon.latitude();
		if (!OCTTransform(to_target.get(), 1, &x, &y, nullptr))
			return false;
		min_x = std::min(min_x, x);
		max_x = std::max(max_x, x);
		min_y = std::min(min_y, y);
		max_y = std::max(max_y, y);
		return true;
	};
	for (int i = 0; i <= samples; ++i)
	{
		auto const fx = map_extent.left() + map_extent.width() * i / samples;
		auto const fy = map_extent.top() + map_extent.height() * i / samples;
		if (!add_point({fx, map_extent.top()})
		    || !add_point({fx, map_extent.bottom()})
		    || !add_point({map_extent.left(), fy})
		    || !add_point({map_extent.right(), fy}))
		{
			result.error = tr("Cannot transform the area to %1.").arg(result.srs);
			return result;
		}
	}

	// Ground distances of the bounding box edges, in meters
	auto const to_projected = [&](double x, double y, bool& ok) -> QPointF {
		if (!OCTTransform(to_geographic.get(), 1, &x, &y, nullptr))
		{
			ok = false;
			return {};
		}
		bool georef_ok = false;
		auto const projected = georef.toProjectedCoords(LatLon(y, x), &georef_ok);
		ok = ok && georef_ok;
		return projected;
	};
	bool ok = true;
	auto const top_left = to_projected(min_x, max_y, ok);
	auto const top_right = to_projected(max_x, max_y, ok);
	auto const bottom_left = to_projected(min_x, min_y, ok);
	if (!ok)
	{
		result.error = tr("Cannot transform the area to %1.").arg(result.srs);
		return result;
	}
	// Grid distances to ground distances
	auto const scale = georef.getCombinedScaleFactor();
	auto const width_m = QLineF(top_left, top_right).length() / scale;
	auto const height_m = QLineF(top_left, bottom_left).length() / scale;
	auto const size_x = std::ceil(width_m / resolution);
	auto const size_y = std::ceil(height_m / resolution);
	if (!(size_x >= 1 && size_y >= 1 && size_x < std::numeric_limits<int>::max() && size_y < std::numeric_limits<int>::max()))
	{
		result.error = tr("Invalid image size.");
		return result;
	}
	result.size = QSize(int(size_x), int(size_y));

	// Let GDAL write a service description, then restrict it.
	auto const vsi_path = QByteArray(QByteArray("/vsimem/mapper_wms_")
	                      + QByteArray::number(reinterpret_cast<quintptr>(dataset.get()), 16)
	                      + ".xml");
	auto* driver = GDALGetDriverByName("WMS");
	if (!driver)
	{
		result.error = tr("The GDAL WMS driver is not available.");
		return result;
	}
	if (auto copy = DatasetPtr(GDALCreateCopy(driver, vsi_path, dataset.get(), FALSE, nullptr, nullptr, nullptr)))
	{
		copy.reset();  // flush
	}
	else
	{
		result.error = lastGdalError();
		VSIUnlink(vsi_path);
		return result;
	}

	vsi_l_offset length = 0;
	auto* buffer = VSIGetMemFileBuffer(vsi_path, &length, FALSE);
	auto xml_text = buffer ? QByteArray(reinterpret_cast<const char*>(buffer), int(length)) : QByteArray();
	VSIUnlink(vsi_path);

	auto root = XmlPtr(CPLParseXMLString(xml_text.constData()));
	auto* wms = root ? CPLGetXMLNode(root.get(), "=GDAL_WMS") : nullptr;
	if (!wms)
	{
		result.error = tr("Failed to create the WMS service description.");
		return result;
	}

	CPLSetXMLValue(wms, "DataWindow.UpperLeftX", toXmlNumber(min_x));
	CPLSetXMLValue(wms, "DataWindow.UpperLeftY", toXmlNumber(max_y));
	CPLSetXMLValue(wms, "DataWindow.LowerRightX", toXmlNumber(max_x));
	CPLSetXMLValue(wms, "DataWindow.LowerRightY", toXmlNumber(min_y));
	CPLSetXMLValue(wms, "DataWindow.SizeX", QByteArray::number(result.size.width()));
	CPLSetXMLValue(wms, "DataWindow.SizeY", QByteArray::number(result.size.height()));

	// Overviews were computed for the world-wide image size.
	if (auto* overviews = CPLGetXMLNode(wms, "OverviewCount"))
	{
		CPLRemoveXMLChild(wms, overviews);
		CPLDestroyXMLNode(overviews);
	}

	// GDAL defaults to opaque JPEG, regardless of the formats offered by the server.
	if (transparent)
	{
		CPLSetXMLValue(wms, "Service.ImageFormat", "image/png");
		CPLSetXMLValue(wms, "Service.Transparent", "TRUE");
		CPLSetXMLValue(wms, "BandsCount", "4");
	}
	else
	{
		CPLSetXMLValue(wms, "Service.ImageFormat", "image/jpeg");
		CPLSetXMLValue(wms, "Service.Transparent", "FALSE");
		CPLSetXMLValue(wms, "BandsCount", "3");
	}
	
	// An empty Cache element: GDAL uses GDAL_DEFAULT_WMS_CACHE_PATH, which
	// is set by GdalManager. So the file contains no machine-specific path.
	if (auto* cache = CPLGetXMLNode(wms, "Cache"))
	{
		CPLRemoveXMLChild(wms, cache);
		CPLDestroyXMLNode(cache);
	}
	if (use_cache)
		CPLCreateXMLNode(wms, CXT_Element, "Cache");

	auto* serialized = CPLSerializeXMLTree(wms);
	result.xml = QByteArray(serialized);
	CPLFree(serialized);

	return result;
}


}  // namespace OpenOrienteering
