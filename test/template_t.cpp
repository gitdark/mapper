/*
 *    Copyright 2017-2020 Kai Pastor
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


#include <algorithm>
#include <cmath>
#include <iosfwd>
#include <memory>
#include <vector>

#include <Qt>
#include <QtGlobal>
#include <QtMath>
#include <QtTest>
#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QRegularExpression>
#include <QImageWriter>
#include <QIODevice>
#include <QLineF>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSignalSpy>  // IWYU pragma: keep
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTransform>

#ifdef ACCEPT_USE_OF_DEPRECATED_PROJ_API_H
#  include <proj_api.h>
#endif

#include "test_config.h"

#include "global.h"
#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map.h"
#include "core/map_coord.h"
#include "core/map_view.h"
#include "fileformats/file_format.h"
#include "fileformats/file_format_registry.h"
#include "fileformats/file_import_export.h"
#include "fileformats/xml_file_format_p.h"
#include "gdal/ogr_template.h"
#include "gdal/gdal_manager.h"
#ifdef MAPPER_USE_GDAL
#  include "gdal/gdal_image_reader.h"
#  include "gdal/wms_service.h"
#endif
#include "templates/template.h"
#include "templates/template_image.h"
#include "templates/template_table_model.h"
#include "templates/template_track.h"
#include "templates/world_file.h"

#ifdef MAPPER_USE_GDAL
#  include "gdal/gdal_file.h"
#endif

using namespace OpenOrienteering;

namespace
{

QPointF center(const Template* temp)
{
	// TemplateTrack intentionally doesn't provide a tight extent
	// but the bounding box is fine.
	if (auto* template_track = qobject_cast<const TemplateTrack*>(temp))
		return template_track->calculateTemplateBoundingBox().center();
	return temp->templateToMap(temp->getTemplateExtent().center());
}

}  // namespace


/**
 * @test Tests template classes.
 */
class TemplateTest : public QObject
{
Q_OBJECT
private slots:
	void initTestCase()
	{
		// Use distinct QSettings
		QCoreApplication::setOrganizationName(QString::fromLatin1("OpenOrienteering.org"));
		QCoreApplication::setApplicationName(QString::fromLatin1(metaObject()->className()));
		QVERIFY2(QDir::home().exists(), "The home dir must be writable in order to use QSettings.");
		
		Q_INIT_RESOURCE(resources);
		doStaticInitializations();
		// Static map initializations
		Map map;
		QDir::addSearchPath(QStringLiteral("testdata"), QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)).absoluteFilePath(QStringLiteral("data")));
	}
	
#ifdef MAPPER_USE_GDAL
	// These tests are related to GDAL/OGR template support, but also for OgrFileFormat.
	void gdalUtilTest()
	{
		QString const test_file = QStringLiteral("testdata:templates/vsi-test.kmz");
		auto const test_file_info = QFileInfo(test_file);
		QVERIFY(test_file_info.exists());
		
		auto const abs_path_utf8 = test_file_info.absoluteFilePath().toUtf8();
		QVERIFY(GdalFile::exists(abs_path_utf8));
		QVERIFY(!GdalFile::exists(abs_path_utf8 + "_xy"));
		
		auto const vsizip = QByteArray("/vsizip/" + abs_path_utf8);
		QVERIFY(GdalFile::exists(vsizip + "/doc.kml"));
		QVERIFY(!GdalFile::exists(vsizip + "/doc.kml_xy"));
		QVERIFY(GdalFile::exists(vsizip + "/files"));
		QVERIFY(!GdalFile::exists(vsizip + "/files_xy"));
		
		QVERIFY(GdalFile::exists(vsizip + "/files/1.jpg"));
		QVERIFY(!GdalFile::exists(vsizip + "/files/1.jpg_xy"));
		
		QVERIFY(GdalFile::isRelative("files/1.jpg"));
		QVERIFY(GdalFile::isRelative("../doc.kml"));
		
		QVERIFY(GdalFile::isDir(vsizip + "/files"));
		QVERIFY(!GdalFile::isDir(vsizip + "/doc.kml"));
		QVERIFY(!GdalFile::isDir(vsizip + "/doc.kml_xy"));
		
		auto resolved_path = GdalFile::tryToFindRelativeTemplateFile("files/1.jpg", vsizip);
		QVERIFY(!resolved_path.isEmpty());
		QVERIFY(resolved_path.startsWith("/vsizip/"));
		QVERIFY(resolved_path.endsWith("/1.jpg"));
		QVERIFY(GdalFile::exists(resolved_path));
		
		resolved_path = GdalFile::tryToFindRelativeTemplateFile("files/1.jpg", vsizip + "/doc.kml");
		QVERIFY(!resolved_path.isEmpty());
		QVERIFY(resolved_path.startsWith("/vsizip/"));
		QVERIFY(resolved_path.endsWith("/1.jpg"));
		QVERIFY(GdalFile::exists(resolved_path));
		
		resolved_path = GdalFile::tryToFindRelativeTemplateFile("../doc.kml", vsizip + "/files/1.jpg");
		QVERIFY(!resolved_path.isEmpty());
		QVERIFY(resolved_path.startsWith("/vsizip/"));
		QVERIFY(resolved_path.endsWith("/doc.kml"));
		QVERIFY(GdalFile::exists(resolved_path));
		
		resolved_path = GdalFile::tryToFindRelativeTemplateFile("../doc.kml_xy", vsizip + "/files/1.jpg");
		QVERIFY(resolved_path.isEmpty());
	}
