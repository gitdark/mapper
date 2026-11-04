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

#include "wms_template_dialog.h"

#include <algorithm>

#include <Qt>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPointF>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>
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

	// Server URL: editable, with the history of successfully used URLs.
	// Note: URLs may contain access tokens which are stored in plain text.
	url_combo = new QComboBox();
	url_combo->setEditable(true);
	url_combo->setInsertPolicy(QComboBox::NoInsert);
	url_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	url_combo->setMinimumContentsLength(30);
	url_combo->setCompleter(nullptr);  // The popup shows the history.
	url_edit = url_combo->lineEdit();
	url_edit->setClearButtonEnabled(true);
	url_edit->setPlaceholderText(QStringLiteral("https://example.com/wms"));
	{
		auto history = settings.value(settingsKey("serverUrls")).toStringList();
		if (settings.contains(settingsKey("serverUrl")))
		{
			// Migrate from the single last URL
			history = updateWmsUrlHistory(history, settings.value(settingsKey("serverUrl")).toString());
			settings.setValue(settingsKey("serverUrls"), history);
			settings.remove(settingsKey("serverUrl"));
		}
		setUrlHistory(history);
	}
	load_button = new QPushButton(tr("Load layers"));
	auto* url_layout = new QHBoxLayout();
	url_layout->addWidget(url_combo, 1);
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

	transparent_check = new QCheckBox(tr("Transparent background"));
	transparent_check->setToolTip(tr("Request PNG images with a transparent background, "
	                                 "for overlay layers such as buildings or parcels. "
	                                 "For aerial imagery, opaque JPEG images are smaller."));
	transparent_check->setChecked(settings.value(settingsKey("transparent"), false).toBool());
	
	info_label = new QLabel();
	info_label->setWordWrap(true);

	credential_warning = new QWidget();
	{
		auto* icon = new QLabel();
		auto const size = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
		icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, this).pixmap(size, size));
		icon->setAlignment(Qt::AlignTop);
		credential_warning_label = new QLabel();
		credential_warning_label->setWordWrap(true);
		auto* warning_layout = new QHBoxLayout(credential_warning);
		warning_layout->setContentsMargins(0, 0, 0, 0);
		warning_layout->addWidget(icon);
		warning_layout->addWidget(credential_warning_label, 1);
	}
	credential_warning->setVisible(false);

	button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	button_box->button(QDialogButtonBox::Ok)->setText(tr("Add template..."));

	auto* form = new QFormLayout();
	form->addRow(tr("Server URL:"), url_layout);
	form->addRow(tr("Layer:"), layer_list);
	form->addRow(tr("Area:"), area_combo);
	form->addRow(tr("Resolution:"), resolution_edit);
	form->addRow({}, transparent_check);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(info_label);
	layout->addWidget(credential_warning);
	layout->addWidget(button_box);

	connect(load_button, &QPushButton::clicked, this, &WmsTemplateDialog::loadLayers);
	// Choosing a history entry from the popup loads its layers, and so
	// does Enter. (With NoInsert and without completer, the combo box does
	// not emit activated() on Enter, so the layers are loaded only once.)
	connect(url_combo, QOverload<int>::of(&QComboBox::activated), this, &WmsTemplateDialog::loadLayers);
	connect(url_edit, &QLineEdit::returnPressed, this, &WmsTemplateDialog::loadLayers);
	connect(layer_list, &QListWidget::currentRowChanged, this, &WmsTemplateDialog::updateDescription);
	connect(layer_list, &QListWidget::currentRowChanged, this, &WmsTemplateDialog::updateCredentialWarning);
	connect(url_edit, &QLineEdit::textChanged, this, &WmsTemplateDialog::updateCredentialWarning);
	connect(area_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &WmsTemplateDialog::updateDescription);
	connect(resolution_edit, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &WmsTemplateDialog::updateDescription);
	connect(transparent_check, &QCheckBox::toggled, this, &WmsTemplateDialog::updateDescription);
	connect(button_box, &QDialogButtonBox::accepted, this, &WmsTemplateDialog::accept);
	connect(button_box, &QDialogButtonBox::rejected, this, &WmsTemplateDialog::reject);

	if (map->getGeoreferencing().getState() != Georeferencing::Geospatial)
	{
		for (auto* widget : std::initializer_list<QWidget*>{ url_combo, load_button, layer_list, area_combo, resolution_edit, transparent_check })
			widget->setEnabled(false);
		info_label->setText(tr("The map must be georeferenced in order to add a WMS layer."));
	}

	updateDescription();
	updateCredentialWarning();
	resize(560, 480);
}

WmsTemplateDialog::~WmsTemplateDialog() = default;


void WmsTemplateDialog::keyPressEvent(QKeyEvent* event)
{
	// Enter in the URL field loads the layers (via returnPressed).
	// It must not also trigger the default button, i.e. accept the dialog.
	if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
	    && url_edit->hasFocus())
	{
		event->accept();
		return;
	}
	QDialog::keyPressEvent(event);
}


void WmsTemplateDialog::setUrlHistory(const QStringList& history)
{
	// Keep the current text, e.g. a URL which is not yet in the history.
	auto const text = url_combo->count() ? url_edit->text() : history.value(0);
	QSignalBlocker blocker(url_combo);
	url_combo->clear();
	url_combo->addItems(history);
	auto const index = url_combo->findText(normalizedWmsUrl(text));
	if (index >= 0)
		url_combo->setCurrentIndex(index);
	url_edit->setText(index >= 0 ? url_combo->itemText(index) : text);
}


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
		QSettings settings;
		auto const history = updateWmsUrlHistory(settings.value(settingsKey("serverUrls")).toStringList(), url);
		settings.setValue(settingsKey("serverUrls"), history);
		setUrlHistory(history);
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

	description = makeWmsDescription(layers[std::size_t(row)].dataset_name, georef,
	                                 selectedArea(), resolution_edit->value(), /* use_cache */ true,
	                                 transparent_check->isChecked());
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


void WmsTemplateDialog::updateCredentialWarning()
{
	// The URL in the field, and the GetMap URL of the selected layer which
	// will be written to the template file.
	auto names = wmsCredentialParameters(url_edit->text());
	auto const row = layer_list->currentRow();
	if (row >= 0 && row < int(layers.size()))
	{
		auto const more = wmsCredentialParameters(layers[std::size_t(row)].dataset_name);
		for (auto const& name : more)
		{
			if (!names.contains(name, Qt::CaseInsensitive))
				names.push_back(name);
		}
	}
	
	credential_warning->setVisible(!names.isEmpty());
	if (names.isEmpty())
		return;
	
	credential_warning_label->setText(
	    tr("The URL contains access credentials (%1). They are saved in plain text "
	       "in the template file and in the list of recent server URLs. "
	       "Anyone who receives the template file can use them.")
	    .arg(names.join(QLatin1String(", "))));
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
	QSettings().setValue(settingsKey("transparent"), transparent_check->isChecked());
	template_path = path;
	QDialog::accept();
}


}  // namespace OpenOrienteering
