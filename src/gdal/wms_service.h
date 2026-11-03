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
 * This function does not access the network.
 */
WmsDescription makeWmsDescription(const QString& dataset_name,
                                  const Georeferencing& georef,
                                  const QRectF& map_extent,
                                  double resolution,
                                  const QString& cache_path = {});


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_WMS_SERVICE_H