#endif
	
	void worldFilePathTest()
	{
		QCOMPARE(WorldFile::pathForImage(QStringLiteral("dir.ext/file-noext")), QStringLiteral("dir.ext/file-noext.wld"));
		QCOMPARE(WorldFile::pathForImage(QStringLiteral("dir/file.")), QStringLiteral("dir/file.wld"));
		QCOMPARE(WorldFile::pathForImage(QStringLiteral("dir/file.j")), QStringLiteral("dir/file.jw"));
		QCOMPARE(WorldFile::pathForImage(QStringLiteral("dir/file.jp")), QStringLiteral("dir/file.jpw"));
		QCOMPARE(WorldFile::pathForImage(QStringLiteral("dir/file.jpg")), QStringLiteral("dir/file.jgw"));
		QCOMPARE(WorldFile::pathForImage(QStringLiteral("dir/file.jpeg")), QStringLiteral("dir/file.jpegw"));
		QCOMPARE(WorldFile::pathForImage(QStringLiteral("dir/file.png.jpeg")), QStringLiteral("dir/file.png.jpegw"));
	}
	
	void worldFileUnitTest()
	{
		auto world_file = WorldFile { 1, 2, 3, 4, 5, 6 };
		QCOMPARE(world_file.parameters[0], 1.0);
		QCOMPARE(world_file.parameters[1], 2.0);
		QCOMPARE(world_file.parameters[2], 3.0);
		QCOMPARE(world_file.parameters[3], 4.0);
		QCOMPARE(world_file.parameters[4], 5.0);
		QCOMPARE(world_file.parameters[5], 6.0);
		
		auto const transform = QTransform(world_file);
		QCOMPARE(transform.m11(), 1.0);
		QCOMPARE(transform.m12(), 2.0);
		QCOMPARE(transform.m21(), 3.0);
		QCOMPARE(transform.m22(), 4.0);
		QCOMPARE(transform.map(QPointF(0.5, 0.5)), QPointF(5, 6));
		
		WorldFile world_file_2;
		QCOMPARE(QTransform(world_file_2), QTransform{});
		
		world_file = world_file_2;
		QCOMPARE(QTransform(world_file), QTransform{});
		
		auto const verify_parameters = [](auto const & parameters) {
			QCOMPARE(parameters[0], 0.15);
			QCOMPARE(parameters[1], 0.0);
			QCOMPARE(parameters[2], 0.0);
			QCOMPARE(parameters[3], -0.15);
			QCOMPARE(parameters[4], 649078.0);
			QCOMPARE(parameters[5], 394159.0);
		};
		
		QString world_file_path = QStringLiteral("testdata:templates/world-file.pgw");
		QVERIFY(world_file.load(world_file_path));
		verify_parameters(world_file.parameters);
		
		QString image_path = QStringLiteral("testdata:templates/world-file.png");
		QVERIFY(world_file_2.tryToLoadForImage(image_path));
		verify_parameters(world_file_2.parameters);
		
		// Try reading garbage
		QVERIFY(!world_file.load(image_path));
	}
	
	void worldFileTemplateTest()
	{
		Map map;
		MapView view{ &map };
		QVERIFY(map.loadFrom(QStringLiteral("testdata:templates/world-file.xmap"), &view));
		
		const auto& georef = map.getGeoreferencing();
		QCOMPARE(georef.getState(), Georeferencing::Geospatial);
		
		QCOMPARE(map.getNumTemplates(), 1);
		auto temp = map.getTemplate(0);
		QCOMPARE(temp->getTemplateType(), "TemplateImage");
		QCOMPARE(temp->getTemplateFilename(), QString::fromLatin1("world-file.png"));
		QVERIFY(temp->isTemplateGeoreferenced());
		
		QVERIFY(temp->getTemplateState() != Template::Invalid);
		QCOMPARE(temp->getTemplateRelativePath(), QStringLiteral("world-file.png"));
		QCOMPARE(temp->getTemplateRelativePath(nullptr), QStringLiteral("world-file.png"));
		auto base_dir = QDir(QStringLiteral("testdata:templates"));
		QCOMPARE(temp->getTemplateRelativePath(&base_dir), QStringLiteral("world-file.png"));
		base_dir.cdUp();
		QCOMPARE(temp->getTemplateRelativePath(&base_dir), QStringLiteral("templates/world-file.png"));
		
		map.loadTemplateFiles(view);
		QCOMPARE(temp->getTemplateState(), Template::Loaded);
		auto rotation_template = 0.01 * qRound(100 * qRadiansToDegrees(temp->getTemplateRotation()));
		auto rotation_map = 0.01 * qRound(100 * georef.getGrivation());
		QCOMPARE(rotation_template, rotation_map);
		
		// Test boundingRect for TemplateImage
		QCOMPARE(temp->boundingRect().center(), center(temp));
	}
	
	
	void templatePathTest()
	{
		QFile file{ QStringLiteral("testdata:templates/world-file.xmap") };
		QVERIFY(file.open(QIODevice::ReadOnly));
		auto original_data = file.readAll();
		QCOMPARE(file.error(), QFileDevice::NoError);
		file.close();
		
		Map map;
		MapView view{ &map };
		
		// The buffer has no path, so the template cannot be loaded.
		// This results in a warning.
		QBuffer buffer{ &original_data };
		buffer.open(QIODevice::ReadOnly);
		XMLFileImporter importer{ {}, &map, &view };
		importer.setDevice(&buffer);
		QVERIFY(importer.doImport());
		QCOMPARE(importer.warnings().size(), std::size_t(1));
		
		// The image is in Invalid state, but path attributes are intact.
		QCOMPARE(map.getNumTemplates(), 1);
		auto temp = map.getTemplate(0);
		QCOMPARE(temp->getTemplateType(), "TemplateImage");
		QCOMPARE(temp->getTemplateFilename(), QStringLiteral("world-file.png"));
		QCOMPARE(temp->getTemplatePath(), QStringLiteral("world-file.png"));
		QCOMPARE(temp->getTemplateRelativePath(), QStringLiteral("world-file.png"));
		QCOMPARE(temp->getTemplateState(), Template::Invalid);
		
		QCOMPARE(temp->getTemplateRelativePath(nullptr), QStringLiteral("world-file.png"));
		auto base_dir = QDir(QStringLiteral("testdata:templates"));
		QCOMPARE(temp->getTemplateRelativePath(&base_dir), QStringLiteral("world-file.png"));
		base_dir.cdUp();
		QCOMPARE(temp->getTemplateRelativePath(&base_dir), QStringLiteral("world-file.png"));
		
		QBuffer out_buffer;
		QVERIFY(out_buffer.open(QIODevice::WriteOnly));
		XMLFileExporter exporter{ {}, &map, &view };
		exporter.setOption(QStringLiteral("autoFormatting"), true);
		exporter.setDevice(&out_buffer);
		exporter.doExport();
		out_buffer.close();
		QCOMPARE(exporter.warnings().size(), std::size_t(0));
		
		// The exported data matches the original data.
		QCOMPARE(out_buffer.buffer(), original_data);
	}
	
	void templateImageDrawableTest()
	{
		QVERIFY(!QImageWriter::supportedImageFormats().isEmpty());
		
		Map map;
		XMLFileImporter importer{ QStringLiteral("testdata:templates/world-file.xmap"), &map, nullptr };
		QVERIFY(importer.doImport());
		
		QCOMPARE(map.getNumTemplates(), 1);
		auto* temp = map.getTemplate(0);
		QCOMPARE(temp->getTemplateType(), "TemplateImage");
		QCOMPARE(temp->getTemplateFilename(), QStringLiteral("world-file.png"));
		QCOMPARE(temp->getTemplateState(), Template::Unloaded);
		QVERIFY(!temp->canBeDrawnOnto());
		
		QVERIFY(temp->loadTemplateFile());
		QCOMPARE(temp->getTemplateState(), Template::Loaded);
		if (!QImageWriter::supportedImageFormats().contains("png"))
			QEXPECT_FAIL("", "No PNG support in this build of Qt", Abort);
		QVERIFY(temp->canBeDrawnOnto());
		QVERIFY(!temp->hasUnsavedChanges());
		
		QTemporaryDir tmp_dir;
		QVERIFY(tmp_dir.isValid());
		temp->setTemplatePath(tmp_dir.filePath(temp->getTemplateFilename()));
		QVERIFY(temp->canBeDrawnOnto());
		QVERIFY(!temp->hasUnsavedChanges());
		
		auto const box = temp->calculateTemplateBoundingBox();
		MapCoordF map_coords[] = { MapCoordF(box.topLeft()), MapCoordF(box.bottomRight()) };
		temp->drawOntoTemplate(map_coords, 2, QColor(Qt::red), box.width()+box.height(), box, {});
		QVERIFY(temp->hasUnsavedChanges());
		
#if !defined(MAPPER_BIG_ENDIAN)
		auto format = FileFormats.findFormatForFilename(QStringLiteral("some.ocd"), &FileFormat::supportsFileSave);
		QVERIFY(bool(format));
		auto exporter = format->makeExporter(QString{}, &map, nullptr);
		QVERIFY(bool(exporter));
		QBuffer buffer;
		exporter->setDevice(&buffer);
		QVERIFY(exporter->doExport());
		QVERIFY(!temp->hasUnsavedChanges());
#endif  // !defined(MAPPER_BIG_ENDIAN)
	}
	
	void geoTiffTemplateTest()
	{
		Map map;
		MapView view{ &map };
		QVERIFY(map.loadFrom(QStringLiteral("testdata:templates/geotiff.xmap"), &view));
		
		const auto& georef = map.getGeoreferencing();
		QCOMPARE(georef.getState(), Georeferencing::Geospatial);
		
		QCOMPARE(map.getNumTemplates(), 1);
		auto temp = map.getTemplate(0);
		QCOMPARE(temp->getTemplateFilename(), QString::fromWCharArray(L"\u0433\u0435\u043E.tiff"));
#ifndef MAPPER_USE_GDAL
		QCOMPARE(temp->getTemplateType(), "TemplateImage");
#else
		QCOMPARE(temp->getTemplateType(), "GdalTemplate");
#ifdef Q_OS_UNIX
		if (QFile::encodeName(temp->getTemplateFilename()) != temp->getTemplateFilename().toUtf8())
			QEXPECT_FAIL("", "Local 8 bit encoding is not UTF-8", Abort);
#endif
		QVERIFY(temp->isTemplateGeoreferenced());
		
		map.loadTemplateFiles(view);
		QCOMPARE(temp->getTemplateState(), Template::Loaded);
		auto rotation_template = 0.01 * qRound(100 * qRadiansToDegrees(temp->getTemplateRotation()));
		auto rotation_map = 0.01 * qRound(100 * georef.getGrivation());
		QCOMPARE(rotation_template, rotation_map);
#endif
	}
	
