/*
 *    Copyright 2026 The OpenOrienteering developers
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

#ifndef OPENORIENTEERING_WMS_SERVICE_H
#define OPENORIENTEERING_WMS_SERVICE_H

#include <vector>

#include <QByteArray>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>

namespace OpenOrienteering {

class Georeferencing;


/**
 * A layer offered by a WMS server.
 */
struct WmsLayer
{
	QString title;         ///< Human readable title, from the capabilities
	QString dataset_name;  ///< GDAL connection string ("WMS:...GetMap...")
};


/**
 * Returns a GDAL connection string for the GetCapabilities request
 * of the WMS server at the given URL.
 *
 * Existing SERVICE and REQUEST parameters in the URL are replaced,
 * other parameters (e.g. MAP=...) are preserved.
 */
QString wmsCapabilitiesUrl(const QString& server_url);


/**
 * Returns the GDAL dataset name to be used for a layer, given the GetMap
 * dataset name derived from the capabilities, and the URL entered by the user.
 * 
 * - Some servers advertise a GetMap URL which is not reachable from the
 *   internet, e.g. "http://172.28.5.2:7007/..." behind a proxy. For such
 *   URLs (private, loopback or link-local addresses, "localhost", or host
 *   names without a domain), the server part is taken from the user's URL.
 * - Query parameters from the user's URL which are not part of the WMS
 *   protocol (e.g. "token=...", "map=...") are added to the GetMap URL.
 */
QString wmsGetMapDatasetName(const QString& advertised_dataset_name, const QString& server_url);


/**
 * Returns the URL in the form used for the URL history:
 * trimmed, without "WMS:" prefix and without trailing '?' or '&'.
 */
QString normalizedWmsUrl(const QString& url);


/**
 * Returns the URL history with the given URL at the front.
 *
 * URLs are normalized (cf. normalizedWmsUrl()). Duplicates and empty
 * entries are removed, and the result has at most max_size entries.
 */
QStringList updateWmsUrlHistory(const QStringList& history, const QString& url, int max_size = 10);


/**
 * Queries the layers offered by a WMS server.
 *
 * If the URL is already a GetMap request, the result is that single layer.
 *
 * \warning This function does blocking network I/O.
 */
std::vector<WmsLayer> queryWmsLayers(const QString& server_url, QString& error);


/**
 * A GDAL WMS service description which is limited to a particular area
 * and resolution, so that it can be loaded as a regular raster template.
 */
struct WmsDescription
{
	QByteArray xml;      ///< The <GDAL_WMS> service description
	QSize size;          ///< Size of the resulting image, in pixels
	QString srs;         ///< The spatial reference system requested from the server
	QString error;       ///< Error message, if xml is empty

	bool isValid() const { return !xml.isEmpty(); }
};


/**
 * Creates a GDAL WMS service description for the given layer which covers
 * map_extent (in map coordinates of a map with the given georeferencing)
 * at the given ground resolution (in meters per pixel).
 *
 * If cache_path is not empty, GDAL's tile cache is enabled at this path.
 *
 * If transparent is true, images are requested as PNG with transparent
 * background, for overlay layers (e.g. buildings, parcels). Otherwise,
 * images are requested as JPEG, which is smaller for photos.
 *
 * This function does not access the network.
 */
WmsDescription makeWmsDescription(const QString& dataset_name,
                                  const Georeferencing& georef,
                                  const QRectF& map_extent,
                                  double resolution,
                                  const QString& cache_path = {},
                                  bool transparent = false);


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_WMS_SERVICE_H
