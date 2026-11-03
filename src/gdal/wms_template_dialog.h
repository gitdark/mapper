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

#ifndef OPENORIENTEERING_WMS_TEMPLATE_DIALOG_H
#define OPENORIENTEERING_WMS_TEMPLATE_DIALOG_H

#include <vector>

#include <QDialog>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QStringList>

#include "gdal/wms_service.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QWidget;

namespace OpenOrienteering {

class MapEditorController;


/**
 * A dialog for adding a layer from a WMS server as a template.
 *
 * The dialog writes a GDAL WMS service description file (.xml) for a
 * particular area and resolution. This file can be opened as a regular
 * GDAL raster template.
 */
class WmsTemplateDialog : public QDialog
{
Q_OBJECT
public:
	/// Maximum number of pixels of the generated image.
	static constexpr double max_pixels = 100'000'000;

	WmsTemplateDialog(QWidget* parent, MapEditorController& controller);
	~WmsTemplateDialog() override;

	/// The path of the service description file written on accept.
	QString templatePath() const { return template_path; }

	void accept() override;

protected:
	void keyPressEvent(QKeyEvent* event) override;

private:
	void loadLayers();
	void setUrlHistory(const QStringList& history);
	void updateDescription();
	QRectF selectedArea() const;
	QString defaultDirectory() const;

	MapEditorController& controller;
	std::vector<WmsLayer> layers;
	WmsDescription description;
	QRectF map_extent;
	QRectF visible_extent;
	QString template_path;

	QComboBox* url_combo;   ///< Editable, with the URL history
	QLineEdit* url_edit;    ///< The url_combo's line edit
	QPushButton* load_button;
	QListWidget* layer_list;
	QComboBox* area_combo;
	QDoubleSpinBox* resolution_edit;
	QCheckBox* transparent_check;
	QLabel* info_label;
	QDialogButtonBox* button_box;
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_WMS_TEMPLATE_DIALOG_H
