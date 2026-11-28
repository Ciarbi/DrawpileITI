// SPDX-License-Identifier: GPL-3.0-or-later
#include "libclient/brushes/brushpresetmodel.h"
#include "libclient/drawdance/ziparchive.h"
#include "libshared/util/paths.h"
#include <QBuffer>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <zlib.h>

namespace {

QByteArray be32(quint32 value)
{
	return QByteArray::fromHex(QByteArray::number(value, 16).rightJustified(8, '0'));
}

QByteArray makeKpp(const QByteArray &presetXml, bool compressed = false)
{
	QImage image(1, 1, QImage::Format_ARGB32);
	image.fill(Qt::red);
	QBuffer buffer;
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "PNG");
	QByteArray png = buffer.data();

	QByteArray text("preset");
	text += '\0';
	QByteArray type("tEXt");
	if(compressed) {
		type = QByteArray("zTXt");
		text += '\0';
		uLongf length = compressBound(uLong(presetXml.size()));
		QByteArray compressedData(int(length), Qt::Uninitialized);
		if(compress2(
				   reinterpret_cast<Bytef *>(compressedData.data()), &length,
				   reinterpret_cast<const Bytef *>(presetXml.constData()),
				   uLong(presetXml.size()), Z_DEFAULT_COMPRESSION) != Z_OK) {
			return QByteArray();
		}
		compressedData.truncate(int(length));
		text += compressedData;
	} else {
		text += presetXml;
	}

	quint32 crc = crc32(0, reinterpret_cast<const Bytef *>(type.constData()), 4);
	crc = crc32(
		crc, reinterpret_cast<const Bytef *>(text.constData()),
		uInt(text.size()));
	QByteArray chunk = be32(uInt(text.size())) + type + text + be32(crc);

	int iend = png.lastIndexOf(QByteArray("IEND"));
	if(iend < 4) {
		return QByteArray();
	}
	png.insert(iend - 4, chunk);
	return png;
}

QDomDocument makeLegacyDoc(const QString &settingsXml)
{
	QDomDocument doc;
	doc.setContent(
		QStringLiteral("<brush><name>Probe</name><engine>paintbrush</engine>")
			.append(settingsXml)
			.append(QStringLiteral("</brush>")));
	return doc;
}

}

class TestKritaBundle final : public QObject {
	Q_OBJECT

private slots:
	void testImportModernBundle()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		utils::paths::setDataPath(
			QStringLiteral(DP_BRUSH_ASSET_DIR));
		utils::paths::setWritablePath(dir.path());

		QString path = dir.filePath("probe.bundle");
		QByteArray manifest =
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
			"<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
			"<manifest:file-entry manifest:media-type=\"application/x-krita-resourcebundle\" manifest:full-path=\"/\"/>"
			"<manifest:file-entry manifest:media-type=\"paintoppresets\" manifest:full-path=\"paintoppresets/probe.kpp\"/>"
			"</manifest:manifest>";
		QByteArray presetXml =
			"<Preset name=\"Probe Brush\" paintopid=\"paintbrush\">"
			"<param type=\"string\" name=\"brush_definition\"><![CDATA["
			"<Brush type=\"auto_brush\" spacing=\"0.1\">"
			"<MaskGenerator type=\"circle\" id=\"soft\" softness_curve=\"0,0;1,1;\" diameter=\"42\"/>"
			"</Brush>]]></param>"
			"<param type=\"string\" name=\"SizeValue\"><![CDATA[1]]></param>"
			"<param type=\"string\" name=\"OpacityValue\"><![CDATA[1]]></param>"
			"<param type=\"string\" name=\"FlowValue\"><![CDATA[1]]></param>"
			"</Preset>";

		QByteArray kpp = makeKpp(presetXml);
		QVERIFY(!kpp.isEmpty());

		drawdance::ZipWriter zw{path};
		QVERIFY(!zw.isNull());
		QVERIFY(zw.addDir("META-INF"));
		QVERIFY(zw.addFile("META-INF/manifest.xml", manifest));
		QVERIFY(zw.addDir("paintoppresets"));
		QVERIFY(zw.addFile("paintoppresets/probe.kpp", kpp));
		QVERIFY(zw.finish());

