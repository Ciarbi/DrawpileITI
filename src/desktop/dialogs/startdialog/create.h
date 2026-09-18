// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DESKTOP_DIALOGS_STARTDIALOG_CREATE_H
#define DESKTOP_DIALOGS_STARTDIALOG_CREATE_H

#include "desktop/dialogs/startdialog/page.h"
#include <QWidget>

class KisSliderSpinBox;
class QComboBox;
class QLabel;
class QToolButton;

namespace color_widgets {
class ColorPreview;
}

namespace dialogs {
namespace startdialog {

class Create final : public Page {
	Q_OBJECT
public:
	Create(QWidget *parent = nullptr);
	void activate() final override;
	void accept() final override;

	static color_widgets::ColorPreview *
	makeBackgroundPreview(const QColor &color);

signals:
	void showButtons();
	void enableCreate(bool enabled);
	void create(const QSize &size, const QColor &backgroundColor);

private slots:
	void updateCreateButton();
	void updatePresetCombo();
	void swapDimensions();
	void showColorPicker();

private:
	KisSliderSpinBox *m_widthSpinner;
	KisSliderSpinBox *m_heightSpinner;
	QComboBox *m_presetCombo;
	QToolButton *m_swapButton;
	bool m_customDimensions;
	bool m_applyingPreset;
	color_widgets::ColorPreview *m_backgroundPreview;
	QLabel *m_errorLabel;
};

}
}

#endif