#ifdef MAPPER_USE_GDAL
	void geoTiffSRSTest()
	{
		auto file_info = QFileInfo(QStringLiteral("testdata:templates/epsg-27700.tiff"));
		QVERIFY(file_info.exists());
		
		Map map;
		auto temp = Template::templateForPath(file_info.absoluteFilePath(), &map);
		QCOMPARE(temp->getTemplateType(), "GdalTemplate");
		QVERIFY(temp->loadTemplateFile());
		
		auto gdal_template = qobject_cast<TemplateImage*>(temp.get());
		QVERIFY(gdal_template);
		auto options = gdal_template->availableGeoreferencing();
		QVERIFY(!options.template_file.crs_spec.isEmpty());
		
		Georeferencing georef;
		georef.setProjectedCRS({}, options.template_file.crs_spec);
		bool ok;
		auto projected = georef.toProjectedCoords(LatLon(54.558203, -3.393209), &ok);
		QVERIFY(ok);
		auto expected = QPointF{310000, 519000};
		if (QLineF(projected, expected).length() > 0.5)
			QCOMPARE(projected, expected);
		else
			QVERIFY2(true, "SRS from GeoTIFF is okay");
	}
#endif
	
#ifdef MAPPER_USE_GDAL
	void gdalImageReaderProgressTest()
	{
		auto const path = QFileInfo(QStringLiteral("testdata:templates/epsg-27700.tiff")).absoluteFilePath();
		
		// Complete read with progress
		{
			GdalImageReader reader(path);
			QVERIFY(reader.canRead());
			QVERIFY(!reader.isNetworkSource());
			std::vector<double> values;
			QImage image;
			QVERIFY(reader.read(&image, [&values](double complete) { values.push_back(complete); return true; }));
			QVERIFY(!reader.wasCancelled());
			QVERIFY(!image.isNull());
			QVERIFY(values.size() >= 2);
			QCOMPARE(values.front(), 0.0);
			QCOMPARE(values.back(), 1.0);
			QVERIFY(std::is_sorted(values.begin(), values.end()));
		}
		
		// Cancelled before reading
		{
			GdalImageReader reader(path);
			QImage image;
			QVERIFY(!reader.read(&image, [](double) { return false; }));
			QVERIFY(reader.wasCancelled());
			QVERIFY(!reader.errorString().isEmpty());
		}
		
		// Cancelled while reading
		{
			GdalImageReader reader(path);
			QImage image;
			QVERIFY(!reader.read(&image, [](double complete) { return complete == 0.0; }));
			QVERIFY(reader.wasCancelled());
		}
		
		// Plain read still works, and resets the cancelled flag
		{
			GdalImageReader reader(path);
			QImage image;
			QVERIFY(!reader.read(&image, [](double) { return false; }));
			QVERIFY(reader.read(&image));
			QVERIFY(!reader.wasCancelled());
		}
	}
	
	void wmsCapabilitiesUrlTest()
	{
		QCOMPARE(wmsCapabilitiesUrl(QStringLiteral("https://example.com/wms")),
		         QStringLiteral("WMS:https://example.com/wms?SERVICE=WMS&REQUEST=GetCapabilities"));
		QCOMPARE(wmsCapabilitiesUrl(QStringLiteral(" WMS:https://example.com/wms?map=x&service=wms&request=GetMap ")),
		         QStringLiteral("WMS:https://example.com/wms?map=x&SERVICE=WMS&REQUEST=GetCapabilities"));
	}
	
	void wmsUrlHistoryTest()
	{
		QCOMPARE(normalizedWmsUrl(QStringLiteral("  WMS:https://a.example/wms?  ")), QStringLiteral("https://a.example/wms"));
		QCOMPARE(normalizedWmsUrl(QStringLiteral("https://a.example/wms?map=x&")), QStringLiteral("https://a.example/wms?map=x"));
		
		auto const a = QStringLiteral("https://a.example/wms");
		auto const b = QStringLiteral("https://b.example/wms");
		auto const c = QStringLiteral("https://c.example/wms");
		
		// New URL goes to the front
		QCOMPARE(updateWmsUrlHistory({a, b}, c), (QStringList{c, a, b}));
		// Reused URL moves to the front, without duplicate
		QCOMPARE(updateWmsUrlHistory({a, b, c}, c), (QStringList{c, a, b}));
		// Normalization merges variants
		QCOMPARE(updateWmsUrlHistory({a, b}, QStringLiteral(" https://b.example/wms? ")), (QStringList{b, a}));
		// Empty URL and empty entries are dropped
		QCOMPARE(updateWmsUrlHistory({a, QString(), b}, QString()), (QStringList{a, b}));
		// Size limit
		QStringList many;
		for (int i = 0; i < 15; ++i)
			many.push_back(QStringLiteral("https://h%1.example/wms").arg(i));
		auto const limited = updateWmsUrlHistory(many, a);
		QCOMPARE(limited.size(), 10);
		QCOMPARE(limited.front(), a);
		QCOMPARE(limited.back(), QStringLiteral("https://h8.example/wms"));
		QCOMPARE(updateWmsUrlHistory(many, a, 3), (QStringList{a, many[0], many[1]}));
	}
	
	void wmsGetMapDatasetNameTest_data()
	{
		QTest::addColumn<QString>("advertised");
		QTest::addColumn<QString>("server_url");
		QTest::addColumn<QString>("expected");
		
		auto const getmap = QStringLiteral("?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=a&SRS=EPSG:3765&BBOX=1,2,3,4");
		QTest::newRow("public, unchanged")
		        << QString(QLatin1String("WMS:https://maps.example.com/wms") + getmap)
		        << QStringLiteral("https://maps.example.com/wms")
		        << QString(QLatin1String("WMS:https://maps.example.com/wms") + getmap);
		QTest::newRow("other public host, unchanged")
		        << QString(QLatin1String("WMS:https://tiles.example.org/ows") + getmap)
		        << QStringLiteral("https://maps.example.com/wms")
		        << QString(QLatin1String("WMS:https://tiles.example.org/ows") + getmap);
		QTest::newRow("private IPv4 -> user URL")
		        << QString(QLatin1String("WMS:http://172.28.5.2:7007/ows2-m/wms") + getmap)
		        << QStringLiteral("https://oss.example.hr/OssWebServices/wms")
		        << QString(QLatin1String("WMS:https://oss.example.hr/OssWebServices/wms") + getmap);
		QTest::newRow("localhost -> user URL")
		        << QString(QLatin1String("WMS:http://localhost:8080/geoserver/wms") + getmap)
		        << QStringLiteral("https://maps.example.com/wms")
		        << QString(QLatin1String("WMS:https://maps.example.com/wms") + getmap);
		QTest::newRow("172.32 is public")
		        << QString(QLatin1String("WMS:http://172.32.0.1/wms") + getmap)
		        << QStringLiteral("https://maps.example.com/wms")
		        << QString(QLatin1String("WMS:http://172.32.0.1/wms") + getmap);
		QTest::newRow("token is carried over, WMS parameters are not")
		        << QString(QLatin1String("WMS:http://10.0.0.5/wms") + getmap)
		        << QStringLiteral("https://maps.example.com/wms?token=abc&request=GetCapabilities&VERSION=1.3.0")
		        << QString(QLatin1String("WMS:https://maps.example.com/wms") + getmap + QLatin1String("&token=abc"));
		QTest::newRow("internal user URL, unchanged")
		        << QString(QLatin1String("WMS:http://10.0.0.5/wms") + getmap)
		        << QStringLiteral("http://192.168.1.2/wms")
		        << QString(QLatin1String("WMS:http://10.0.0.5/wms") + getmap);
	}
	
	void wmsGetMapDatasetNameTest()
	{
		QFETCH(QString, advertised);
		QFETCH(QString, server_url);
		QFETCH(QString, expected);
		QCOMPARE(wmsGetMapDatasetName(advertised, server_url), expected);
	}
	
	void wmsUnreachableTemplateTest()
	{
		// A failed download must not result in a blank template.
		// Port 9 (discard) on localhost: connections are refused immediately.
		auto const xml = QByteArray(
		    "<GDAL_WMS><Service name=\"WMS\"><Version>1.1.1</Version>"
		    "<ServerUrl>http://127.0.0.1:9/wms?</ServerUrl><SRS>EPSG:3857</SRS>"
		    "<ImageFormat>image/png</ImageFormat><Layers>x</Layers></Service>"
		    "<DataWindow><UpperLeftX>0</UpperLeftX><UpperLeftY>100</UpperLeftY>"
		    "<LowerRightX>100</LowerRightX><LowerRightY>0</LowerRightY>"
		    "<SizeX>100</SizeX><SizeY>100</SizeY></DataWindow>"
		    "<BandsCount>3</BandsCount><MaxConnections>1</MaxConnections><Timeout>5</Timeout></GDAL_WMS>");
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		auto const path = dir.filePath(QStringLiteral("unreachable.xml"));
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write(xml);
		file.close();
		
		Map map;
		auto temp = Template::templateForPath(path, &map);
		QVERIFY(temp);
		QCOMPARE(temp->getTemplateType(), "GdalTemplate");
		QVERIFY(!temp->loadTemplateFile());
		QCOMPARE(temp->getTemplateState(), Template::Invalid);
		QVERIFY(!temp->errorString().isEmpty());
	}
	
	void wmsDescriptionTest_data()
	{
		QTest::addColumn<QString>("dataset_name");
		QTest::addColumn<QString>("srs");
		QTest::addColumn<double>("expected_left");  // approximate
		
		QTest::newRow("WMS 1.1.1, EPSG:3857")
		        << QStringLiteral("WMS:https://example.invalid/wms?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=test"
		                          "&SRS=EPSG:3857&BBOX=-20037508.34,-20037508.34,20037508.34,20037508.34")
		        << QStringLiteral("EPSG:3857") << 1669792.0;
		QTest::newRow("WMS 1.3.0, EPSG:4326")
		        << QStringLiteral("WMS:https://example.invalid/wms?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=test"
		                          "&CRS=EPSG:4326&BBOX=-90,-180,90,180")
		        << QStringLiteral("EPSG:4326") << 15.0;
	}
	
	void wmsDescriptionTest()
	{
		QFETCH(QString, dataset_name);
		QFETCH(QString, srs);
		QFETCH(double, expected_left);
		
		// 2 km x 2 km around 15°E, 51.5°N at 1:10000
		Georeferencing georef;
		georef.setScaleDenominator(10000);
		georef.setProjectedCRS(QStringLiteral("UTM 33"), QStringLiteral("+proj=utm +zone=33 +datum=WGS84"));
		QCOMPARE(georef.getState(), Georeferencing::Geospatial);
		georef.setProjectedRefPoint(QPointF{500000, 5705000});
		auto const extent = QRectF{-100, -100, 200, 200};
		
		auto const description = makeWmsDescription(dataset_name, georef, extent, 1.0, QStringLiteral("/tmp/wms-cache"));
		QVERIFY2(description.isValid(), qPrintable(description.error));
		QCOMPARE(description.srs, srs);
		// 2000 m at 1 m/px, slightly more due to the enclosing bounding box
		QVERIFY2(description.size.width() >= 1995 && description.size.width() <= 2050, qPrintable(QString::number(description.size.width())));
		QVERIFY2(description.size.height() >= 1995 && description.size.height() <= 2050, qPrintable(QString::number(description.size.height())));
		
		auto const xml = QString::fromUtf8(description.xml);
		QVERIFY(xml.contains(QStringLiteral("<SizeX>%1</SizeX>").arg(description.size.width())));
		QVERIFY(xml.contains(QStringLiteral("<Path>/tmp/wms-cache</Path>")));
		QVERIFY(!xml.contains(QStringLiteral("OverviewCount")));
		// Default: opaque JPEG
		QVERIFY(xml.contains(QStringLiteral("<ImageFormat>image/jpeg</ImageFormat>")));
		QVERIFY(xml.contains(QStringLiteral("<Transparent>FALSE</Transparent>")));
		QVERIFY(xml.contains(QStringLiteral("<BandsCount>3</BandsCount>")));
		{
			auto const transparent = makeWmsDescription(dataset_name, georef, extent, 1.0, {}, true);
			QVERIFY2(transparent.isValid(), qPrintable(transparent.error));
			auto const transparent_xml = QString::fromUtf8(transparent.xml);
			QVERIFY(transparent_xml.contains(QStringLiteral("<ImageFormat>image/png</ImageFormat>")));
			QVERIFY(transparent_xml.contains(QStringLiteral("<Transparent>TRUE</Transparent>")));
			QVERIFY(transparent_xml.contains(QStringLiteral("<BandsCount>4</BandsCount>")));
			QVERIFY(!transparent_xml.contains(QStringLiteral("<Cache>")));
		}
		auto const left = QRegularExpression(QStringLiteral("<UpperLeftX>([^<]*)</UpperLeftX>")).match(xml).captured(1).toDouble();
		QVERIFY2(std::abs(left - expected_left) < std::abs(expected_left) * 0.001, qPrintable(QString::number(left, 'f')));
		
		// The description can be opened as a GDAL template (without network access).
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		auto const path = dir.filePath(QStringLiteral("wms.xml"));
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		QCOMPARE(file.write(description.xml), qint64(description.xml.size()));
		file.close();
		
		Map map;
		auto temp = Template::templateForPath(path, &map);
		QVERIFY(temp);
		QCOMPARE(temp->getTemplateType(), "GdalTemplate");
		
		if (qEnvironmentVariableIsEmpty("MAPPER_TEST_NETWORK"))
			return;
		
		// End-to-end with a public server
		auto const layers_url = QStringLiteral("https://ows.terrestris.de/osm/service");
		QString error;
		auto const layers = queryWmsLayers(layers_url, error);
		QVERIFY2(!layers.empty(), qPrintable(error));
		auto const live = makeWmsDescription(layers.front().dataset_name, georef, extent, 4.0);
		QVERIFY2(live.isValid(), qPrintable(live.error));
		QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
		file.write(live.xml);
		file.close();
		auto live_temp = Template::templateForPath(path, &map);
		QVERIFY2(live_temp->loadTemplateFile(), qPrintable(live_temp->errorString()));
		auto const* image_template = qobject_cast<TemplateImage*>(live_temp.get());
		QVERIFY(image_template);
		QCOMPARE(image_template->getImage().size(), live.size);
	}