		QCOMPARE(
			brushes::BrushPresetTagModel::detectBundleFormat(path),
			brushes::BundleFormat::Krita);

		brushes::BrushPresetTagModel model;
		brushes::BrushImportResult result = model.importKritaBundle(path);
		QVERIFY2(
			result.errors.isEmpty(),
			qPrintable(result.errors.join(QLatin1Char('\n'))));
		QCOMPARE(result.importedBrushCount, 1);
		QCOMPARE(result.importedTags.size(), 1);
		QCOMPARE(result.importedTags[0].name, QStringLiteral("probe"));
	}

	void testEngineTranslation()
	{
		QDomDocument doc = makeLegacyDoc(QStringLiteral(
			"<settings blend=\"svg:src-over\" paintmode=\"direct\" "
			"pixel=\"false\">"
			"<setting id=\"size\" dynamic=\"pressure\">42"
			"<curve><point x=\"0\" y=\"0\"/><point x=\"1\" y=\"1\"/>"
			"</curve></setting>"
			"<setting id=\"opacity\">0.75</setting>"
			"<setting id=\"hard\">0.25</setting>"
			"<setting id=\"spacing\">12.5</setting>"
			"<setting id=\"jitter\" dynamic=\"velocity\">20</setting>"
			"<setting id=\"smudge\" dynamic=\"pressure\">0.4</setting>"
			"</settings>"));

		QVERIFY(!doc.documentElement().isNull());
		QJsonObject classic =
			brushes::KritaBrushConverter::presetToClassicSettings(
				doc, QStringLiteral("paintbrush"));
		QCOMPARE(classic[QStringLiteral("size")].toDouble(), 42.0);
		QCOMPARE(classic[QStringLiteral("opacity")].toDouble(), 0.75);
		QCOMPARE(classic[QStringLiteral("hard")].toDouble(), 0.25);
		QCOMPARE(classic[QStringLiteral("spacing")].toDouble(), 12.5);
		QCOMPARE(classic[QStringLiteral("jitter")].toDouble(), 20.0);
		QCOMPARE(classic[QStringLiteral("smudge")].toDouble(), 0.4);
		QVERIFY(classic[QStringLiteral("sizep")].toBool());
		QVERIFY(classic[QStringLiteral("jitterv")].toBool());
		QVERIFY(classic[QStringLiteral("smudgep")].toBool());
		QCOMPARE(
			classic[QStringLiteral("blend")].toString(),
			QStringLiteral("svg:src-over"));
		QCOMPARE(
			classic[QStringLiteral("sizecurve")].toString(),
			QStringLiteral("0,0;1,1;"));

		QJsonObject active =
			brushes::KritaBrushConverter::presetToActiveBrushJson(
				doc, QStringLiteral("paintbrush"));
		QCOMPARE(
			active[QStringLiteral("type")].toString(),
			QStringLiteral("dp-classic"));
		brushes::ActiveBrush brush = brushes::ActiveBrush::fromJson(active);
		QCOMPARE(brush.activeType(), brushes::ActiveBrush::CLASSIC);
	}

	void testMyPaintTranslation()
	{
		QDomDocument doc = makeLegacyDoc(QStringLiteral(
			"<settings blend=\"svg:src-over\" paintmode=\"direct\" "
			"pixel=\"false\">"
			"<setting id=\"size\">28</setting>"
			"<setting id=\"opacity\">0.6</setting>"
			"<setting id=\"hard\">0.5</setting>"
			"<setting id=\"spacing\">15</setting>"
			"<setting id=\"smudge\" dynamic=\"pressure\">0.7</setting>"
			"</settings>"));
		QVERIFY(!doc.documentElement().isNull());
		QVERIFY(!brushes::KritaBrushConverter::shouldUseClassic(
			QStringLiteral("colorsmudge"), doc));

		QJsonObject active =
			brushes::KritaBrushConverter::presetToActiveBrushJson(
				doc, QStringLiteral("colorsmudge"));
		QCOMPARE(
			active[QStringLiteral("type")].toString(),
			QStringLiteral("dp-mypaint"));
		QJsonObject settings = active[QStringLiteral("settings")].toObject();
		QJsonObject mapping = settings[QStringLiteral("mapping")].toObject();
		QVERIFY(mapping.contains(QStringLiteral("radius_logarithmic")));
		QVERIFY(mapping.contains(QStringLiteral("opaque")));
		QVERIFY(mapping.contains(QStringLiteral("hardness")));
		QVERIFY(mapping.contains(QStringLiteral("opaque_multiply")));
		QVERIFY(mapping.contains(QStringLiteral("smudge")));
		QVERIFY(
			mapping[QStringLiteral("smudge")]
				.toObject()[QStringLiteral("base_value")]
				.toDouble() > 0.0);
		brushes::ActiveBrush brush = brushes::ActiveBrush::fromJson(active);
		QCOMPARE(brush.activeType(), brushes::ActiveBrush::MYPAINT);
	}

	void testImportCompressedEngineBundle()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		utils::paths::setDataPath(QStringLiteral(DP_BRUSH_ASSET_DIR));
		utils::paths::setWritablePath(dir.path());

		QString path = dir.filePath("compressed.bundle");
		QByteArray manifest =
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
			"<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
			"<manifest:file-entry manifest:media-type=\"application/x-krita-resourcebundle\" manifest:full-path=\"/\"/>"
			"<manifest:file-entry manifest:media-type=\"paintoppresets\" manifest:full-path=\"paintoppresets/mix.kpp\"/>"
			"</manifest:manifest>";
		QByteArray presetXml =
			"<Preset name=\"Compressed Mix\" paintopid=\"colorsmudge\">"
			"<param type=\"string\" name=\"SmudgeRateValue\"><![CDATA[0.7]]></param>"
			"<param type=\"string\" name=\"ColorRateValue\"><![CDATA[1]]></param>"
			"<param type=\"string\" name=\"OpacityValue\"><![CDATA[0.8]]></param>"
			"<param type=\"string\" name=\"SizeValue\"><![CDATA[1]]></param>"
			"<param type=\"string\" name=\"SmudgeRateSensor\"><![CDATA[<!DOCTYPE params> <params id=\"pressure\"> <curve>0,0;1,1;</curve> </params> ]]></param>"
			"</Preset>";
		QByteArray kpp = makeKpp(presetXml, true);
		QVERIFY(!kpp.isEmpty());
		QVERIFY(kpp.contains("zTXt"));

		drawdance::ZipWriter zw{path};
		QVERIFY(!zw.isNull());
		QVERIFY(zw.addDir("META-INF"));
		QVERIFY(zw.addFile("META-INF/manifest.xml", manifest));
		QVERIFY(zw.addDir("paintoppresets"));
		QVERIFY(zw.addFile("paintoppresets/mix.kpp", kpp));
		QVERIFY(zw.finish());

		brushes::BrushPresetTagModel model;
		brushes::BrushImportResult result = model.importKritaBundle(path);
		QVERIFY2(
			result.errors.isEmpty(),
			qPrintable(result.errors.join(QLatin1Char('\n'))));
		QCOMPARE(result.importedBrushCount, 1);
		QCOMPARE(result.importedTags.size(), 1);

		brushes::BrushPresetModel *presets = model.presetModel();
		QModelIndex index = presets->indexForTagId(result.importedTags[0].id, 0);
		QVERIFY(index.isValid());
		int presetId = presets->data(index, brushes::BrushPresetModel::IdRole).toInt();
		std::optional<brushes::Preset> preset =
			presets->searchPresetBrushData(presetId);
		QVERIFY(preset.has_value());
		brushes::ActiveBrush brush = preset->originalConstBrushLoad();
		QCOMPARE(brush.activeType(), brushes::ActiveBrush::MYPAINT);
	}
};

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	TestKritaBundle test;
	return QTest::qExec(&test, argc, argv);
}

#include "kritabundle.moc"
