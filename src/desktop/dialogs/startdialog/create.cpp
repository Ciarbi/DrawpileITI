// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/dialogs/startdialog/create.h"
#include "desktop/dialogs/colordialog.h"
#include "desktop/dialogs/resizedialog.h"
#include "desktop/main.h"
#include "desktop/widgets/kis_slider_spin_box.h"
#include "libclient/config/config.h"
#include "libclient/utils/images.h"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSignalBlocker>
#include <QToolButton>
#include <QtColorWidgets/ColorPreview>

using color_widgets::ColorDialog;
using color_widgets::ColorPreview;

namespace dialogs {
namespace startdialog {

namespace {

struct PresetSize {
	const char *name;
	int width;
	int height;
};

static const PresetSize presetSizes[] = {
	{"A4 150ppi", 1240, 1754},
	{"A4 300ppi", 2480, 3508},
	{"A3 150ppi", 1123, 1587},
	{"A3 300ppi", 3508, 4961},
	{"HD", 1280, 720},
	{"Full HD", 1920, 1080},
	{"2K", 2048, 1080},
	{"4K UHD", 3840, 2160},
	{"8K UHD", 7680, 4320},
	{"Square", 512, 512},
	{"Square 1K", 1024, 1024},
	{"Square 2K", 2048, 2048},
	{"Square 4K", 4096, 4096},
};

}

Create::Create(QWidget *parent)
	: Page{parent}
{
	QFormLayout *layout = new QFormLayout;
	layout->setContentsMargins(0, 0, 0, 0);
	setLayout(layout);
	setMaximumWidth(600);

	m_presetCombo = new QComboBox;
	for(const PresetSize &preset : presetSizes) {
		m_presetCombo->addItem(QString::fromUtf8(preset.name));
	}
	QHBoxLayout *swapButtonLayout = new QHBoxLayout;
	m_swapButton = new QToolButton;
	m_swapButton->setIcon(QIcon::fromTheme(QStringLiteral("page-orientation")));
	m_swapButton->setToolTip(tr("Swap Orientation"));
	swapButtonLayout->addWidget(m_swapButton);

	QHBoxLayout *sizeLayout = new QHBoxLayout;
	sizeLayout->addWidget(m_presetCombo);
	sizeLayout->addLayout(swapButtonLayout);
	sizeLayout->addStretch(1);

	layout->addRow(tr("Size:"), sizeLayout);

	QHBoxLayout *widthLayout = new QHBoxLayout;
	layout->addRow(tr("Width:"), widthLayout);
	m_widthSpinner = new KisSliderSpinBox;
	m_widthSpinner->setIndeterminate(true);
	m_widthSpinner->setRange(1, 9999999);
	m_widthSpinner->setFastSliderStep(10);
	widthLayout->addWidget(m_widthSpinner);
	widthLayout->addWidget(new QLabel(tr("px")), 1);

	QHBoxLayout *heightLayout = new QHBoxLayout;
	layout->addRow(tr("Height:"), heightLayout);
	m_heightSpinner = new KisSliderSpinBox;
	m_heightSpinner->setIndeterminate(true);
	m_heightSpinner->setRange(1, 9999999);
	m_heightSpinner->setFastSliderStep(10);
	heightLayout->addWidget(m_heightSpinner);
	heightLayout->addWidget(new QLabel(tr("px")), 1);

	DrawpileApp &app = dpApp();
	QSize lastSize = app.safeNewCanvasSize();
	bool lastSizeValid =
		lastSize.isValid() && ResizeDialog::checkDimensions(
								  lastSize.width(), lastSize.height(), false);
	m_widthSpinner->setValue(lastSizeValid ? lastSize.width() : 1024);
	m_heightSpinner->setValue(lastSizeValid ? lastSize.height() : 1024);
	m_customDimensions = false;
	m_applyingPreset = false;

	int presetIndex = -1;
	for(size_t i = 0; i < sizeof(presetSizes) / sizeof(presetSizes[0]); ++i) {
		const PresetSize &preset = presetSizes[i];
		if(preset.width == m_widthSpinner->value() &&
		   preset.height == m_heightSpinner->value()) {
			presetIndex = int(i);
			break;
		}
	}
	if(presetIndex < 0) {
		m_widthSpinner->setValue(1024);
		m_heightSpinner->setValue(1024);
	}

	m_backgroundPreview =
		makeBackgroundPreview(app.config()->getNewCanvasBackColor());
	layout->addRow(tr("Background:"), m_backgroundPreview);

	m_errorLabel = new QLabel;
	m_errorLabel->setTextFormat(Qt::PlainText);
	m_errorLabel->setVisible(false);
	m_errorLabel->setWordWrap(true);
	layout->addRow(m_errorLabel);

	connect(
		m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int index) {
			bool hasCustom =
				m_presetCombo->count() > 0 &&
				m_presetCombo->itemText(0) == tr("Custom");
			int presetIdx = index - (hasCustom ? 1 : 0);
			if(presetIdx < 0) {
				return;
			}

			const PresetSize &preset = presetSizes[presetIdx];
			m_applyingPreset = true;
			m_customDimensions = false;
			m_widthSpinner->setValue(preset.width);
			m_heightSpinner->setValue(preset.height);
			m_applyingPreset = false;
			updatePresetCombo();
		});
	auto dimensionsChanged = [this](int) {
		if(!m_applyingPreset) {
			m_customDimensions = true;
			updatePresetCombo();
		}
		updateCreateButton();
	};
	connect(
		m_widthSpinner, QOverload<int>::of(&KisSliderSpinBox::valueChanged),
		this, dimensionsChanged);
	connect(
		m_heightSpinner, QOverload<int>::of(&KisSliderSpinBox::valueChanged),
		this, dimensionsChanged);
	connect(m_swapButton, &QToolButton::clicked, this, &Create::swapDimensions);
	connect(
		m_backgroundPreview, &ColorPreview::clicked, this,
		&Create::showColorPicker);
	updatePresetCombo();
	updateCreateButton();
}

void Create::activate()
{
	emit showButtons();
	updateCreateButton();
}

void Create::accept()
{
	QSize size{m_widthSpinner->value(), m_heightSpinner->value()};
	QColor backgroundColor = m_backgroundPreview->color();
	config::Config *cfg = dpAppConfig();
	cfg->setNewCanvasSize(size);
	cfg->setNewCanvasBackColor(backgroundColor);
	emit create(size, backgroundColor);
}

void Create::updatePresetCombo()
{
	int presetIndex = -1;
	QSize size{m_widthSpinner->value(), m_heightSpinner->value()};
	for(size_t i = 0; i < sizeof(presetSizes) / sizeof(presetSizes[0]); ++i) {
		const PresetSize &preset = presetSizes[i];
		if(preset.width == size.width() && preset.height == size.height()) {
			presetIndex = int(i);
			break;
		}
	}

	if(m_customDimensions) {
		if(m_presetCombo->count() == 0 ||
		   m_presetCombo->itemText(0) != tr("Custom")) {
			QSignalBlocker blocker(m_presetCombo);
			m_presetCombo->insertItem(0, tr("Custom"));
		}
	} else {
		if(m_presetCombo->count() > 0 &&
		   m_presetCombo->itemText(0) == tr("Custom")) {
			QSignalBlocker blocker(m_presetCombo);
			m_presetCombo->removeItem(0);
		}
	}

	QSignalBlocker blocker(m_presetCombo);
	m_presetCombo->setCurrentIndex(m_customDimensions
									   ? 0
									   : (presetIndex >= 0 ? presetIndex : 10));
}

void Create::swapDimensions()
{
	m_applyingPreset = true;
	int width = m_widthSpinner->value();
	m_widthSpinner->setValue(m_heightSpinner->value());
	m_heightSpinner->setValue(width);
	m_applyingPreset = false;
}

color_widgets::ColorPreview *Create::makeBackgroundPreview(const QColor &color)
{
	ColorPreview *backgroundPreview = new ColorPreview;
	backgroundPreview->setDisplayMode(ColorPreview::DisplayMode::AllAlpha);
	backgroundPreview->setToolTip(tr("Canvas background color"));
	backgroundPreview->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	backgroundPreview->setCursor(Qt::PointingHandCursor);
	backgroundPreview->setColor(color.isValid() ? color : Qt::white);
	return backgroundPreview;
}

void Create::updateCreateButton()
{
	QString error;
	bool ok = ResizeDialog::checkDimensions(
		m_widthSpinner->value(), m_heightSpinner->value(), false, &error);
	m_errorLabel->setText(error);
	m_errorLabel->setVisible(!ok);
	emit enableCreate(ok);
}

void Create::showColorPicker()
{
	ColorDialog *dlg = dialogs::newDeleteOnCloseColorDialog(
		m_backgroundPreview->color(), this);
	connect(
		dlg, &ColorDialog::colorSelected, m_backgroundPreview,
		&ColorPreview::setColor);
	dlg->show();
}

}
}
