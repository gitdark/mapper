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

#include "wms_template_dialog.h"

#include <algorithm>

#include <Qt>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPointF>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QVariant>

#include "core/georeferencing.h"
#include "core/map.h"
#include "core/map_coord.h"
#include "gui/file_dialog.h"
#include "gui/main_window.h"
#include "gui/map/map_editor.h"
#include "gui/map/map_widget.h"


namespace OpenOrienteering {

namespace {

QString settingsKey(const char* key)
{
	return QLatin1String("Templates/WMS/") + QLatin1String(key);
}

}  // namespace


WmsTemplateDialog::WmsTemplateDialog(QWidget* parent, MapEditorController& controller)
: QDialog(parent)
, controller(controller)
{
	setWindowTitle(tr("Add WMS layer"));

	QSettings settings;

	auto* map = controller.getMap();
	map_extent = map->calculateExtent();
	if (auto* map_widget = controller.getMainWidget())
	{
		auto const rect = map_widget->rect();
		auto const corners = { rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight() };
		// Note: QRectF::united() ignores null (zero size) rectangles.
		auto const first = QPointF(map_widget->viewportToMapF(rect.topLeft()));
		auto left = first.x(), right = first.x(), top = first.y(), bottom = first.y();
		for (auto const& corner : corners)
		{
			auto const pos = QPointF(map_widget->viewportToMapF(corner));
			left = std::min(left, pos.x());
			right = std::max(right, pos.x());
			top = std::min(top, pos.y());
			bottom = std::max(bottom, pos.y());
		}
		visible_extent = QRectF(QPointF(left, top), QPointF(right, bottom));
	}

	url_edit = new QLineEdit(settings.value(settingsKey("serverUrl")).toString());
	url_edit->setPlaceholderText(QStringLiteral("https://example.com/wms"));
	load_button = new QPushButton(tr("Load layers"));
	auto* url_layout = new QHBoxLayout();
	url_layout->addWidget(url_edit, 1);
	url_layout->addWidget(load_button);

	layer_list = new QListWidget();

	area_combo = new QComboBox();
	area_combo->addItem(tr("Map content"));
	area_combo->addItem(tr("Visible area"));
	if (map_extent.isEmpty())
	{
		area_combo->setCurrentIndex(1);
		area_combo->setItemData(0, false, Qt::UserRole - 1);  // disable item
	}

	resolution_edit = new QDoubleSpinBox();
	resolution_edit->setRange(0.05, 1000.0);
	resolution_edit->setDecimals(2);
	resolution_edit->setSingleStep(0.25);
	resolution_edit->setSuffix(tr(" m/px"));
	resolution_edit->setValue(settings.value(settingsKey("resolution"), 1.0).toDouble());

	info_label = new QLabel();
	info_label->setWordWrap(true);

	button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	button_box->button(QDialogButtonBox::Ok)->setText(tr("Add template..."));

	auto* form = new QFormLayout();
	form->addRow(tr("Server URL:"), url_layout);
	form->addRow(tr("Layer:"), layer_list);
	form->addRow(tr("Area:"), area_combo);
	form->addRow(tr("Resolution:"), resolution_edit);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(info_label);
	layout->addWidget(button_box);

	connect(load_button, &QPushButton::clicked, this, &WmsTemplateDialog::loadLayers);
	connect(url_edit, &QLineEdit::returnPressed, this, &WmsTemplateDialog::loadLayers);
	connect(layer_list, &QListWidget::currentRowChanged, this, &WmsTemplateDialog::updateDescription);
	connect(area_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &WmsTemplateDialog::updateDescription);
	connect(resolution_edit, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &WmsTemplateDialog::updateDescription);
	connect(button_box, &QDialogButtonBox::accepted, this, &WmsTemplateDialog::accept);
	connect(button_box, &QDialogButtonBox::rejected, this, &WmsTemplateDialog::reject);

	if (map->getGeoreferencing().getState() != Georeferencing::Geospatial)
	{
		for (auto* widget : std::initializer_list<QWidget*>{ url_edit, load_button, layer_list, area_combo, resolution_edit })
			widget->setEnabled(false);
		info_label->setText(tr("The map must be georeferenced in order to add a WMS layer."));
	}

	updateDescription();
	resize(560, 480);
}

WmsTemplateDialog::~WmsTemplateDialog() = default;


void WmsTemplateDialog::loadLayers()
{
	auto const url = url_edit->text().trimmed();
	if (url.isEmpty())
		return;

	layers.clear();
	layer_list->clear();
	info_label->setText(tr("Loading layers..."));

	QString error;
	QApplication::setOverrideCursor(Qt::WaitCursor);
	layers = queryWmsLayers(url, error);
	QApplication::restoreOverrideCursor();

	if (!error.isEmpty())
	{
		QMessageBox::warning(this, tr("Error"), tr("Failed to load layers from %1:\n%2").arg(url, error));
	}
	else
	{
		QSettings().setValue(settingsKey("serverUrl"), url);
	}

	for (auto const& layer : layers)
	{
		auto* item = new QListWidgetItem(layer.title, layer_list);
		item->setToolTip(layer.dataset_name);
	}
	if (!layers.empty())
		layer_list->setCurrentRow(0);

	updateDescription();
}


QRectF WmsTemplateDialog::selectedArea() const
{
	return area_combo->currentIndex() == 0 ? map_extent : visible_extent;
}


void WmsTemplateDialog::updateDescription()
{
	auto* ok_button = button_box->button(QDialogButtonBox::Ok);
	ok_button->setEnabled(false);
	description = {};

	auto const& georef = controller.getMap()->getGeoreferencing();
	if (georef.getState() != Georeferencing::Geospatial)
		return;

	auto const row = layer_list->currentRow();
	if (row < 0 || row >= int(layers.size()))
	{
		info_label->setText(tr("Enter the URL of a WMS server and load its layers."));
		return;
	}

	// Not auto: with QStringBuilder, that would dangle on the temporary.
	QString const cache_path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
	                           + QLatin1String("/wms");
	description = makeWmsDescription(layers[std::size_t(row)].dataset_name, georef,
	                                 selectedArea(), resolution_edit->value(), cache_path);
	if (!description.isValid())
	{
		info_label->setText(tr("Error: %1").arg(description.error));
		return;
	}

	auto const pixels = double(description.size.width()) * description.size.height();
	auto const megabytes = pixels * 4 / 1024 / 1024;
	auto text = tr("Image size: %1 x %2 pixels (%3 MB), coordinate reference system: %4")
	            .arg(description.size.width())
	            .arg(description.size.height())
	            .arg(megabytes, 0, 'f', 1)
	            .arg(description.srs);
	if (pixels > max_pixels)
	{
		text += QLatin1Char('\n') + tr("The image is too large. Increase the resolution value or choose a smaller area.");
		info_label->setText(text);
		return;
	}

	info_label->setText(text);
	ok_button->setEnabled(true);
}


QString WmsTemplateDialog::defaultDirectory() const
{
	auto const map_path = controller.getWindow() ? controller.getWindow()->currentPath() : QString();
	if (!map_path.isEmpty())
		return QFileInfo(map_path).absolutePath();
	return QSettings().value(QStringLiteral("templateFileDirectory"), QDir::homePath()).toString();
}


void WmsTemplateDialog::accept()
{
	if (!description.isValid())
		return;

	auto const row = layer_list->currentRow();
	auto name = layers[std::size_t(row)].title;
	name.replace(QRegularExpression(QStringLiteral("[^\\w\\- ]+")), QStringLiteral("_"));
	name = name.trimmed();
	if (name.isEmpty())
		name = QStringLiteral("wms");

	auto path = FileDialog::getSaveFileName(this, tr("Save WMS service description"),
	                                        QDir(defaultDirectory()).absoluteFilePath(name + QLatin1String(".xml")),
	                                        tr("GDAL WMS service description (*.xml)"));
	if (path.isEmpty())
		return;
	if (!path.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
		path += QLatin1String(".xml");

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
	    || file.write(description.xml) != description.xml.size()
	    || !file.flush())
	{
		QMessageBox::warning(this, tr("Error"), tr("Cannot save file\n%1:\n%2").arg(path, file.errorString()));
		return;
	}
	file.close();

	QSettings().setValue(settingsKey("resolution"), resolution_edit->value());
	template_path = path;
	QDialog::accept();
}


}  // namespace OpenOrienteering