#endif
	
	void templateTrackTest_data()
	{
		QTest::addColumn<QString>("map_file");
		QTest::addColumn<int>("template_index");

		QTest::newRow("TemplateTrack georef")         << QStringLiteral("testdata:templates/template-track.xmap") << 0;
		QTest::newRow("TemplateTrack non-georef")     << QStringLiteral("testdata:templates/template-track.xmap") << 1;
		QTest::newRow("TemplateTrack OGR georef")     << QStringLiteral("testdata:templates/template-track.xmap") << 2;
		QTest::newRow("TemplateTrack OGR non-georef") << QStringLiteral("testdata:templates/template-track.xmap") << 3;
		QTest::newRow("TemplateTrack NAD83")          << QStringLiteral("testdata:templates/template-track-NA.xmap") << 0;
		QTest::newRow("OgrTemplate NAD83")            << QStringLiteral("testdata:templates/template-track-NA.xmap") << 1;
		QTest::newRow("TemplateTrack from v0.8.4")    << QStringLiteral("testdata:templates/template-track-NA-084.xmap") << 0;
		QTest::newRow("TemplateTrack from v0.9.3")    << QStringLiteral("testdata:templates/template-track-NA-093-PROJ.xmap") << 0;
	}
	
	void templateTrackTest()
	{
#ifdef MAPPER_USE_GDAL
		GdalManager().setFormatEnabled(GdalManager::GPX, false);
#endif
		
		QFETCH(QString, map_file);
		QFETCH(int, template_index);
		
		Map map;
		MapView view{ &map };
		QVERIFY(map.loadFrom(map_file, &view));
		
		QVERIFY(map.getNumTemplates() > template_index);
		auto const* temp = map.getTemplate(template_index);
		QCOMPARE(temp->getTemplateType(), "TemplateTrack");
		QCOMPARE(temp->getTemplateState(), Template::Unloaded);
		QVERIFY(map.getTemplate(template_index)->loadTemplateFile());
		QCOMPARE(temp->getTemplateState(), Template::Loaded);

#if !defined(ACCEPT_USE_OF_DEPRECATED_PROJ_API_H) || PJ_VERSION >= 600
		QEXPECT_FAIL("TemplateTrack from v0.8.4", "Unsupported WGS 84 -> NAD 83 transformation", Continue);
#else
		QEXPECT_FAIL("TemplateTrack NAD83", "Unsupported WGS 84 -> NAD 83 transformation", Continue);
		QEXPECT_FAIL("OgrTemplate NAD83", "Unsupported WGS 84 -> NAD 83 transformation", Continue);
		QEXPECT_FAIL("TemplateTrack from v0.9.3", "Unsupported WGS 84 -> NAD 83 transformation", Continue);
#endif
		auto const expected_center = map.calculateExtent().center();
		if (QLineF(center(temp), expected_center).length() > 0.125) // 50 cm
			QCOMPARE(center(temp), expected_center);
		else
			QVERIFY2(true, "Centers do match");
	}
	
#ifdef MAPPER_USE_GDAL
	void ogrTemplateTest_data()
	{
		QTest::addColumn<QString>("map_file");
		QTest::addColumn<int>("template_index");

		QTest::newRow("TemplateTrack georef")         << QStringLiteral("testdata:templates/template-track.xmap") << 0;
		QTest::newRow("TemplateTrack non-georef")     << QStringLiteral("testdata:templates/template-track.xmap") << 1;
		QTest::newRow("TemplateTrack OGR georef")     << QStringLiteral("testdata:templates/template-track.xmap") << 2;
		QTest::newRow("TemplateTrack OGR non-georef") << QStringLiteral("testdata:templates/template-track.xmap") << 3;
		QTest::newRow("OgrTemplate basic")            << QStringLiteral("testdata:templates/ogr-template.xmap")   << 0;
		QTest::newRow("OgrTemplate compatibility")    << QStringLiteral("testdata:templates/ogr-template.xmap")   << 1;
		QTest::newRow("TemplateTrack NAD83")          << QStringLiteral("testdata:templates/template-track-NA.xmap") << 0;
		QTest::newRow("OgrTemplate NAD83")            << QStringLiteral("testdata:templates/template-track-NA.xmap") << 1;
		QTest::newRow("TemplateTrack from v0.8.4")    << QStringLiteral("testdata:templates/template-track-NA-084.xmap") << 0;
		QTest::newRow("OGRTemplate from v0.9.3")      << QStringLiteral("testdata:templates/template-track-NA-093-GDAL.xmap") << 0;
	}
	
	void ogrTemplateTest()
	{
		GdalManager().setFormatEnabled(GdalManager::GPX, true);
		
		QFETCH(QString, map_file);
		QFETCH(int, template_index);
		
		Map map;
		MapView view{ &map };
		QVERIFY(map.loadFrom(map_file, &view));
		
		QVERIFY(map.getNumTemplates() > template_index);
		auto const* temp = map.getTemplate(template_index);
		QCOMPARE(temp->getTemplateType(), "OgrTemplate");
		QCOMPARE(temp->getTemplateState(), Template::Unloaded);
		QVERIFY(map.getTemplate(template_index)->loadTemplateFile());
		QCOMPARE(temp->getTemplateState(), Template::Loaded);

		QEXPECT_FAIL("TemplateTrack NAD83", "Unsupported WGS 84 -> NAD 83 transformation", Continue);
		QEXPECT_FAIL("OgrTemplate NAD83", "Unsupported WGS 84 -> NAD 83 transformation", Continue);
		auto const expected_center = map.calculateExtent().center();
		if (QLineF(center(temp), expected_center).length() > 0.25) // 1 m
			QCOMPARE(center(temp), expected_center);
		else
			QVERIFY2(true, "Centers do match");
		
		// Test boundingRect for OgrTemplate
		QCOMPARE(temp->boundingRect().center(), center(temp));
	}
	
	void ogrTemplateGeoreferencingTest()
	{
		auto const osm_fileinfo = QFileInfo(QStringLiteral("testdata:templates/map.osm"));
		QVERIFY(osm_fileinfo.exists());
		auto georef = OgrTemplate::getDataGeoreferencing(osm_fileinfo.absoluteFilePath(), Georeferencing{});
		QVERIFY(bool(georef));
		auto latlon = georef->getGeographicRefPoint();
		QCOMPARE(qRound(latlon.latitude()), 50);
		QCOMPARE(qRound(latlon.longitude()), 8);
	}
	
	void templateTypesConsistentTest_data()
	{
		QTest::addColumn<QString>("map_file");
		QTest::addColumn<int>("template_index");

		QTest::newRow("TemplateTrack georef")     << QStringLiteral("testdata:templates/template-track.xmap") << 0;
		QTest::newRow("OgrTemplate georef")       << QStringLiteral("testdata:templates/template-track.xmap") << 2;
		QTest::newRow("TemplateTrack NAD83")      << QStringLiteral("testdata:templates/template-track-NA.xmap") << 0;
		QTest::newRow("OgrTemplate NAD83")        << QStringLiteral("testdata:templates/template-track-NA.xmap") << 1;
	}
	
	void templateTypesConsistentTest()
	{
		QFETCH(QString, map_file);
		QFETCH(int, template_index);
		
		GdalManager().setFormatEnabled(GdalManager::GPX, false);
		QPointF template_track_center;
		{
			Map map;
			MapView view{ &map };
			QVERIFY(map.loadFrom(map_file, &view));

			QVERIFY(map.getNumTemplates() > template_index);
			auto const* temp = map.getTemplate(template_index);
			QCOMPARE(temp->getTemplateType(), "TemplateTrack");
			QCOMPARE(temp->getTemplateState(), Template::Unloaded);
			QVERIFY(map.getTemplate(template_index)->loadTemplateFile());
			QCOMPARE(temp->getTemplateState(), Template::Loaded);

			template_track_center = center(temp);
		}

		GdalManager().setFormatEnabled(GdalManager::GPX, true);
		QPointF ogr_template_center;
		{
			Map map;
			MapView view{ &map };
			QVERIFY(map.loadFrom(map_file, &view));

			QVERIFY(map.getNumTemplates() > template_index);
			auto const* temp = map.getTemplate(template_index);
			QCOMPARE(temp->getTemplateType(), "OgrTemplate");
			QCOMPARE(temp->getTemplateState(), Template::Unloaded);
			QVERIFY(map.getTemplate(template_index)->loadTemplateFile());
			QCOMPARE(temp->getTemplateState(), Template::Loaded);

			ogr_template_center = center(temp);
		}
		
#if !defined(ACCEPT_USE_OF_DEPRECATED_PROJ_API_H) || PJ_VERSION >= 600
		QEXPECT_FAIL("TemplateTrack NAD83", "Unsupported WGS 84 -> NAD 83 transformation", Continue);
		QEXPECT_FAIL("OgrTemplate NAD83", "Unsupported WGS 84 -> NAD 83 transformation", Continue);
#endif
		if (QLineF(ogr_template_center, template_track_center).length() > 0.1) // 40 cm
			QCOMPARE(ogr_template_center, template_track_center);
		else
			QVERIFY2(true, "Centers do match");
	}
#endif
	
	void templateRotationTest_data()
	{
		QTest::addColumn<QString>("map_file");
		QTest::addColumn<int>("template_index");

		QTest::newRow("TemplateImage world file")  << QStringLiteral("testdata:templates/world-file.xmap")     << 0;
		QTest::newRow("TemplateTrack georef")      << QStringLiteral("testdata:templates/template-track.xmap") << 0;
		QTest::newRow("TemplateTrack non-georef")  << QStringLiteral("testdata:templates/template-track.xmap") << 1;
#ifdef MAPPER_USE_GDAL
		QTest::newRow("GdalImage geotiff")         << QStringLiteral("testdata:templates/geotiff.xmap")        << 0;
		QTest::newRow("OgrTemplate basic")         << QStringLiteral("testdata:templates/ogr-template.xmap")   << 0;
		QTest::newRow("OgrTemplate compatibility") << QStringLiteral("testdata:templates/ogr-template.xmap")   << 1;
#endif
	}
	
	void templateRotationTest()
	{
#ifdef MAPPER_USE_GDAL
		GdalManager().setFormatEnabled(GdalManager::GPX, false);
#endif
		
		QFETCH(QString, map_file);
		QFETCH(int, template_index);
		
		Map map;
		MapView view{ &map };
		QVERIFY(map.loadFrom(map_file, &view));
		
		auto const& georef = map.getGeoreferencing();
		QCOMPARE(georef.getState(), Georeferencing::Geospatial);
		QVERIFY(std::abs(georef.getGrivation()) >= 0.01);
		
		QVERIFY(map.getNumTemplates() > template_index);
		auto const* temp = map.getTemplate(template_index);
		QCOMPARE(temp->getTemplateState(), Template::Unloaded);
		
		// A clone of temp which is loaded before map rotation
		auto* loaded_clone = temp->duplicate();
		map.addTemplate(map.getNumTemplates(), std::unique_ptr<Template>(loaded_clone));
		if (loaded_clone->getTemplateState() != Template::Loaded)
			QVERIFY(loaded_clone->loadTemplateFile());
		QCOMPARE(loaded_clone->getTemplateState(), Template::Loaded);
		
		// A clone of temp which is loaded and unloaded before map rotation
		auto* reloaded_clone = temp->duplicate();
		map.addTemplate(map.getNumTemplates(), std::unique_ptr<Template>(reloaded_clone));
		if (reloaded_clone->getTemplateState() != Template::Loaded)
			QVERIFY(reloaded_clone->loadTemplateFile());
		reloaded_clone->unloadTemplateFile();
		QCOMPARE(reloaded_clone->getTemplateState(), Template::Unloaded);
		
		auto initially_georeferenced = temp->isTemplateGeoreferenced();
		auto initial_rotation = temp->getTemplateRotation();  // radians
		if (initially_georeferenced)
			QCOMPARE(initial_rotation, 0.0);
		
		// ROTATION
		auto const ref_point = map.getGeoreferencing().getMapRefPoint();
		map.rotateMap(0.1, ref_point, true, true, true);  // radians
		
		if (!initially_georeferenced)
			QCOMPARE(temp->getTemplateRotation(), initial_rotation + 0.1);  // radians
		
		// TEMPLATE LOADING
		QVERIFY(map.getTemplate(template_index)->loadTemplateFile());
		QCOMPARE(temp->getTemplateState(), Template::Loaded);
		
		QVERIFY(reloaded_clone->loadTemplateFile());
		QCOMPARE(reloaded_clone->getTemplateState(), Template::Loaded);
		
		// The template rectangle is in the interior of the map figure.
		auto const expected_center = map.calculateExtent().center();
		// Template instance loaded only after configuration
		if (QLineF(center(temp), expected_center).length() > 0.5)
			QCOMPARE(center(temp), expected_center);
		else
			QVERIFY2(true, "Centers do match.");
		
		// Template instance loaded before rotation
		if (QLineF(center(loaded_clone), expected_center).length() > 0.5)
			QCOMPARE(center(loaded_clone), expected_center);
		else
			QVERIFY2(true, "Centers do match.");
		
		// Template instance loaded+unloaded before, and loaded again after, rotation
		if (QLineF(center(reloaded_clone), expected_center).length() > 0.5)
			QCOMPARE(center(reloaded_clone), expected_center);
		else
			QVERIFY2(true, "Centers do match.");
	}
	
	void asyncLoadingTest_data()
	{
		QTest::addColumn<QString>("map_file");
		QTest::addColumn<int>("template_index");
		
		QTest::newRow("TemplateImage")             << QStringLiteral("testdata:templates/world-file.xmap")     << 0;
		QTest::newRow("TemplateTrack georef")      << QStringLiteral("testdata:templates/template-track.xmap") << 0;
		QTest::newRow("TemplateTrack non-georef")  << QStringLiteral("testdata:templates/template-track.xmap") << 1;
#ifdef MAPPER_USE_GDAL
		QTest::newRow("OgrTemplate basic")         << QStringLiteral("testdata:templates/ogr-template.xmap")   << 0;
		QTest::newRow("OgrTemplate compatibility") << QStringLiteral("testdata:templates/ogr-template.xmap")   << 1;
#endif
	}
	
	void asyncLoadingTest()
	{
#ifdef MAPPER_USE_GDAL
		GdalManager().setFormatEnabled(GdalManager::GPX, false);
#endif
		
		QFETCH(QString, map_file);
		QFETCH(int, template_index);
		
		Map map;
		MapView view{ &map };
		QVERIFY(map.loadFrom(map_file, &view));
		QVERIFY(map.getNumTemplates() > template_index);
		
		auto* temp = map.getTemplate(template_index);
		QCOMPARE(temp->getTemplateState(), Template::Unloaded);
		view.setTemplateVisibility(temp, TemplateVisibility{1, true});
		
		QStringList messages;
		messages.reserve(10);
		map.loadTemplateFilesAsync(view, [&messages](const QString& msg){
			messages.push_back(msg);
		});
		QSignalSpy(temp, &Template::templateStateChanged).wait();
		QCOMPARE(temp->getTemplateState(), Template::Loaded);
		QVERIFY(messages.size() >= 2);
		QVERIFY(!messages.front().isEmpty());
		QVERIFY(messages.back().isEmpty());
	}
	
	void templateTableModelTest()
	{
		Map map;
		MapView view{ &map };
		QVERIFY(map.loadFrom(QStringLiteral("testdata:templates/world-file.xmap"), &view));
		QCOMPARE(map.getNumTemplates(), 1);
		
		TemplateTableModel model(map, view);
		QCOMPARE(model.rowCount(), 2);
		
		// Single template, in background
		QCOMPARE(map.getFirstFrontTemplate(), 1);
		QCOMPARE(model.mapRow(map.getFirstFrontTemplate()), 0);
		QCOMPARE(model.posFromRow(0), -1);
		QCOMPARE(model.rowFromPos(0), 1);
		QCOMPARE(model.posFromRow(1), 0);
		// Three possible calls in onTemplateAboutToBeAdded
		QCOMPARE(model.insertionRowFromPos(0, true), 2);
		QCOMPARE(model.insertionRowFromPos(1, true), 1);
		QCOMPARE(model.insertionRowFromPos(1, false), 0);
		
		// Single template, in foreground
		map.setFirstFrontTemplate(0);
		QCOMPARE(model.mapRow(map.getFirstFrontTemplate()), 1);
		QCOMPARE(model.posFromRow(1), -1);
		QCOMPARE(model.rowFromPos(0), 0);
		QCOMPARE(model.posFromRow(0), 0);
		// Three possible calls in onTemplateAboutToBeAdded
		QCOMPARE(model.insertionRowFromPos(0, true), 2);
		QCOMPARE(model.insertionRowFromPos(0, false), 1);
		QCOMPARE(model.insertionRowFromPos(1, false), 0);
	}
	
};



/*
 * We don't need a real GUI window.
 * 
 * But we discovered QTBUG-58768 macOS: Crash when using QPrinter
 * while running with "minimal" platform plugin.
 */
#ifndef Q_OS_MACOS
namespace  {
	auto Q_DECL_UNUSED qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");  // clazy:exclude=non-pod-global-static
}
#endif


QTEST_MAIN(TemplateTest)
#include "template_t.moc"  // IWYU pragma: keep
