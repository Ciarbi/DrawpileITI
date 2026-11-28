// SPDX-License-Identifier: GPL-3.0-or-later
#include "libclient/brushes/brushpresetmodel.h"
#include "libclient/brushes/enums.h"
#include "libclient/canvas/blendmodes.h"
#include "libclient/drawdance/compress.h"
#include "libclient/drawdance/ziparchive.h"
#include "libclient/utils/compressedtimer.h"
#include "libclient/utils/wasmpersistence.h"
#include "libshared/util/database.h"
#include "libshared/util/paths.h"
#include "libshared/util/qtcompat.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QTextStream>
#include <QVector>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <dpcommon/platform_qt.h>
#include <zlib.h>

namespace brushes {

static constexpr int ALL_ROW = 0;
static constexpr int UNTAGGED_ROW = 1;
static constexpr int HISTORY_ROW = 2;
static constexpr int TAG_OFFSET = 3;
static constexpr int ALL_ID = -1;
static constexpr int UNTAGGED_ID = -2;
static constexpr int HISTORY_ID = -3;

namespace {
struct CachedTag {
	int id;
	QString name;
};

struct CachedPreset : Preset {
	QSet<int> tagIds;
};

struct PresetChange {
	std::optional<QString> name;
	std::optional<QString> description;
	std::optional<LazyThumbnail> thumbnail;
	std::optional<LazyBrush> brush;
};

struct PresetHistoryChange {
	double timestamp;
};

struct PresetShortcutEntry {
	int id;
	QString name;

	bool operator==(const PresetShortcutEntry &other) const
	{
		return id == other.id && name == other.name;
	}

	bool operator<(const PresetShortcutEntry &other) const
	{
		return name.compare(other.name, Qt::CaseInsensitive) < 0;
	}
};

struct PresetShortcut {
	QVector<PresetShortcutEntry> entries;

	bool containsId(int id) const
	{
		for(const PresetShortcutEntry &entry : entries) {
			if(entry.id == id) {
				return true;
			}
		}
		return false;
	}

	QVector<int> ids() const
	{
		QVector<int> v;
		v.reserve(entries.size());
		for(const PresetShortcutEntry &entry : entries) {
			v.append(entry.id);
		}
		return v;
	}

	QStringList names() const
	{
		QStringList v;
		v.reserve(entries.size());
		for(const PresetShortcutEntry &entry : entries) {
			v.append(entry.name);
		}
		return v;
	}

	QString text(const QLocale &locale) const
	{
		return locale.createSeparatedList(names());
	}
};

struct DomParseResult {
	bool ok;
	QString errorMessage;
	int errorLine;
	int errorColumn;
};

// Qt 6.8 deprecated all setContent overloads that take error output
// parameters, so keep the version-specific parsing in one place.
static DomParseResult
parseDomXml(QDomDocument &doc, const QByteArray &xml, bool namespaceProcessing)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	QDomDocument::ParseOptions options =
		namespaceProcessing ? QDomDocument::ParseOption::UseNamespaceProcessing
							: QDomDocument::ParseOption::Default;
	QDomDocument::ParseResult result = doc.setContent(xml, options);
	return {
		static_cast<bool>(result), result.errorMessage,
		static_cast<int>(result.errorLine),
		static_cast<int>(result.errorColumn)};
#else
	QString errorMessage;
	int errorLine = 0;
	int errorColumn = 0;
	bool ok = doc.setContent(
		xml, namespaceProcessing, &errorMessage, &errorLine, &errorColumn);
	return {ok, errorMessage, errorLine, errorColumn};
#endif
}

using PresetShortcutMap = QHash<QKeySequence, PresetShortcut>;

static const QString KRITA_MANIFEST_NAMESPACE = QStringLiteral(
	"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0");

static bool isKritaPngData(const QByteArray &data)
{
	static const QByteArray signature = QByteArray::fromHex("89504e470d0a1a0a");
	return data.startsWith(signature);
}

static QByteArray uncompressZlibData(const QByteArray &compressed)
{
	uLongf capacity =
		static_cast<uLongf>(qMax<qint64>(4096, qint64(compressed.size()) * 4));
	for(int attempt = 0; attempt < 16; ++attempt) {
		QByteArray out(static_cast<int>(capacity), Qt::Uninitialized);
		uLongf length = capacity;
		int err = uncompress(
			reinterpret_cast<Bytef *>(out.data()), &length,
			reinterpret_cast<const Bytef *>(compressed.constData()),
			static_cast<uLong>(compressed.size()));
		if(err == Z_OK) {
			out.truncate(static_cast<int>(length));
			return out;
		}
		if(err != Z_BUF_ERROR) {
			return QByteArray();
		}
		capacity *= 2;
	}
	return QByteArray();
}

static QByteArray extractKritaPresetXml(const QByteArray &pngData)
{
	const int signatureLength = 8;
	int offset = signatureLength;
	while(offset + 12 <= pngData.size()) {
		const char *p = pngData.constData() + offset;
		quint32 chunkLength = (quint32(quint8(p[0])) << 24) |
			(quint32(quint8(p[1])) << 16) | (quint32(quint8(p[2])) << 8) |
			quint32(quint8(p[3]));
		QByteArray type = pngData.mid(offset + 4, 4);
		qint64 nextOffset = qint64(offset) + 12 + qint64(chunkLength);
		if(nextOffset > pngData.size()) {
			break;
		}

		if(type == "zTXt" || type == "tEXt") {
			QByteArray chunk = pngData.mid(offset + 8, int(chunkLength));
			int nul = chunk.indexOf('\0');
			if(nul >= 0 &&
			   chunk.left(nul).compare("preset", Qt::CaseInsensitive) == 0) {
				if(type == "tEXt") {
					return chunk.mid(nul + 1);
				}
				if(nul + 1 < chunk.size() && quint8(chunk.at(nul + 1)) == 0) {
					return uncompressZlibData(chunk.mid(nul + 2));
				}
			}
		}

		if(type == "IEND") {
			break;
		}
		offset = int(nextOffset);
	}
	return QByteArray();
}

static QStringList readKritaManifestPresetPaths(QDomDocument &doc)
{
	QStringList paths;
	QDomNodeList entries = doc.elementsByTagNameNS(
		KRITA_MANIFEST_NAMESPACE, QStringLiteral("file-entry"));
	for(int i = 0; i < entries.size(); ++i) {
		QDomElement entry = entries.at(i).toElement();
		if(entry.isNull()) {
			continue;
		}
		QString mediaType = entry.attributeNS(
			KRITA_MANIFEST_NAMESPACE, QStringLiteral("media-type"));
		if(mediaType.compare(
				   QStringLiteral("paintoppresets"), Qt::CaseInsensitive) != 0) {
			continue;
		}
		QString path = entry.attributeNS(
			KRITA_MANIFEST_NAMESPACE, QStringLiteral("full-path"));
		while(path.startsWith(QLatin1Char('/'))) {
			path.remove(0, 1);
		}
		if(!path.isEmpty() &&
		   path.endsWith(QStringLiteral(".kpp"), Qt::CaseInsensitive) &&
		   !paths.contains(path)) {
			paths.append(path);
		}
	}
	return paths;
}

static bool hasKritaManifestEntry(QDomDocument &doc)
{
	QDomNodeList entries = doc.elementsByTagNameNS(
		KRITA_MANIFEST_NAMESPACE, QStringLiteral("file-entry"));
	for(int i = 0; i < entries.size(); ++i) {
		QDomElement entry = entries.at(i).toElement();
		if(entry.isNull()) {
			continue;
		}
		QString mediaType = entry.attributeNS(
			KRITA_MANIFEST_NAMESPACE, QStringLiteral("media-type"));
		if(mediaType.startsWith(
				   QStringLiteral("application/x-krita-resourcebundle"),
				   Qt::CaseInsensitive) ||
		   mediaType.compare(
			   QStringLiteral("paintoppresets"), Qt::CaseInsensitive) == 0) {
			return true;
		}
	}
	return false;
}

static QHash<QString, QString> readKritaParams(const QDomElement &root)
{
	QHash<QString, QString> params;
	QDomNodeList nodes = root.elementsByTagName(QStringLiteral("param"));
	for(int i = 0; i < nodes.size(); ++i) {
		QDomElement param = nodes.at(i).toElement();
		if(param.isNull()) {
			continue;
		}
		QString name = param.attribute(QStringLiteral("name"));
		if(!name.isEmpty()) {
			params.insert(name, param.text().trimmed());
		}
	}
	return params;
}

static QHash<QString, QString>
readKritaParamAttributes(const QDomDocument &doc)
{
	QHash<QString, QString> params;
	QDomNodeList nodes =
		doc.documentElement().elementsByTagName(QStringLiteral("param"));
	for(int i = 0; i < nodes.size(); ++i) {
		QDomElement param = nodes.at(i).toElement();
		if(param.isNull()) {
			continue;
		}
		QString name = param.attribute(QStringLiteral("name"));
		if(name.isEmpty()) {
			continue;
		}
		QString value = param.attribute(QStringLiteral("value"));
		if(value.isEmpty()) {
			value = param.text().trimmed();
		}
		params.insert(name.toLower(), value.trimmed());
	}
	return params;
}

static QString kritaParam(
	const QHash<QString, QString> &params, const QString &name,
	const QString &fallback = QString())
{
	QString value = params.value(name.toLower());
	return value.isEmpty() ? fallback : value;
}

static double kritaParamDouble(
	const QHash<QString, QString> &params, const QString &name,
	double fallback)
{
	bool ok = false;
	double value = kritaParam(params, name).toDouble(&ok);
	return ok ? value : fallback;
}

static bool kritaParamBool(
	const QHash<QString, QString> &params, const QString &name,
	bool fallback = false)
{
	QString value = kritaParam(params, name).toLower();
	if(value.isEmpty()) {
		return fallback;
	}
	return value == QLatin1String("true") || value == QLatin1String("1") ||
		   value == QLatin1String("yes");
}

static QString kritaFirstCurveText(const QDomElement &element)
{
	QDomNodeList curves = element.elementsByTagName(QStringLiteral("curve"));
	for(int i = 0; i < curves.size(); ++i) {
		QString text = curves.at(i).toElement().text().trimmed();
		if(text.contains(QLatin1Char(','))) {
			return text;
		}
	}
	return QString();
}

struct KritaSensorInfo {
	QString id;
	QString curve;
};

static KritaSensorInfo kritaSensorInfo(const QString &xml)
{
	KritaSensorInfo info;
	if(xml.isEmpty()) {
		return info;
	}

	QDomDocument doc;
	if(!doc.setContent(xml)) {
		return info;
	}

	QDomElement root = doc.documentElement();
	info.id = root.attribute(QStringLiteral("id"));
	info.curve = kritaFirstCurveText(root);
	if(info.id.compare(QStringLiteral("sensorslist"), Qt::CaseInsensitive) ==
	   0) {
		QDomNodeList children =
			root.elementsByTagName(QStringLiteral("ChildSensor"));
		for(int i = 0; i < children.size(); ++i) {
			QDomElement child = children.at(i).toElement();
			if(child.attribute(QStringLiteral("id")).compare(
					   QStringLiteral("pressure"), Qt::CaseInsensitive) ==
			   0) {
				info.id = QStringLiteral("pressure");
				info.curve = kritaFirstCurveText(child);
				break;
			}
		}
		if(info.id.compare(
				   QStringLiteral("sensorslist"), Qt::CaseInsensitive) == 0 &&
		   !children.isEmpty()) {
			QDomElement child = children.at(0).toElement();
			info.id = child.attribute(QStringLiteral("id"));
			info.curve = kritaFirstCurveText(child);
		}
	}
	return info;
}

static QString normalizeKritaCurve(const QString &curveString)
{
	if(curveString.isEmpty()) {
		return QString();
	}

	KisCubicCurve curve;
	curve.fromString(curveString);
	QList<QPointF> points = curve.points();
	if(points.isEmpty()) {
		return QString();
	}

	if(points.first().x() > 0.0) {
		points.prepend(QPointF(0.0, points.first().y()));
	}
	if(points.last().x() < 1.0) {
		points.append(QPointF(1.0, points.last().y()));
	}
	for(QPointF &point : points) {
		point.setY(qBound(0.0, point.y(), 1.0));
	}
	curve.setPoints(points);
	return curve.toString();
}

static QString kritaBlendToDrawpile(const QString &kritaName, bool &erase)
{
	erase = false;
	QString name = kritaName.trimmed().toLower();
	static const QHash<QString, QString> modes{
		{QStringLiteral("normal"), QStringLiteral("svg:src-over")},
		{QStringLiteral("copy"), QStringLiteral("svg:src-over")},
		{QStringLiteral("multiply"), QStringLiteral("svg:multiply")},
		{QStringLiteral("divide"), QStringLiteral("krita:divide")},
		{QStringLiteral("burn"), QStringLiteral("svg:color-burn")},
		{QStringLiteral("dodge"), QStringLiteral("svg:color-dodge")},
		{QStringLiteral("darken"), QStringLiteral("svg:darken")},
		{QStringLiteral("lighten"), QStringLiteral("svg:lighten")},
		{QStringLiteral("add"), QStringLiteral("svg:plus")},
		{QStringLiteral("screen"), QStringLiteral("svg:screen")},
		{QStringLiteral("overlay"), QStringLiteral("svg:overlay")},
		{QStringLiteral("hard_light"), QStringLiteral("svg:hard-light")},
		{QStringLiteral("soft_light"), QStringLiteral("svg:soft-light")},
		{QStringLiteral("difference"), QStringLiteral("svg:difference")},
		{QStringLiteral("hue"), QStringLiteral("svg:hue")},
		{QStringLiteral("saturation"), QStringLiteral("svg:saturation")},
		{QStringLiteral("color"), QStringLiteral("svg:color")},
		{QStringLiteral("value"), QStringLiteral("svg:value")},
	};

	if(name == QLatin1String("erase") || name == QLatin1String("eraser") ||
	   name == QLatin1String("erase_preserve")) {
		erase = true;
		return QStringLiteral("-dp-erase");
	}
	return modes.value(name, QStringLiteral("svg:src-over"));
}

static DP_ClassicBrushDynamicType
kritaDynamicType(const QString &sensorId, bool pressureEnabled)
{
	if(!pressureEnabled && sensorId.compare(
							   QStringLiteral("pressure"),
							   Qt::CaseInsensitive) == 0) {
		return DP_CLASSIC_BRUSH_DYNAMIC_NONE;
	}
	if(sensorId.compare(QStringLiteral("pressure"), Qt::CaseInsensitive) == 0 ||
	   sensorId.isEmpty()) {
		return DP_CLASSIC_BRUSH_DYNAMIC_PRESSURE;
	}
	if(sensorId.contains(QStringLiteral("speed"), Qt::CaseInsensitive) ||
	   sensorId.compare(
		   QStringLiteral("drawingangle"), Qt::CaseInsensitive) == 0) {
		return DP_CLASSIC_BRUSH_DYNAMIC_VELOCITY;
	}
	if(sensorId.compare(QStringLiteral("distance"), Qt::CaseInsensitive) == 0) {
		return DP_CLASSIC_BRUSH_DYNAMIC_DISTANCE;
	}
	return DP_CLASSIC_BRUSH_DYNAMIC_NONE;
}

static QString invertKritaCurve(const QString &curve)
{
	if(curve.isEmpty()) {
		return curve;
	}
	QStringList pairs = curve.split(QLatin1Char(';'), compat::SkipEmptyParts);
	for(int i = 0; i < pairs.size(); ++i) {
		int comma = pairs[i].indexOf(QLatin1Char(','));
		if(comma < 0) {
			continue;
		}
		bool ok = false;
		double y = pairs[i].mid(comma + 1).trimmed().toDouble(&ok);
		if(ok) {
			pairs[i] = pairs[i].left(comma + 1) +
				QString::number(1.0 - qBound(0.0, y, 1.0));
		}
	}
	return pairs.join(QLatin1Char(';')) + QLatin1Char(';');
}

static void appendKritaCurve(
	QDomDocument &doc, QDomElement &setting, const QString &curveString)
{
	if(curveString.isEmpty()) {
		return;
	}
	QDomElement curve = doc.createElement(QStringLiteral("curve"));
	QStringList pairs =
		curveString.split(QLatin1Char(';'), compat::SkipEmptyParts);
	for(const QString &pair : pairs) {
		int comma = pair.indexOf(QLatin1Char(','));
		if(comma < 0) {
			continue;
		}
		bool xOk = false, yOk = false;
		double x = pair.left(comma).trimmed().toDouble(&xOk);
		double y = pair.mid(comma + 1).trimmed().toDouble(&yOk);
		if(!xOk || !yOk) {
			continue;
		}
		QDomElement point = doc.createElement(QStringLiteral("point"));
		point.setAttribute(
			QStringLiteral("x"), QString::number(qBound(0.0, x, 1.0)));
		point.setAttribute(
			QStringLiteral("y"), QString::number(qBound(0.0, y, 1.0)));
		curve.appendChild(point);
	}
	if(curve.hasChildNodes()) {
		setting.appendChild(curve);
	}
}

static QDomElement appendKritaSetting(
	QDomDocument &doc, QDomElement &settings, const QString &id, double value,
	const QString &curve = QString(), const QString &dynamic = QString())
{
	QDomElement setting = doc.createElement(QStringLiteral("setting"));
	setting.setAttribute(QStringLiteral("id"), id);
	if(!dynamic.isEmpty()) {
		setting.setAttribute(QStringLiteral("dynamic"), dynamic);
	}
	setting.appendChild(
		doc.createTextNode(QString::number(qBound(0.0, value, 1000.0))));
	appendKritaCurve(doc, setting, curve);
	settings.appendChild(setting);
	return setting;
}

static QString kritaDynamicName(DP_ClassicBrushDynamicType type)
{
	switch(type) {
	case DP_CLASSIC_BRUSH_DYNAMIC_PRESSURE:
		return QStringLiteral("pressure");
	case DP_CLASSIC_BRUSH_DYNAMIC_VELOCITY:
		return QStringLiteral("velocity");
	case DP_CLASSIC_BRUSH_DYNAMIC_DISTANCE:
		return QStringLiteral("distance");
	default:
		return QString();
	}
}

static QDomDocument kritaPresetToLegacyDocument(
	const QDomDocument &presetDoc, const QString &presetPath)
{
	QDomDocument doc;
	QDomElement presetRoot = presetDoc.documentElement();
	QHash<QString, QString> params = readKritaParams(presetRoot);
	QHash<QString, QString> lowerParams;
	for(auto it = params.constBegin(); it != params.constEnd(); ++it) {
		lowerParams.insert(it.key().toLower(), it.value());
	}

	QString name = presetRoot.attribute(QStringLiteral("name"));
	if(name.isEmpty()) {
		name = presetRoot.attribute(QStringLiteral("id"));
	}
	if(name.isEmpty()) {
		name = QFileInfo(presetPath).baseName();
	}

	QString engine = kritaParam(lowerParams, QStringLiteral("paintop"));
	if(engine.isEmpty()) {
		engine = presetRoot.attribute(QStringLiteral("paintopid"));
	}
	if(engine.isEmpty()) {
		engine = kritaParam(lowerParams, QStringLiteral("paintopid"));
	}
	QString engineLower = engine.toLower();

	QDomElement root = doc.createElement(QStringLiteral("brush"));
	root.setAttribute(QStringLiteral("id"), name);
	doc.appendChild(root);

	QDomElement nameElem = doc.createElement(QStringLiteral("name"));
	nameElem.appendChild(doc.createTextNode(name));
	root.appendChild(nameElem);

	QDomElement engineElem = doc.createElement(QStringLiteral("engine"));
	engineElem.appendChild(doc.createTextNode(engine));
	root.appendChild(engineElem);

	QString comment = kritaParam(lowerParams, QStringLiteral("comment"));
	if(comment.isEmpty()) {
		comment = kritaParam(lowerParams, QStringLiteral("description"));
	}
	if(!comment.isEmpty()) {
		QDomElement commentElem = doc.createElement(QStringLiteral("comment"));
		commentElem.appendChild(doc.createTextNode(comment));
		root.appendChild(commentElem);
	}

	QDomElement paramsElem = doc.createElement(QStringLiteral("params"));
	for(auto it = params.constBegin(); it != params.constEnd(); ++it) {
		QDomElement param = doc.createElement(QStringLiteral("param"));
		param.setAttribute(QStringLiteral("name"), it.key());
		param.setAttribute(QStringLiteral("value"), it.value());
		paramsElem.appendChild(param);
	}
	root.appendChild(paramsElem);

	QDomElement settings = doc.createElement(QStringLiteral("settings"));
	root.appendChild(settings);

	double size = 16.0;
	double spacing = 10.0;
	double hard = 0.0;
	QString maskSoftnessCurve;

	QString definition = kritaParam(lowerParams, QStringLiteral("brush_definition"));
	if(!definition.isEmpty()) {
		QDomDocument defDoc;
		QString wrapped = QStringLiteral("<kritaBrushDefinition>%1</kritaBrushDefinition>")
							  .arg(definition);
		if(parseDomXml(defDoc, wrapped.toUtf8(), false).ok) {
			QDomElement brush = defDoc.documentElement().firstChildElement(
				QStringLiteral("Brush"));
			if(!brush.isNull()) {
				bool ok = false;
				double diameter = brush.attribute(QStringLiteral("diameter"))
									  .toDouble(&ok);
				if(ok && diameter > 0.0) {
					size = diameter;
				}
				double brushSpacing =
					brush.attribute(QStringLiteral("spacing")).toDouble(&ok);
				if(ok && brushSpacing > 0.0) {
					spacing = brushSpacing <= 1.0 ? brushSpacing * 100.0
												  : brushSpacing;
				}

				QDomElement mask = brush.firstChildElement(
					QStringLiteral("MaskGenerator"));
				maskSoftnessCurve =
					mask.attribute(QStringLiteral("softness_curve"));
				if(!maskSoftnessCurve.isEmpty()) {
					KisCubicCurve curve;
					curve.fromString(maskSoftnessCurve);
					hard = 1.0 - curve.value(0.5);
				} else {
					bool vfadeOk = false;
					double vfade = mask.attribute(QStringLiteral("vfade"))
									   .toDouble(&vfadeOk);
					if(vfadeOk) {
						hard = 1.0 - qBound(0.0, vfade, 1.0) * 0.5;
					} else if(mask.attribute(QStringLiteral("id"))
								  .contains(
									  QStringLiteral("soft"),
									  Qt::CaseInsensitive)) {
						hard = 0.3;
					} else {
						hard = 0.8;
					}
				}
			}
		}
	}

	if(engineLower == QLatin1String("spraybrush")) {
		size = kritaParamDouble(
			lowerParams, QStringLiteral("spray/diameter"), size);
		spacing = kritaParamDouble(
			lowerParams, QStringLiteral("spray/spacing"), 0.5) * 100.0;
	} else if(engineLower == QLatin1String("roundmarker")) {
		size = kritaParamDouble(
			lowerParams, QStringLiteral("diameter"), size);
		double markerSpacing = kritaParamDouble(
			lowerParams, QStringLiteral("spacing"), spacing / 100.0);
		spacing = markerSpacing <= 1.0 ? markerSpacing * 100.0
									   : markerSpacing;
	} else if(engineLower == QLatin1String("deformbrush")) {
		size = kritaParamDouble(
			lowerParams, QStringLiteral("brush/diameter"), size);
		double deformSpacing = kritaParamDouble(
			lowerParams, QStringLiteral("brush/spacing"), spacing / 100.0);
		spacing = deformSpacing <= 1.0 ? deformSpacing * 100.0
									   : deformSpacing;
	} else if(engineLower == QLatin1String("dynabrush")) {
		size = kritaParamDouble(
			lowerParams, QStringLiteral("dyna/diameter"), size);
	}

	size = qBound(1.0, size * kritaParamDouble(
							lowerParams, QStringLiteral("sizevalue"), 1.0),
				  1000.0);
	spacing = qBound(
		1.0,
		spacing * kritaParamDouble(
					  lowerParams, QStringLiteral("spacingvalue"), 1.0),
		1000.0);
	double opacity = qBound(
		0.0,
		kritaParamDouble(lowerParams, QStringLiteral("opacityvalue"), 1.0) *
			kritaParamDouble(lowerParams, QStringLiteral("flowvalue"), 1.0) *
			(kritaParamDouble(
				 lowerParams, QStringLiteral("krita/opacity"), 100.0) /
			 100.0),
		1.0);
	hard = qBound(0.0, hard, 1.0);

	KritaSensorInfo sizeSensor =
		kritaSensorInfo(kritaParam(lowerParams, QStringLiteral("sizesensor")));
	KritaSensorInfo opacitySensor = kritaSensorInfo(
		kritaParam(lowerParams, QStringLiteral("opacitysensor")));
	KritaSensorInfo softnessSensor = kritaSensorInfo(
		kritaParam(lowerParams, QStringLiteral("softnesssensor")));
	KritaSensorInfo sharpnessSensor = kritaSensorInfo(
		kritaParam(lowerParams, QStringLiteral("sharpnesssensor")));
	KritaSensorInfo scatterSensor = kritaSensorInfo(
		kritaParam(lowerParams, QStringLiteral("scattersensor")));
	KritaSensorInfo spacingSensor = kritaSensorInfo(
		kritaParam(lowerParams, QStringLiteral("spacingsensor")));
	KritaSensorInfo smudgeSensor = kritaSensorInfo(
		kritaParam(lowerParams, QStringLiteral("smudgeratesensor")));
	KritaSensorInfo colorRateSensor = kritaSensorInfo(
		kritaParam(lowerParams, QStringLiteral("colorratesensor")));

	QString sizeCurve = normalizeKritaCurve(sizeSensor.curve);
	if(sizeCurve.isEmpty()) {
		sizeCurve = normalizeKritaCurve(
			kritaParam(lowerParams, QStringLiteral("curvesize")));
	}
	QString opacityCurve = normalizeKritaCurve(opacitySensor.curve);
	if(opacityCurve.isEmpty()) {
		opacityCurve = normalizeKritaCurve(
			kritaParam(lowerParams, QStringLiteral("curveopacity")));
	}
	QString hardCurve;
	if(!softnessSensor.curve.isEmpty()) {
		hardCurve = normalizeKritaCurve(
			invertKritaCurve(softnessSensor.curve));
	} else if(!sharpnessSensor.curve.isEmpty()) {
		hardCurve = normalizeKritaCurve(sharpnessSensor.curve);
	} else if(!maskSoftnessCurve.isEmpty()) {
		hardCurve = normalizeKritaCurve(
			invertKritaCurve(maskSoftnessCurve));
	} else {
		hardCurve = normalizeKritaCurve(invertKritaCurve(kritaParam(
			lowerParams, QStringLiteral("curvesoftness"))));
		if(hardCurve.isEmpty()) {
			hardCurve = normalizeKritaCurve(
				kritaParam(lowerParams, QStringLiteral("curvesharpness")));
		}
	}
	QString jitterCurve = normalizeKritaCurve(scatterSensor.curve);
	if(jitterCurve.isEmpty()) {
		jitterCurve = normalizeKritaCurve(
			kritaParam(lowerParams, QStringLiteral("curvescatter")));
	}
	QString spacingCurve = normalizeKritaCurve(spacingSensor.curve);
	QString smudgeCurve = normalizeKritaCurve(smudgeSensor.curve);
	if(smudgeCurve.isEmpty()) {
		smudgeCurve = normalizeKritaCurve(colorRateSensor.curve);
	}

	double jitter = 0.0;
	if(kritaParam(lowerParams, QStringLiteral("scattering/amount")).isEmpty() &&
	   kritaParamBool(lowerParams, QStringLiteral("pressurescatter"))) {
		jitter = kritaParamDouble(
					 lowerParams, QStringLiteral("scattervalue"), 1.0) *
				 100.0;
	} else {
		jitter = kritaParamDouble(
					 lowerParams, QStringLiteral("scattering/amount"),
					 kritaParamDouble(
						 lowerParams, QStringLiteral("sketch/probability"),
						 kritaParamBool(
							 lowerParams,
							 QStringLiteral("spray/jittermovement"))
							 ? kritaParamDouble(
								   lowerParams,
								   QStringLiteral("spray/jittermoveamount"),
								   1.0)
							 : kritaParamDouble(
								   lowerParams,
								   QStringLiteral("hairybristle/random"),
								   0.0))) *
				 100.0;
	}

	double smudge = 0.0;
	if(!kritaParam(lowerParams, QStringLiteral("smudgeratevalue")).isEmpty()) {
		smudge = kritaParamDouble(
					 lowerParams, QStringLiteral("smudgeratevalue"), 0.0) *
				 kritaParamDouble(
					 lowerParams, QStringLiteral("colorratevalue"), 1.0);
	} else if(engineLower == QLatin1String("duplicate") ||
			  engineLower == QLatin1String("filter")) {
		smudge = kritaParamDouble(
			lowerParams, QStringLiteral("opacityvalue"), 1.0);
	}
	smudge = qBound(0.0, smudge, 1.0);
	int resmudge = qBound(
		0,
		qRound(kritaParamDouble(
			lowerParams, QStringLiteral("smudgeradiusvalue"), 0.0)),
		16);

	bool erase = kritaParamBool(lowerParams, QStringLiteral("krita/erase"));
	QString composite = kritaParam(
		lowerParams, QStringLiteral("compositeop"),
		kritaParam(lowerParams, QStringLiteral("krita/compositeop"), QStringLiteral("normal")));
	QString blend = kritaBlendToDrawpile(composite, erase);
	if(blend == QLatin1String("-dp-erase")) {
		blend = QStringLiteral("svg:src-over");
	}
	if(!canvas::blendmode::isValidBrushMode(
		   canvas::blendmode::fromOraName(blend))) {
		blend = QStringLiteral("svg:src-over");
	}

	bool airbrush = kritaParamBool(
		lowerParams, QStringLiteral("airbrushoption/isairbrushing"));
	bool indirect = airbrush || engineLower == QLatin1String("sketchbrush") ||
					engineLower == QLatin1String("hatchingbrush") ||
					engineLower == QLatin1String("curvebrush") ||
					engineLower == QLatin1String("hairybrush") ||
					engineLower == QLatin1String("spraybrush") ||
					engineLower == QLatin1String("dynabrush") ||
					engineLower == QLatin1String("particlebrush") ||
					engineLower == QLatin1String("experimentbrush");
	bool pixel = engineLower.contains(QStringLiteral("pixel"));

	settings.setAttribute(QStringLiteral("engine"), engineLower);
	settings.setAttribute(QStringLiteral("blend"), blend);
	settings.setAttribute(QStringLiteral("erase"), erase);
	settings.setAttribute(
		QStringLiteral("paintmode"),
		indirect ? QStringLiteral("indirect_wash") : QStringLiteral("direct"));
	settings.setAttribute(QStringLiteral("airbrush"), airbrush);
	settings.setAttribute(QStringLiteral("pixel"), pixel);

	QString sizeDynamic = kritaDynamicName(kritaDynamicType(
		sizeSensor.id,
		kritaParamBool(lowerParams, QStringLiteral("pressuresize"), true)));
	QString opacityDynamic = kritaDynamicName(kritaDynamicType(
		opacitySensor.id,
		kritaParamBool(lowerParams, QStringLiteral("pressureopacity"), true)));
	QString hardDynamic = kritaDynamicName(kritaDynamicType(
		!softnessSensor.id.isEmpty() ? softnessSensor.id : sharpnessSensor.id,
		kritaParamBool(lowerParams, QStringLiteral("pressuresoftness")) ||
			kritaParamBool(lowerParams, QStringLiteral("pressuresharpness"))));
	QString jitterDynamic = kritaDynamicName(kritaDynamicType(
		scatterSensor.id,
		kritaParamBool(lowerParams, QStringLiteral("pressurescatter"))));
	QString spacingDynamic = kritaDynamicName(kritaDynamicType(
		spacingSensor.id,
		kritaParamBool(lowerParams, QStringLiteral("pressurespacing"))));
	QString smudgeDynamic = kritaDynamicName(kritaDynamicType(
		!smudgeSensor.id.isEmpty() ? smudgeSensor.id : colorRateSensor.id,
		kritaParamBool(lowerParams, QStringLiteral("pressuresmudgerate")) ||
			kritaParamBool(lowerParams, QStringLiteral("pressurecolorrate"))));

	appendKritaSetting(
		doc, settings, QStringLiteral("size"), size, sizeCurve, sizeDynamic);
	appendKritaSetting(
		doc, settings, QStringLiteral("opacity"), opacity, opacityCurve,
		opacityDynamic);
	appendKritaSetting(
		doc, settings, QStringLiteral("hard"), hard, hardCurve, hardDynamic);
	appendKritaSetting(
		doc, settings, QStringLiteral("spacing"), spacing, spacingCurve,
		spacingDynamic);
	appendKritaSetting(
		doc, settings, QStringLiteral("jitter"), jitter, jitterCurve,
		jitterDynamic);
	appendKritaSetting(
		doc, settings, QStringLiteral("smudge"), smudge, smudgeCurve,
		smudgeDynamic);
	appendKritaSetting(
		doc, settings, QStringLiteral("resmudge"), resmudge);

	return doc;
}
}

class BrushPresetTagModel::Private {
public:
	Private(BrushPresetTagModel *parent)
		: m_presetChangeTimer(Qt::VeryCoarseTimer, 4000)
	{
		initDb();
		connect(
			&m_presetChangeTimer, &CompressedTimer::timeout, parent,
			std::bind(&Private::writePresetChanges, this));
	}

	void setPresetModel(BrushPresetModel *presetModel)
	{
		Q_ASSERT(!m_presetModel);
		m_presetModel = presetModel;
	}

	const QString &path() const { return db.path(); }

	drawdance::Database &database() { return db; }

	BrushPresetModel *presetModel() const { return m_presetModel; }

	int createTag(drawdance::Query &qry, const QString &name)
	{
		if(qry.exec("insert into tag (name) values (?)", {name})) {
			return qry.lastInsertId();
		} else {
			return 0;
		}
	}

	int createTag(const QString &name)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query qry = db.query();
		return createTag(qry, name);
	}

	int readTagCount() { return db.readInt("select count(*) from tag"); }

	int readTagCountByName(drawdance::Query &qry, const QString &name)
	{
		const char *sql = "select count(*) from tag where name = ?";
		if(qry.exec(sql, {name}) && qry.next()) {
			return qry.columnInt(0);
		} else {
			return 0;
		}
	}

	int readTagCountByName(const QString &name)
	{
		drawdance::Query qry = db.query();
		return readTagCountByName(qry, name);
	}

	int readTagIdAtIndex(int index)
	{
		return db.readInt(
			"select id from tag order by LOWER(name) limit 1 offset ?", 0,
			{index});
	}

	Tag readTagAtIndex(int index)
	{
		drawdance::Query query = db.query();
		const char *sql =
			"select id, name from tag order by LOWER(name) limit 1 offset ?";
		if(query.exec(sql, {index}) && query.next()) {
			return Tag{query.columnInt(0), query.columnText16(1)};
		} else {
			return Tag{0, QString()};
		}
	}

	QString readTagNameById(int id)
	{
		return db.readText16("select name from tag where id = ?", {id});
	}

	int readTagIdByName(const QString &name)
	{
		return db.readInt("select id from tag where name = ?", 0, {name});
	}

	int readTagIndexById(int id)
	{
		const char *sql =
			"select n - 1 from (select row_number() over("
			"order by LOWER(name)) as n, id from tag) where id = ?";
		return db.readInt(sql, -1, {id});
	}

	bool updateTagName(int id, const QString &name)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		if(query.exec("update tag set name = ? where id = ?", {name, id})) {
			return query.numRowsAffected() == 1;
		} else {
			return false;
		}
	}

	bool deleteTagById(int id)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		if(query.exec("delete from tag where id = ?", {id})) {
			return query.numRowsAffected() == 1;
		} else {
			return false;
		}
	}

	int createPreset(
		drawdance::Query &qry, int state, const QString &name,
		const QString &description, const QByteArray &thumbnail,
		const QString &type, const QByteArray &data)
	{
		const char *sql = "insert into preset (state, name, description, "
						  "thumbnail, type, data) values (?, ?, ?, ?, ?, ?)";
		if(qry.exec(sql, {state, name, description, thumbnail, type, data})) {
			return qry.lastInsertId();
		} else {
			return 0;
		}
	}

	int createPreset(
		int state, const QString &name, const QString &description,
		const QByteArray &thumbnail, const QString &type,
		const QByteArray &data)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query qry = db.query();
		return createPreset(
			qry, state, name, description, thumbnail, type, data);
	}

	int readPresetCountAll()
	{
		return db.readInt("select count(*) from preset where state = 0");
	}

	int readPresetCountByUntagged()
	{
		return db.readInt(
			"select count(*) from preset p where p.state = 0 and not exists("
			"select 1 from preset_tag pt where pt.preset_id = p.id)");
	}

	int readPresetCountHistory()
	{
		return db.readInt(
			"select count(*) from preset p "
			"join preset_history ph on ph.preset_id = p.id");
	}

	int readPresetHistoryCountPendingRemoval()
	{
		return db.readInt(
			"select count(*) from preset p "
			"join preset_history ph on ph.preset_id = p.id "
			"where p.state <> 0");
	}

	int readPresetCountByTagId(int tagId)
	{
		return db.readInt(
			"select count(*) from preset p "
			"join preset_tag pt on pt.preset_id = p.id "
			"where p.state = 0 and pt.tag_id = ?",
			0, {tagId});
	}

	int readPresetCountByName(const QString &name)
	{
		return db.readInt(
			"select count(*) from preset where state = 0 and name = ?", 0,
			{name});
	}

	int readPresetIdAtIndexAll(int index)
	{
		return db.readInt(
			"select id from preset where state = 0 "
			"order by LOWER(name) limit 1 offset ?",
			0, {index});
	}

	int readPresetIdAtIndexByUntagged(int index)
	{
		return db.readInt(
			"select p.id from preset p where state = 0 and not exists("
			"select 1 from preset_tag pt where pt.preset_id = p.id) "
			"order by LOWER(p.name) limit 1 offset ?",
			0, {index});
	}

	int readPresetIdAtIndexByTagId(int index, int tagId)
	{
		return db.readInt(
			"select p.id from preset p "
			"join preset_tag pt on pt.preset_id = p.id "
			"where p.state = 0 and pt.tag_id = ? "
			"order by LOWER(p.name) limit 1 offset ?",
			0, {tagId, index});
	}

	int readPresetStateById(int id)
	{
		return db.readInt(
			"select state from preset where id = ?", int(PresetState::Normal),
			{id});
	}

	QString readPresetEffectiveNameById(int id)
	{
		return db.readText16(
			"select coalesce(changed_name, name) from preset where id = ?",
			{id});
	}

	QString readPresetEffectiveDescriptionById(int id)
	{
		return db.readText16(
			"select coalesce(changed_description, description) "
			"from preset where id = ?",
			{id});
	}

	QByteArray readPresetEffectiveThumbnailById(int id)
	{
		return db.readBlob(
			"select coalesce(changed_thumbnail, thumbnail) from "
			"preset where id = ?",
			{id});
	}

	static QByteArray readPreparedBlobById(drawdance::Query &qry, int id)
	{
		drawdance::DatabaseLocker locker(db);
		QByteArray result;
		if(qry.bind(0, id) && qry.execPrepared() && qry.next()) {
			result = qry.columnBlob(0);
		}
		qry.reset();
		return result;
	}

	QByteArray readPresetThumbnailById(int id)
	{
		return readPreparedBlobById(queries->readThumbnailByIdQuery, id);
	}

	QByteArray readPresetDataById(int id)
	{
		return readPreparedBlobById(queries->readDataByIdQuery, id);
	}

	QByteArray readPresetChangedThumbnailById(int id)
	{
		return readPreparedBlobById(queries->readChangedThumbnailByIdQuery, id);
	}

	QByteArray readPresetChangedDataById(int id)
	{
		return readPreparedBlobById(queries->readChangedDataByIdQuery, id);
	}

	bool readPresetHasChangesById(int id)
	{
		return db.readInt(
			"select changed_name is not null or changed_description is not "
			"null or changed_thumbnail is not null or changed_data is not "
			"null as has_changes from preset where id = ?",
			0, {id});
	}

	std::optional<Preset> readPresetById(int id)
	{
		drawdance::Query query = db.query();
		const char *sql = "select id, state, name, description, changed_name, "
						  "changed_description, changed_thumbnail is not null, "
						  "changed_data is not null from preset where id = ?";
		if(query.exec(sql, {id}) && query.next()) {
			Preset preset;
			readPreset(preset, query);
			return preset;
		} else {
			return {};
		}
	}

	int readPresetIdByContent(const QString &type, const QByteArray &data)
	{
		drawdance::Query query = db.query();
		const char *sql =
			"select id from preset where type = ? and "
			"coalesce(changed_data, data) = ? order by id limit 1";
		if(query.exec(sql, {type, data}) && query.next()) {
			return query.columnInt(0);
		} else {
			return 0;
		}
	}

	bool updatePreset(
		int id, const QString &name, const QString &description,
		const QByteArray &thumbnail, const QString &type,
		const QByteArray &data)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		if(query.exec(
			   "update preset set name = ?, description = ?, thumbnail = ?, "
			   "type = ?, data = ?, changed_name = null, "
			   "changed_description = null, changed_thumbnail = null, "
			   "changed_type = null, changed_data = null where id = ?",
			   {name, description, thumbnail, type, data, id})) {
			return query.numRowsAffected() == 1;
		} else {
			return false;
		}
	}

	bool updateTransientPreset(
		int id, int state, const QString &name, const QString &description,
		const QByteArray &thumbnail, const QSet<int> &tagIds)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();

		if(!query.exec(
			   "update preset set state = ?, name = ?, description = ?, "
			   "thumbnail = ?, changed_name = null, "
			   "changed_description = null, changed_thumbnail = null "
			   "where id = ?",
			   {state, name, description, thumbnail, id})) {
			return false;
		}

		// Just keep going if something about the tag replacement fails.
		if(query.exec("delete from preset_tag where preset_id = ?", {id}) &&
		   !tagIds.isEmpty()) {
			if(query.prepare(
				   "insert into preset_tag (preset_id, tag_id) "
				   "values (?, ?)") &&
			   query.bind(0, id)) {
				for(int tagId : tagIds) {
					if(query.bind(1, tagId)) {
						query.execPrepared();
					}
				}
			}
		}

		return true;
	}

	bool updatePresetState(int id, int state)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		if(query.exec(
			   "update preset set state = ? where id = ?", {state, id})) {
			return query.numRowsAffected() == 1;
		} else {
			return false;
		}
	}

	bool updatePresetBrush(int id, const QString &type, const QByteArray &data)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		if(query.exec(
			   "update preset set type = ?, data = ?, changed_type = null, "
			   "changed_data = null where id = ?",
			   {type, data, id})) {
			return query.numRowsAffected() == 1;
		} else {
			return false;
		}
	}

	bool updatePresetShortcut(int id, const QString &shortcut)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		if(query.exec(
			   "update preset set shortcut = ? where id = ?", {shortcut, id})) {
			return query.numRowsAffected() == 1;
		} else {
			return false;
		}
	}

	bool resetAllPresetChanges()
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		return query.exec(
			"update preset set changed_description = null, "
			"changed_thumbnail = null, changed_type = null, "
			"changed_data = null");
	}

	bool deletePresetById(int id)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		if(query.exec("delete from preset where id = ?", {id})) {
			return query.numRowsAffected() == 1;
		} else {
			return false;
		}
	}

	bool deletePresetHistoryById(int id)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		if(query.exec("delete from preset_history where preset_id = ?", {id})) {
			bool ok = query.numRowsAffected() == 1;
			deleteOrphanedDeletedPresetsInternal(query);
			return ok;
		} else {
			return false;
		}
	}

	bool deleteAllPresetHistory()
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		return query.exec("delete from preset_history");
	}

	QVector<int> readOrphanedDeletedPresets()
	{
		QVector<int> presetIds;
		drawdance::Query query = db.query();
		if(query.exec(
			   "select p.id from preset p where p.state <> 0 and not exists ("
			   "select 1 from preset_history ph where ph.preset_id = p.id)")) {
			while(query.next()) {
				presetIds.append(query.columnInt(0));
			}
		}
		return presetIds;
	}

	bool deleteOrphanedDeletedPresets()
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		return deleteOrphanedDeletedPresetsInternal(query);
	}

	QList<TagAssignment> readTagAssignmentsByPresetId(int presetId)
	{
		QList<TagAssignment> tagAssignments;
		drawdance::Query query = db.query();
		const char *sql =
			"select t.id, t.name, pt.preset_id is not null "
			"from tag t left outer join preset_tag pt "
			"on pt.tag_id = t.id and pt.preset_id = ? order by t.id";
		if(query.exec(sql, {presetId})) {
			while(query.next()) {
				tagAssignments.append(
					TagAssignment{
						query.columnInt(0), query.columnText16(1),
						query.columnBool(2)});
			}
		}
		return tagAssignments;
	}

	bool createPresetTag(drawdance::Query &qry, int presetId, int tagId)
	{
		const char *sql =
			"insert into preset_tag (preset_id, tag_id) values (?, ?)";
		return qry.exec(sql, {presetId, tagId});
	}

	bool createPresetTag(int presetId, int tagId)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query qry = db.query();
		return createPresetTag(qry, presetId, tagId);
	}

	bool deletePresetTag(int presetId, int tagId)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		const char *sql =
			"delete from preset_tag where preset_id = ? and tag_id = ?";
		if(query.exec(sql, {presetId, tagId})) {
			return query.numRowsAffected() == 1;
		} else {
			return false;
		}
	}

	bool createOrUpdateState(
		const QString &key, const drawdance::Query::Param &value)
	{
		DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
		drawdance::Query query = db.query();
		return createOrUpdateStateInternal(query, key, value);
	}

	bool readStateBool(const QString &key, bool defaultValue)
	{
		drawdance::Query query = db.query();
		return readStateBoolInternal(query, key, defaultValue);
	}

	int readStateInt(const QString &key, int defaultValue)
	{
		drawdance::Query query = db.query();
		return readStateIntInternal(query, key, defaultValue);
	}


	void refreshTagCache()
	{
		drawdance::Query query = db.query();
		bool tagIdToFilterFound = refreshTagCacheInternal(query);
		if(!tagIdToFilterFound) {
			m_tagIdToFilter = ALL_ID;
			refreshPresetCacheInternal(query);
		}
	}

	int tagCacheSize() const { return m_tagCache.size(); }

	const CachedTag getCachedTag(int index) const { return m_tagCache[index]; }


	int presetIconSize() const { return m_presetIconSize; }

	void setPresetIconSize(int presetIconSize)
	{
		createOrUpdateState(QStringLiteral("preset_icon_size"), presetIconSize);
		refreshPresetCache();
	}


	int tagIdToFilter() const { return m_tagIdToFilter; }

	void setTagIdToFilter(int tagIdToFilter)
	{
		if(tagIdToFilter != m_tagIdToFilter) {
			m_tagIdToFilter = tagIdToFilter;
			refreshPresetCache();
		}
	}

	void refreshPresetCache()
	{
		drawdance::Query query = db.query();
		refreshPresetCacheInternal(query);
	}

	void resetAllPresetsInCache()
	{
		for(CachedPreset &cp : m_presetCache) {
			cp.changedName = {};
			cp.changedDescription = {};
			cp.changedThumbnail = {};
			cp.changedBrush = {};
		}
	}

	int presetCacheSize() const { return m_presetCache.size(); }

	CachedPreset &getCachedPreset(int index) { return m_presetCache[index]; }

	void removeCachedPreset(int index) { m_presetCache.removeAt(index); }

	void clearCachedPresets() { m_presetCache.clear(); }

	int getCachedPresetIndexById(int presetId)
	{
		int count = m_presetCache.size();
		for(int i = 0; i < count; ++i) {
			if(m_presetCache[i].id == presetId) {
				return i;
			}
		}
		return -1;
	}

	void prependCachedIndex(int index) { m_presetCache.move(index, 0); }

	void prependCachedPreset(CachedPreset &&cp)
	{
		m_presetCache.prepend(std::move(cp));
	}

	bool loadCachedPresetById(int presetId, CachedPreset &outCp)
	{
		drawdance::Query query = db.query();
		const char *sql =
			"select p.id, p.state, p.name, p.description, p.changed_name, "
			"p.changed_description, p.changed_thumbnail is not null, "
			"p.changed_data is not null, group_concat(t.tag_id) tags "
			"from preset p left join preset_tag t on t.preset_id = p.id "
			"where p.id = ?";
		if(query.prepare(sql) && query.bind(0, presetId) &&
		   query.execPrepared() && query.next()) {
			readPreset(outCp, query);
			parseGroupedTagIds(query.columnText16(7), outCp.tagIds);
			return true;
		} else {
			return false;
		}
	}

	void enqueuePresetChange(int presetId, const PresetChange &presetChange)
	{
		m_presetChanges.insert(presetId, presetChange);
		m_presetChangeTimer.start();
	}

	void enqueuePresetHistoryChange(
		int presetId, const PresetHistoryChange &presetHistoryChange)
	{
		m_presetHistoryChanges.insert(presetId, presetHistoryChange);
		m_presetChangeTimer.start();
	}

	bool hasPresetChange(int presetId)
	{
		return m_presetChanges.contains(presetId);
	}

	void removePresetChange(int presetId) { m_presetChanges.remove(presetId); }

	void removePresetHistoryChange(int presetId)
	{
		m_presetHistoryChanges.remove(presetId);
	}

	void clearPresetHistoryChanges() { m_presetHistoryChanges.clear(); }

	void writePresetChanges()
	{
		m_presetChangeTimer.stop();
		bool havePresetChanges = !m_presetChanges.isEmpty();
		bool havePresetHistoryChanges = !m_presetHistoryChanges.isEmpty();
		if(havePresetChanges || havePresetHistoryChanges) {
			DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
			drawdance::Query query = db.query();
			if(havePresetChanges) {
				const char *sql = "update preset set changed_name = ?, "
								  "changed_description = ?, "
								  "changed_thumbnail = ?, "
								  "changed_type = ?, "
								  "changed_data = ? "
								  "where id = ?";
				if(query.prepare(sql)) {
					QVector<drawdance::Query::Param> params;
					params.reserve(6);
					for(QHash<int, PresetChange>::const_iterator
							it = m_presetChanges.constBegin(),
							end = m_presetChanges.constEnd();
						it != end; ++it) {
						params.clear();
						int presetId = it.key();
						params.append(
							drawdance::Query::Param::fromOptional(it->name));
						params.append(
							drawdance::Query::Param::fromOptional(
								it->description));
						if(it->thumbnail.has_value()) {
							params.append(it->thumbnail->bytes());
						} else {
							params.append(std::nullopt);
						}
						if(it->brush.has_value()) {
							const ActiveBrush &brush =
								it->brush.value().constBrush(presetId);
							params.append(brush.presetType());
							params.append(brush.presetData());
						} else {
							params.append(std::nullopt);
							params.append(std::nullopt);
						}
						params.append(presetId);
						if(query.bindAll(params)) {
							query.execPrepared();
						}
					}
				}
				m_presetChanges.clear();
				refreshShortcutsInternal(query);
			}
			if(havePresetHistoryChanges) {
				const char *sql =
					"insert into preset_history (preset_id, last_used_at) "
					"values (?, ?) "
					"on conflict (preset_id) do update "
					"set last_used_at = excluded.last_used_at";
				if(query.prepare(sql)) {
					for(QHash<int, PresetHistoryChange>::const_iterator
							it = m_presetHistoryChanges.constBegin(),
							end = m_presetHistoryChanges.constEnd();
						it != end; ++it) {
						int presetId = it.key();
						if(query.bind(0, presetId) &&
						   query.bind(1, it->timestamp)) {
							query.execPrepared();
						}
					}
				}
				m_presetHistoryChanges.clear();
			}
		}
	}

	void discardPresetChanges()
	{
		if(m_presetHistoryChanges.isEmpty()) {
			m_presetChangeTimer.stop();
		}
		m_presetChanges.clear();
	}

	void initShortcuts()
	{
		drawdance::Query query = db.query();
		refreshShortcutsInternal(query);
	}

	void refreshShortcuts()
	{
		drawdance::Query query = db.query();
		refreshShortcutsInternal(query);
	}

	void getShortcutActions(
		const std::function<void(
			const QString &, const QString &, const QKeySequence &)> &fn) const
	{
		QLocale locale;
		for(PresetShortcutMap::const_iterator
				it = m_presetShortcuts.constBegin(),
				end = m_presetShortcuts.constEnd();
			it != end; ++it) {
			const QKeySequence &shortcut = it.key();
			fn(getActionName(shortcut), it->text(locale), shortcut);
		}
	}

	QKeySequence getShortcutForPresetId(int presetId) const
	{
		for(PresetShortcutMap::const_iterator
				it = m_presetShortcuts.constBegin(),
				end = m_presetShortcuts.constEnd();
			it != end; ++it) {
			if(it->containsId(presetId)) {
				return it.key();
			}
		}
		return QKeySequence();
	}

	QVector<int> getPresetIdsForShortcut(const QKeySequence &shortcut) const
	{
		PresetShortcutMap::const_iterator found =
			m_presetShortcuts.constFind(shortcut);
		if(found == m_presetShortcuts.constEnd()) {
			qWarning(
				"Brush shortcut for %s not found",
				qUtf8Printable(shortcut.toString(QKeySequence::NativeText)));
			return {};
		} else {
			return found->ids();
		}
	}

	bool haveShortcutForPresetId(int presetId)
	{
		for(const PresetShortcut &ps : m_presetShortcuts) {
			for(const PresetShortcutEntry &entry : ps.entries) {
				if(entry.id == presetId) {
					return true;
				}
			}
		}
		return false;
	}

	QVector<ShortcutPreset> getShortcutPresets()
	{
		drawdance::Query query = db.query();
		const char *sql =
			"select p.id, coalesce(p.changed_name, p.name), "
			"coalesce(p.changed_thumbnail, p.thumbnail), "
			"p.shortcut, group_concat(pt.tag_id) from preset p "
			"left join preset_tag pt on pt.preset_id = p.id "
			"group by p.id, coalesce(p.changed_name, p.name), "
			"coalesce(p.changed_thumbnail, p.thumbnail), p.shortcut "
			"order by lower(coalesce(p.changed_name, p.name))";
		QVector<ShortcutPreset> results;
		if(query.exec(sql)) {
			while(query.next()) {
				ShortcutPreset sp;
				sp.id = query.columnInt(0);
				sp.name = query.columnText16(1);
				sp.thumbnail.setBytes(query.columnBlob(2));
				sp.shortcut = QKeySequence::fromString(
					query.columnText16(3), QKeySequence::PortableText);
				parseGroupedTagIds(query.columnText16(4), sp.tagIds);
				results.append(sp);
			}
		}
		return results;
	}

	static QString brushShortcutPrefix()
	{
		return QStringLiteral("__brushshortcut_");
	}

private:
	struct Queries {
		drawdance::Query readThumbnailByIdQuery = db.queryWithoutLock();
		drawdance::Query readDataByIdQuery = db.queryWithoutLock();
		drawdance::Query readChangedThumbnailByIdQuery = db.queryWithoutLock();
		drawdance::Query readChangedDataByIdQuery = db.queryWithoutLock();
	};

	static void parseGroupedTagIds(const QString &input, QSet<int> &tagIds)
	{
		QStringList tagIdStrings =
			input.split(QChar(','), compat::SkipEmptyParts);
		tagIds.reserve(tagIdStrings.size());
		for(const QString &tagIdString : tagIdStrings) {
			bool ok;
			int tagId = tagIdString.toInt(&ok);
			if(ok && tagId > 0) {
				tagIds.insert(tagId);
			}
		}
	}

	bool createOrUpdateStateInternal(
		drawdance::Query &query, const QString &key,
		const drawdance::Query::Param &value)
	{
		const char *sql = "insert into state (key, value) values (?, ?) "
						  "on conflict (key) do update set value = ?";
		return query.exec(sql, {key, value, value});
	}

	bool readStateBoolInternal(
		drawdance::Query &query, const QString &key, bool defaultValue)
	{
		if(queryState(query, key)) {
			return query.columnBool(0);
		} else {
			return defaultValue;
		}
	}

	int readStateIntInternal(
		drawdance::Query &query, const QString &key, int defaultValue)
	{
		if(queryState(query, key)) {
			return query.columnInt(0);
		} else {
			return defaultValue;
		}
	}

	bool queryState(drawdance::Query &query, const QString &key)
	{
		return query.exec("select value from state where key = ?", {key}) &&
			   query.next();
	}

	bool refreshTagCacheInternal(drawdance::Query &query)
	{
		m_tagCache.clear();
		QString sql =
			QStringLiteral("select id, name from tag order by lower(name)");
		bool tagIdToFilterFound = m_tagIdToFilter < 0;
		if(query.exec(sql)) {
			while(query.next()) {
				int id = query.columnInt(0);
				m_tagCache.append({id, query.columnText16(1)});
				if(id == m_tagIdToFilter) {
					tagIdToFilterFound = true;
				}
			}
		}
		return tagIdToFilterFound;
	}

	void refreshPresetCacheInternal(drawdance::Query &query)
	{
		m_presetIconSize =
			readStateIntInternal(query, QStringLiteral("preset_icon_size"), 0);
		m_presetCache.clear();

		QString sql = QStringLiteral(
			"select p.id, p.state, p.name, p.description, p.changed_name, "
			"p.changed_description, p.changed_thumbnail is not null, "
			"p.changed_data is not null, group_concat(t.tag_id) tags "
			"from preset p left join preset_tag t on t.preset_id = p.id");
		QVector<drawdance::Query::Param> params;
		switch(m_tagIdToFilter) {
		case ALL_ID:
			sql += QStringLiteral(" where p.state = 0");
			break;
		case UNTAGGED_ID:
			sql += QStringLiteral(
				" where p.state = 0 and not exists("
				"select 1 from preset_tag pt where pt.preset_id = p.id)");
			break;
		case HISTORY_ID:
			sql += QStringLiteral(
				" join preset_history ph on ph.preset_id = p.id");
			break;
		default:
			sql += QStringLiteral(
				" join preset_tag pt on pt.preset_id = p.id "
				"where p.state = 0 and pt.tag_id = ?");
			params.append(m_tagIdToFilter);
			break;
		}
		sql += QStringLiteral(" group by p.id order by ");
		switch(m_tagIdToFilter) {
		case HISTORY_ID:
			sql += QStringLiteral("ph.last_used_at desc");
			break;
		default:
			sql += QStringLiteral("lower(p.name)");
			break;
		}

		if(query.exec(sql, params)) {
			while(query.next()) {
				CachedPreset cp;
				readPreset(cp, query);
				parseGroupedTagIds(query.columnText16(8), cp.tagIds);
				m_presetCache.append(cp);
			}
		}
	}

	static QByteArray loadPresetThumbnailCallback(void *user, int presetId)
	{
		return static_cast<Private *>(user)->readPresetThumbnailById(presetId);
	}

	static QByteArray loadPresetDataCallback(void *user, int presetId)
	{
		return static_cast<Private *>(user)->readPresetDataById(presetId);
	}

	static QByteArray
	loadPresetChangedThumbnailCallback(void *user, int presetId)
	{
		return static_cast<Private *>(user)->readPresetChangedThumbnailById(
			presetId);
	}

	static QByteArray loadPresetChangedDataCallback(void *user, int presetId)
	{
		return static_cast<Private *>(user)->readPresetChangedDataById(
			presetId);
	}

	void readPreset(Preset &preset, drawdance::Query &query)
	{
		preset.id = query.columnInt(0);
		preset.state = query.columnInt(1);
		preset.originalName = query.columnText16(2);
		preset.originalDescription = query.columnText16(3);

		preset.originalThumbnail.setLoader(
			&Private::loadPresetThumbnailCallback, this);
		preset.originalBrush.setLoader(&Private::loadPresetDataCallback, this);

		if(!query.columnNull(4)) {
			preset.changedName = query.columnText16(4);
		}

		if(!query.columnNull(5)) {
			preset.changedDescription = query.columnText16(5);
		}

		if(query.columnBool(6)) {
			preset.changedThumbnail = LazyThumbnail::fromLoader(
				&Private::loadPresetChangedThumbnailCallback, this);
		}

		if(query.columnBool(7)) {
			preset.changedBrush = LazyBrush::fromLoader(
				&Private::loadPresetChangedDataCallback, this);
		}
	}

	bool deleteOrphanedDeletedPresetsInternal(drawdance::Query &query)
	{
		return query.exec(
			"delete from preset where preset.state <> 0 and not exists ("
			"select 1 from preset_history ph where ph.preset_id = preset.id)");
	}

	static QString getActionName(const QKeySequence &shortcut)
	{
		return brushShortcutPrefix() +
			   shortcut.toString(QKeySequence::PortableText);
	}

	void refreshShortcutsInternal(drawdance::Query &query)
	{
		Q_ASSERT(m_presetModel);
		PresetShortcutMap newPresetShortcuts;

		const char *sql =
			"select shortcut, id, coalesce(changed_name, name) from preset "
			"where state = 0 and coalesce(shortcut, '') <> ''";
		if(query.exec(sql)) {
			while(query.next()) {
				QKeySequence shortcut = QKeySequence::fromString(
					query.columnText16(0), QKeySequence::PortableText);
				if(!shortcut.isEmpty()) {
					newPresetShortcuts[shortcut].entries.append(
						{query.columnInt(1), query.columnText16(2)});
				}
			}
		}

		QLocale locale;
		for(PresetShortcutMap::iterator it = newPresetShortcuts.begin(),
										end = newPresetShortcuts.end();
			it != end; ++it) {

			std::sort(it->entries.begin(), it->entries.end());
			QString text = locale.createSeparatedList(it->names());
			const QKeySequence &shortcut = it.key();

			PresetShortcutMap::iterator found =
				m_presetShortcuts.find(shortcut);
			if(found == m_presetShortcuts.end()) {
				emit m_presetModel->shortcutActionAdded(
					getActionName(shortcut), text, shortcut);
			} else {
				if(found->entries != it->entries) {
					emit m_presetModel->shortcutActionChanged(
						getActionName(shortcut), text);
				}
				m_presetShortcuts.erase(found);
			}
		}

		for(QKeySequence &shortcut : m_presetShortcuts.keys()) {
			emit m_presetModel->shortcutActionRemoved(getActionName(shortcut));
		}

		m_presetShortcuts.swap(newPresetShortcuts);
	}

	static void onCloseDbCallback(void *user)
	{
		delete static_cast<Queries *>(user);
	}

	void initDb()
	{
		drawdance::DatabaseLocker locker(db);
		if(!db.isOpen()) {
			if(utils::db::open(
				   db, QStringLiteral("prush presets"),
				   QStringLiteral("brushes.db"),
				   QStringLiteral("initialbrushpresets.db"))) {
				drawdance::Query query = db.queryWithoutLock();
				query.enableWalMode();
				query.setForeignKeysEnabled(false);
				query.tx([this, &query] {
					return createStateTable(query) && executeMigrations(query);
				});
				migratePresets();
				cleanupDb(query);
				query.setForeignKeysEnabled(true);
			}
			queries = new Queries;
			queries->readThumbnailByIdQuery.prepare(
				"select thumbnail from preset where id = ?",
				drawdance::Database::PREPARE_PERSISTENT);
			queries->readDataByIdQuery.prepare(
				"select data from preset where id = ?",
				drawdance::Database::PREPARE_PERSISTENT);
			queries->readChangedThumbnailByIdQuery.prepare(
				"select changed_thumbnail from preset where id = ?",
				drawdance::Database::PREPARE_PERSISTENT);
			queries->readChangedDataByIdQuery.prepare(
				"select changed_data from preset where id = ?",
				drawdance::Database::PREPARE_PERSISTENT);
			db.setOnClose(onCloseDbCallback, queries);
		}
		drawdance::Query query = db.queryWithoutLock();
		refreshTagCacheInternal(query);
		refreshPresetCacheInternal(query);
	}

	void cleanupDb(drawdance::Query &query)
	{
		// Purge any orphaned tag assignments.
		query.exec(
			"delete from preset_tag "
			"where not exists(select 1 from tag t "
			"where t.id = preset_tag.tag_id) "
			"or not exists(select 1 from preset p "
			"where p.id = preset_tag.preset_id)");
		deleteOrphanedDeletedPresetsInternal(query);
		query.exec("vacuum");
	}

	bool createStateTable(drawdance::Query &query)
	{
		return query.exec(
			"create table if not exists state ("
			"key text primary key not null, value)");
	}

	bool executeMigrations(drawdance::Query &query)
	{
		QString key = QStringLiteral("migration_version");
		QVariant migrationVersionVariant = readStateIntInternal(query, key, 0);
		if(!migrationVersionVariant.isValid()) {
			return false;
		}

		QVector<std::function<bool(Private *, drawdance::Query &)>> migrations =
			{
				&Private::migrateInitial,
				&Private::migrateChangedPreset,
				&Private::migratePresetShortcuts,
				&Private::migrateMarkerBlendMode,
				&Private::migratePresetHistory,
				&Private::migratePresetState,
		};

		int originalMigrationVersion = migrationVersionVariant.toInt();
		int migrationVersion = originalMigrationVersion;
		int migrationCount = migrations.size();

		while(migrationVersion < migrationCount) {
			if(migrations[migrationVersion](this, query)) {
				++migrationVersion;
			} else {
				return false;
			}
		}

		return migrationVersion == originalMigrationVersion ||
			   createOrUpdateStateInternal(query, key, migrationVersion);
	}

	bool migrateInitial(drawdance::Query &query)
	{
		return query.exec(
				   "create table if not exists preset ("
				   "id integer primary key not null, "
				   "type text not null, "
				   "name text not null, "
				   "description text not null, "
				   "thumbnail blob, "
				   "data blob not null)") &&
			   query.exec(
				   "create table if not exists tag ("
				   "id integer primary key not null, "
				   "name text not null)") &&
			   query.exec(
				   "create table if not exists preset_tag ("
				   "preset_id integer not null "
				   "references preset (id) on delete cascade,"
				   "tag_id integer not null "
				   "references tag (id) on delete cascade, "
				   "primary key (preset_id, tag_id))") &&
			   query.exec(
				   "create index if not exists preset_name_idx "
				   "on preset(lower(name))") &&
			   query.exec(
				   "create index if not exists tag_name_idx "
				   "on tag(lower(name))");
	}

	bool migrateChangedPreset(drawdance::Query &query)
	{
		return query.exec("alter table preset add column changed_type text") &&
			   query.exec("alter table preset add column changed_name text") &&
			   query.exec(
				   "alter table preset add column changed_description text") &&
			   query.exec(
				   "alter table preset add column changed_thumbnail blob") &&
			   query.exec("alter table preset add column changed_data blob");
	}

	bool migratePresetShortcuts(drawdance::Query &query)
	{
		return query.exec(
			"alter table preset add column shortcut text not null default ''");
	}

	bool migrateMarkerBlendMode(drawdance::Query &query)
	{
		// There's a brush called "Marker" in the default brush set that was
		// added before the blend mode of the same name. To avoid confusion,
		// we'll retroactively equip it with the blend mode by default.
		if(!query.exec(
			   "update preset set data = cast(json_set(data, "
			   "    '$.settings.blend', '-dp-marker',"
			   "    '$.settings.blendalpha', json('false')) as blob)"
			   "where id = 205 and name = '0900-Marker'")) {
			qWarning("Failed to migrate marker brush to marker blend mode");
		}
		// This migration isn't critical, just carry on if it fails.
		return true;
	}

	bool migratePresetHistory(drawdance::Query &query)
	{
		return query.exec(
			"create table if not exists preset_history ("
			"    preset_id integer primary key not null "
			"        references preset (id) on delete cascade,"
			"    last_used_at real not null)");
	}

	bool migratePresetState(drawdance::Query &query)
	{
		return query.exec(
				   "alter table preset "
				   "add column state integer not null default 0") &&
			   query.exec(
				   "create index if not exists preset_state_idx "
				   "on preset(state)");
	}

	void migratePresets()
	{
		QStringList migrations = {
			QStringLiteral("0001-2_3_0.zpresets"),
			QStringLiteral("0002-2_3_1.zpresets"),
		};

		for(const QString &name : migrations) {
			QString error;
			if(!migratePresetFile(name, error)) {
				qWarning(
					"Failed to migrate preset file %s: %s",
					qUtf8Printable(name), qUtf8Printable(error));
			}
		}
	}

	bool migratePresetFile(const QString &name, QString &outError)
	{
		if(readStateBool(name, false)) {
			return true; // Already migrated.
		}

		QString path = utils::paths::locateDataFile(
			QStringLiteral("brushes/%1").arg(name));
		if(path.isEmpty()) {
			outError = QStringLiteral("Unable to locate file");
			return false;
		}

		QString content = drawdance::decompressZstdFile(path);
		if(content.isEmpty()) {
			outError = QStringLiteral("Error decompressing '%1': %2")
						   .arg(path, QString::fromUtf8(DP_error()));
			return false;
		}

		if(!createOrUpdateState(name, true)) {
			outError = QStringLiteral("Failed to insert migration state");
			return false;
		}

		QTextStream stream(&content);
		QString line;
		int i = 0;
		while(stream.readLineInto(&line)) {
			if(!line.isEmpty()) {
				++i;
				QString error;
				if(!migratePresetFromFile(line, error)) {
					qWarning(
						"Error migrating preset %d from '%s': %s", i,
						qUtf8Printable(path), qUtf8Printable(error));
				}
			}
		}

		return true;
	}

	bool migratePresetFromFile(const QString &line, QString &outError)
	{
		QJsonParseError err;
		QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &err);
		if(err.error != QJsonParseError::NoError) {
			outError = err.errorString();
			return false;
		}

		if(!doc.isObject()) {
			outError = QStringLiteral("Not an object");
			return false;
		}

		QJsonObject object = doc.object();
		int presetId = createPreset(
			int(PresetState::Normal),
			object.value(QStringLiteral("name")).toString(),
			object.value(QStringLiteral("description")).toString(),
			QByteArray::fromBase64(
				object.value(QStringLiteral("thumbnail")).toString().toUtf8()),
			object.value(QStringLiteral("type")).toString(),
			QJsonDocument(object.value(QStringLiteral("data")).toObject())
				.toJson(QJsonDocument::Compact));
		if(presetId == 0) {
			outError = QStringLiteral("Failed to create preset");
			return false;
		}

		QSet<int> tagIdsToAssign;
		for(const QJsonValue &tagValue :
			object.value(QStringLiteral("tags")).toArray()) {
			QString tagName = tagValue.toString();
			if(!tagName.isEmpty()) {
				int tagId = readTagIdByName(tagName);
				if(tagId == 0) {
					tagId = createTag(tagName);
					if(tagId == 0) {
						qWarning(
							"Failed to create tag %s", qUtf8Printable(tagName));
					} else {
						tagIdsToAssign.insert(tagId);
					}
				} else {
					tagIdsToAssign.insert(tagId);
				}
			}
		}

		for(int tagId : tagIdsToAssign) {
			if(!createPresetTag(presetId, tagId)) {
				qWarning(
					"Failed to assign preset %d to tag %d", presetId, tagId);
			}
		}

		return true;
	}

	static drawdance::Database db;
	static Queries *queries;
	CompressedTimer m_presetChangeTimer;
	QHash<int, PresetChange> m_presetChanges;
	QHash<int, PresetHistoryChange> m_presetHistoryChanges;
	QVector<CachedTag> m_tagCache;
	QVector<CachedPreset> m_presetCache;
	PresetShortcutMap m_presetShortcuts;
	BrushPresetModel *m_presetModel = nullptr;
	int m_presetIconSize = BrushPresetModel::THUMBNAIL_SIZE;
	int m_tagIdToFilter = -1;
};

drawdance::Database BrushPresetTagModel::Private::db;
BrushPresetTagModel::Private::Queries *BrushPresetTagModel::Private::queries;

bool Tag::isHistory() const
{
	return id == HISTORY_ID;
}

bool Tag::accepts(const QSet<int> &tagIds) const
{
	switch(id) {
	case ALL_ID:
		return true;
	case UNTAGGED_ID:
		return tagIds.isEmpty();
	case HISTORY_ID:
		return false;
	default:
		return tagIds.contains(id);
	}
}

QString Preset::effectivePreviewTitle() const
{
	QString title = makeEffectivePreviewTitle(effectiveName());
	switch(state) {
	case int(PresetState::Deleted):
		title.append(
			QCoreApplication::translate(
				"brushes::BrushPresetTagModel", " (deleted)"));
		break;
	case int(PresetState::Transient):
		title.append(
			QCoreApplication::translate(
				"brushes::BrushPresetTagModel", " (unsaved)"));
		break;
	default:
		break;
	}
	return title;
}

QString Preset::makeEffectivePreviewTitle(const QString &originalTitle)
{
	QString title = originalTitle.trimmed();

	// Strip any category and number prefix.
	static QRegularExpression prefixRe(
		QStringLiteral("\\A(.*/)?(?>[0-9]*[\\s+_-]*)(?=\\S)"),
		QRegularExpression::DotMatchesEverythingOption |
			QRegularExpression::DontCaptureOption);
	title.replace(prefixRe, QString());
	if(title.isEmpty()) {
		return originalTitle;
	}

	// Split by:
	static QRegularExpression splitRe(QStringLiteral(
		// Whitespace, plus, underscores and dashes
		"[\\s+_-]+"
		// or a lowercase letter followed by an uppercase letter
		"|(?<=\\p{Ll})(?=\\p{Lu})"
		// or any non-whitespace character followed by a #
		"|(?<=\\S)(?=#)"
		// or a letter followed by a number.
		"|(?<=\\p{L})(?=\\p{N})"));
	{
		QStringList pieces = title.split(splitRe, compat::SkipEmptyParts);

		// Uppercase the first grapheme cluster (what a human would see as a
		// single character) of each split piece and join with a space.
		static QRegularExpression ucfirstRe(
			QStringLiteral("\\A(\\p{N}*)(\\X)"));
		for(QString &piece : pieces) {
			QRegularExpressionMatch match = ucfirstRe.match(piece);
			if(match.hasMatch()) {
				piece = match.captured(1) + match.captured(2).toUpper() +
						piece.mid(match.capturedEnd());
			}
		}
		title = pieces.join(QChar(' '));
	}

	return title.isEmpty() ? originalTitle : title;
}

BrushPresetTagModel::BrushPresetTagModel(QObject *parent)
	: QAbstractItemModel(parent)
	, d(new Private(this))
{
	d->setPresetModel(new BrushPresetModel(this));
	d->initShortcuts();
	connect(
		this, &QAbstractItemModel::modelAboutToBeReset, d->presetModel(),
		&BrushPresetModel::tagsAboutToBeReset);
	connect(
		this, &QAbstractItemModel::modelReset, d->presetModel(),
		&BrushPresetModel::tagsReset);
}

BrushPresetTagModel::~BrushPresetTagModel()
{
	delete d;
}

const QString &BrushPresetTagModel::path() const
{
	return d->path();
}

BrushPresetModel *BrushPresetTagModel::presetModel()
{
	return d->presetModel();
}

int BrushPresetTagModel::rowCount(const QModelIndex &parent) const
{
	return parent.isValid() ? 0 : d->tagCacheSize() + TAG_OFFSET;
}

int BrushPresetTagModel::columnCount(const QModelIndex &parent) const
{
	return parent.isValid() ? 0 : 1;
}

QModelIndex BrushPresetTagModel::parent(const QModelIndex &) const
{
	return QModelIndex();
}

QModelIndex
BrushPresetTagModel::index(int row, int column, const QModelIndex &parent) const
{
	if(parent.isValid() || row < 0 || row - TAG_OFFSET >= d->tagCacheSize() ||
	   column != 0) {
		return QModelIndex();
	} else {
		return createIndex(row, column);
	}
}

QVariant BrushPresetTagModel::data(const QModelIndex &index, int role) const
{
	if(!index.isValid() || !isTagRowInBounds(index.row()) ||
	   index.column() != 0) {
		return QVariant();
	}

	switch(role) {
	case Qt::DecorationRole:
		switch(index.row()) {
		case ALL_ROW:
			return QIcon::fromTheme("folder");
		case UNTAGGED_ROW:
			return QIcon::fromTheme("folder-new");
		case HISTORY_ROW:
			return QIcon::fromTheme("backup");
		default:
			return QVariant();
		}
	case Qt::DisplayRole:
	case Qt::EditRole:
		switch(index.row()) {
		case ALL_ROW:
			return tr("All");
		case UNTAGGED_ROW:
			return tr("Untagged");
		case HISTORY_ROW:
			return tr("History");
		default:
			return d->getCachedTag(index.row() - TAG_OFFSET).name;
		}
	case Qt::ToolTipRole:
		switch(index.row()) {
		case ALL_ROW:
			return tr("Show all brushes, regardless of tagging.");
		case UNTAGGED_ROW:
			return tr("Show brushes not assigned to any tag.");
		case HISTORY_ROW:
			return tr("Show a history of brushes used.");
		default:
			return d->getCachedTag(index.row() - TAG_OFFSET).name;
		}
	default:
		return QVariant();
	}
}

bool BrushPresetTagModel::isExportableRow(int row)
{
	switch(row) {
	case ALL_ROW:
	case HISTORY_ROW:
		return false;
	default:
		return true;
	}
}

bool BrushPresetTagModel::isHistoryRow(int row)
{
	return row == HISTORY_ROW;
}

Tag BrushPresetTagModel::getTagAt(int row) const
{
	switch(row) {
	case ALL_ROW:
		return Tag{ALL_ID, data(createIndex(row, 0)).toString()};
	case UNTAGGED_ROW:
		return Tag{UNTAGGED_ID, data(createIndex(row, 0)).toString()};
	case HISTORY_ROW:
		return Tag{HISTORY_ID, data(createIndex(row, 0)).toString()};
	default:
		if(isTagRowInBounds(row)) {
			const CachedTag &ct = d->getCachedTag(row - TAG_OFFSET);
			return Tag{ct.id, ct.name};
		} else {
			return Tag{0, QString()};
		}
	}
}

int BrushPresetTagModel::getTagRowById(int tagId) const
{
	switch(tagId) {
	case ALL_ID:
		return ALL_ROW;
	case UNTAGGED_ID:
		return UNTAGGED_ROW;
	case HISTORY_ID:
		return HISTORY_ROW;
	default: {
		int count = d->tagCacheSize();
		for(int i = 0; i < count; ++i) {
			if(d->getCachedTag(i).id == tagId) {
				return i + TAG_OFFSET;
			}
		}
		return -1;
	}
	}
}

int BrushPresetTagModel::newTag(const QString &name)
{
	beginResetModel();
	int tagId = d->createTag(name);
	d->refreshTagCache();
	endResetModel();
	return tagId;
}

int BrushPresetTagModel::editTag(int tagId, const QString &name)
{
	beginResetModel();
	d->updateTagName(tagId, name);
	d->refreshTagCache();
	endResetModel();
	return getTagRowById(tagId);
}

void BrushPresetTagModel::deleteTag(int tagId)
{
	beginResetModel();
	d->deleteTagById(tagId);
	d->refreshTagCache();
	endResetModel();
}

int BrushPresetTagModel::getStateInt(const QString &key, int defaultValue) const
{
	return d->readStateInt(key, defaultValue);
}

bool BrushPresetTagModel::setStateInt(const QString &key, int value)
{
	return d->createOrUpdateState(key, value);
}

bool BrushPresetTagModel::isBuiltInTag(int row)
{
	return row == ALL_ROW || row == UNTAGGED_ROW || row == HISTORY_ROW;
}

bool BrushPresetTagModel::isTagRowInBounds(int row) const
{
	if(isBuiltInTag(row)) {
		return true;
	} else {
		int i = row - TAG_OFFSET;
		return i >= 0 && i < d->tagCacheSize();
	}
}

BrushImportResult BrushPresetTagModel::importBrushPack(const QString &file)
{
	BundleFormat fmt = detectBundleFormat(file);
	switch(fmt) {
	case BundleFormat::Drawpile:
		return importDrawpileBrushPack(file);
	case BundleFormat::Krita:
		return importKritaBundle(file);
	case BundleFormat::Unknown:
	default: {
		BrushImportResult result = {{}, {}, 0};
		result.errors.append(
			tr("Unknown brush bundle format: '%1'").arg(file));
		return result;
	}
	}
}

BrushImportResult BrushPresetTagModel::importDrawpileBrushPack(
	const QString &file)
{
	BrushImportResult result = {{}, {}, 0};

	drawdance::ZipReader zr{file};
	if(zr.isNull()) {
		qWarning("Error opening '%s': %s", qUtf8Printable(file), DP_error());
		result.errors.append(tr("Can't open '%1'.").arg(file));
		return result;
	}

	beginResetModel();
	QVector<ImportBrushGroup> groups = readOrderConf(result, file, zr);
	for(const ImportBrushGroup &group : groups) {
		if(!group.brushes.isEmpty()) {
			readImportBrushes(result, zr, group.name, group.brushes);
		}
	}

	d->refreshTagCache();
	d->refreshPresetCache();
	endResetModel();
	return result;
}

BundleFormat
BrushPresetTagModel::detectBundleFormat(const QString &file)
{
	drawdance::ZipReader zr{file};
	if(zr.isNull()) {
		return BundleFormat::Unknown;
	}

	if(!zr.readFile(QStringLiteral("order.conf")).isNull()) {
		return BundleFormat::Drawpile;
	}

	if(!zr.readFile(QStringLiteral("bundle.xml")).isNull()) {
		return BundleFormat::Krita;
	}

	QByteArray manifestData =
		zr.readFile(QStringLiteral("META-INF/manifest.xml")).readBytes();
	if(!manifestData.isEmpty()) {
		QDomDocument manifest;
		if(parseDomXml(manifest, manifestData, true).ok &&
		   hasKritaManifestEntry(manifest)) {
			return BundleFormat::Krita;
		}
	}

	return BundleFormat::Unknown;
}

BrushImportResult BrushPresetTagModel::importKritaBundle(
	const QString &file)
{
	BrushImportResult result = {{}, {}, 0};

	drawdance::ZipReader zr{file};
	if(zr.isNull()) {
		qWarning("Error opening '%s': %s", qUtf8Printable(file), DP_error());
		result.errors.append(tr("Can't open '%1'.").arg(file));
		return result;
	}

	QDomDocument doc;
	QStringList presetPaths;
	QString bundleName = QFileInfo(file).baseName();

	QByteArray bundleXml = zr.readFile(QStringLiteral("bundle.xml")).readBytes();
	if(!bundleXml.isEmpty()) {
		DomParseResult parsed = parseDomXml(doc, bundleXml, false);
		if(!parsed.ok) {
			qWarning(
				"Error parsing bundle.xml: %s at line %d, col %d",
				qUtf8Printable(parsed.errorMessage), parsed.errorLine,
				parsed.errorColumn);
			result.errors.append(
				tr("Invalid Krita bundle: cannot parse bundle.xml"));
			return result;
		}

		QDomElement root = doc.documentElement();
		if(root.tagName() != QStringLiteral("bundle")) {
			result.errors.append(
				tr("Invalid Krita bundle: missing <bundle> root element"));
			return result;
		}

		QString name = root.attribute(QStringLiteral("name"));
		if(!name.isEmpty()) {
			bundleName = name;
		}

		QDomNodeList presetNodes =
			doc.elementsByTagName(QStringLiteral("brush"));
		for(int i = 0; i < presetNodes.size(); ++i) {
			QDomElement presetElem = presetNodes.at(i).toElement();
			if(presetElem.isNull()) {
				continue;
			}
			QString presetPath = presetElem.attribute(QStringLiteral("file"));
			if(presetPath.isEmpty()) {
				presetPath = QStringLiteral("presets/%1.kpp")
					.arg(presetElem.attribute(QStringLiteral("id")));
			}
			if(!presetPath.isEmpty() && !presetPaths.contains(presetPath)) {
				presetPaths.append(presetPath);
			}
		}
	} else {
		QByteArray manifestData =
			zr.readFile(QStringLiteral("META-INF/manifest.xml")).readBytes();
		if(manifestData.isEmpty()) {
			result.errors.append(
				tr("Invalid Krita bundle: neither bundle.xml nor "
				   "META-INF/manifest.xml found"));
			return result;
		}

		DomParseResult parsed = parseDomXml(doc, manifestData, true);
		if(!parsed.ok) {
			qWarning(
				"Error parsing manifest.xml: %s at line %d, col %d",
				qUtf8Printable(parsed.errorMessage), parsed.errorLine,
				parsed.errorColumn);
			result.errors.append(
				tr("Invalid Krita bundle: cannot parse manifest.xml"));
			return result;
		}

		presetPaths = readKritaManifestPresetPaths(doc);
		if(presetPaths.isEmpty()) {
			result.errors.append(
				tr("Invalid Krita bundle: no paintop presets in manifest"));
			return result;
		}
	}

	if(presetPaths.isEmpty()) {
		result.errors.append(tr("Invalid Krita bundle: contains no brushes"));
		return result;
	}

	QString tagName = bundleName;
	if(d->readTagCountByName(tagName) != 0) {
		for(int suffix = 1; suffix < 100; ++suffix) {
			QString candidate =
				QStringLiteral("%1 (%2)").arg(bundleName).arg(suffix);
			if(d->readTagCountByName(candidate) == 0) {
				tagName = candidate;
				break;
			}
		}
	}
	if(d->readTagCountByName(tagName) != 0) {
		result.errors.append(
			tr("No free tag name for bundle '%1'.").arg(bundleName));
		return result;
	}

	DRAWPILE_FS_PERSIST_SCOPE(scopedFsSync);
	beginResetModel();
	int imported = 0;
	drawdance::Query qry = d->database().query();
	bool ok = qry.tx([&]() {
		int tagId = d->createTag(qry, tagName);
		if(tagId < 1) {
			result.errors.append(
				tr("Could not create tag '%1'.").arg(tagName));
			return false;
		}
		result.importedTags.append({tagId, tagName});

		for(const QString &presetPath : presetPaths) {
			QByteArray presetData = zr.readFile(presetPath).readBytes();
			if(presetData.isEmpty()) {
				result.errors.append(
					tr("Can't read Krita preset '%1'").arg(presetPath));
				continue;
			}

			KritaBrushPreset kp =
				readKritaPreset(result, zr, presetPath, presetData);
			if(kp.name.isEmpty()) {
				continue;
			}

			QByteArray thumbnailBytes;
			if(!kp.thumbnailData.isEmpty()) {
				QPixmap thumb;
				thumb.loadFromData(kp.thumbnailData);
				thumbnailBytes = LazyThumbnail::toPng(thumb);
			}

			QString brushType = kp.useClassic ? QStringLiteral("classic")
											  : QStringLiteral("mypaint");
			QByteArray brushData = QJsonDocument(kp.brushJson).toJson();

			int presetId = d->createPreset(
				qry, int(PresetState::Normal), kp.name, kp.description,
				thumbnailBytes, brushType, brushData);
			if(presetId < 1) {
				result.errors.append(
					tr("Could not create brush preset '%1'.").arg(kp.name));
				continue;
			}

			if(!d->createPresetTag(qry, presetId, tagId)) {
				result.errors.append(
					tr("Could not assign brush '%1' to tag '%2'.")
						.arg(kp.name).arg(tagName));
				return false;
			}

			++imported;
		}
		return imported > 0;
	});
	if(!ok) {
		result.importedTags.clear();
		if(result.errors.isEmpty()) {
			result.errors.append(
				tr("Could not import Krita bundle '%1'.").arg(file));
		}
	}
	d->refreshTagCache();
	d->refreshPresetCache();
	endResetModel();

	result.importedBrushCount = ok ? imported : 0;
	return result;
}

KritaBrushPreset BrushPresetTagModel::readKritaPreset(
	BrushImportResult &result, const drawdance::ZipReader &zr,
	const QString &presetPath, const QByteArray &presetData)
{
	KritaBrushPreset kp;

	bool isPng = isKritaPngData(presetData);
	QByteArray presetXml = presetData;
	if(isPng) {
		presetXml = extractKritaPresetXml(presetData);
		if(presetXml.isEmpty()) {
			result.errors.append(
				tr("Krita preset '%1' has no embedded preset XML")
					.arg(presetPath));
			return kp;
		}
		kp.thumbnailData = presetData;
	}

	QDomDocument presetDoc;
	DomParseResult parsed = parseDomXml(presetDoc, presetXml, false);
	if(!parsed.ok) {
		qWarning(
			"Error parsing Krita preset '%s': %s at line %d, col %d",
			qUtf8Printable(presetPath), qUtf8Printable(parsed.errorMessage),
			parsed.errorLine, parsed.errorColumn);
		result.errors.append(
			tr("Cannot parse Krita preset '%1'").arg(presetPath));
		return kp;
	}

	QDomDocument doc;
	QDomElement root;
	QDomElement presetRoot = presetDoc.documentElement();
	if(presetRoot.tagName().compare(
			   QStringLiteral("brush"), Qt::CaseInsensitive) == 0) {
		doc = presetDoc;
		root = presetRoot;
	} else if(presetRoot.tagName().compare(
				  QStringLiteral("preset"), Qt::CaseInsensitive) == 0) {
		doc = kritaPresetToLegacyDocument(presetDoc, presetPath);
		root = doc.documentElement();
	} else {
		result.errors.append(
			tr("Invalid Krita preset '%1': missing <brush> or <Preset> root")
				.arg(presetPath));
		return kp;
	}

	if(root.isNull()) {
		result.errors.append(
			tr("Cannot convert Krita preset '%1'").arg(presetPath));
		return kp;
	}

	kp.name = root.firstChildElement(QStringLiteral("name")).text();
	if(kp.name.isEmpty()) {
		kp.name = root.attribute(QStringLiteral("id"));
	}
	if(kp.name.isEmpty()) {
		kp.name = QFileInfo(presetPath).baseName();
	}

	kp.description = root.firstChildElement(QStringLiteral("comment")).text();
	if(kp.description.isEmpty()) {
		kp.description = root.firstChildElement(QStringLiteral("description")).text();
	}

	kp.engine = root.firstChildElement(QStringLiteral("engine")).text();

	QDomElement settingsElem = root.firstChildElement(QStringLiteral("settings"));
	if(!settingsElem.isNull()) {
		kp.useClassic = KritaBrushConverter::shouldUseClassic(kp.engine, doc);
		kp.classicSettings =
			KritaBrushConverter::presetToClassicSettings(doc, kp.engine);
		kp.mypaintSettings =
			KritaBrushConverter::presetToMyPaintMapping(doc, kp.engine);
		kp.brushJson =
			KritaBrushConverter::presetToActiveBrushJson(doc, kp.engine);
	}

	if(!isPng) {
		kp.thumbnailData = extractKritaThumbnail(doc, zr, presetPath);
	}

	return kp;
}

QByteArray BrushPresetTagModel::extractKritaThumbnail(
	const QDomDocument &doc, const drawdance::ZipReader &zr,
	const QString &presetPath)
{
	QDomElement previewElem =
		doc.documentElement().firstChildElement(QStringLiteral("preview"));
	if(previewElem.isNull()) {
		return QByteArray();
	}

	QString previewPath = previewElem.text().trimmed();
	if(previewPath.isEmpty()) {
		previewPath = previewElem.attribute(QStringLiteral("file"));
	}
	if(previewPath.isEmpty()) {
		return QByteArray();
	}

	QFileInfo info{presetPath};
	QString baseDir = info.path();
	if(!previewPath.startsWith(QLatin1Char('/')) && !baseDir.isEmpty()) {
		previewPath = baseDir + QLatin1String("/") + previewPath;
	}

	drawdance::ZipReader::File previewFile = zr.readFile(previewPath);
	if(previewFile.isNull()) {
		return QByteArray();
	}

	return previewFile.readBytes();
}

QJsonArray KritaBrushConverter::kritaCurveToJson(
	const QDomElement &curveElem)
{
	QJsonArray points;
	QDomNodeList pointNodes = curveElem.elementsByTagName(
		QStringLiteral("point"));
	for(int i = 0; i < pointNodes.size(); ++i) {
		QDomElement pointElem = pointNodes.at(i).toElement();
		if(pointElem.isNull()) {
			continue;
		}
		bool xOk = false, yOk = false;
		double x = pointElem.attribute(QStringLiteral("x")).toDouble(&xOk);
		double y = pointElem.attribute(QStringLiteral("y")).toDouble(&yOk);
		if(xOk && yOk) {
			points.append(QJsonArray({x, y}));
		}
	}
	return points;
}

KisCubicCurve KritaBrushConverter::kritaCurveToCubicCurve(
	const QDomElement &curveElem)
{
	KisCubicCurve curve;
	QJsonArray points = kritaCurveToJson(curveElem);
	if(points.isEmpty()) {
		return curve;
	}

	QList<QPointF> curvePoints;
	for(const QJsonValue &v : points) {
		QJsonArray pt = v.toArray();
		if(pt.size() >= 2) {
			curvePoints.append(
				QPointF(pt[0].toDouble(), pt[1].toDouble()));
		}
	}

	if(curvePoints.isEmpty()) {
		return curve;
	}

	curve.setPoints(curvePoints);
	return curve;
}

QJsonObject KritaBrushConverter::presetToClassicSettings(
	const QDomDocument &doc, const QString &engine)
{
	QJsonObject settings;

	settings[QStringLiteral("shape")] = QStringLiteral("round-soft");
	settings[QStringLiteral("size")] = 16.0;
	settings[QStringLiteral("opacity")] = 1.0;
	settings[QStringLiteral("hard")] = 0.0;
	settings[QStringLiteral("spacing")] = 10.0;
	settings[QStringLiteral("smudge")] = 0.0;
	settings[QStringLiteral("jitter")] = 0.0;
	settings[QStringLiteral("resmudge")] = 0;
	settings[QStringLiteral("smudgealpha")] = true;
	settings[QStringLiteral("paintmode")] = QStringLiteral("direct");
	settings[QStringLiteral("indirect")] = false;
	settings[QStringLiteral("colorpick")] = false;
	settings[QStringLiteral("erase")] = false;
	settings[QStringLiteral("pixelperfect")] = false;
	settings[QStringLiteral("pixelartinput")] = false;
	settings[QStringLiteral("stabilizationmode")] = 0;
	settings[QStringLiteral("stabilizer")] = 0;
	settings[QStringLiteral("smoothing")] = 0;
	settings[QStringLiteral("syncsamples")] = false;
	settings[QStringLiteral("confidential")] = false;
	settings[QStringLiteral("blend")] = QStringLiteral("svg:src-over");
	settings[QStringLiteral("blendalpha")] = false;
	settings[QStringLiteral("blenderase")] = QStringLiteral("-dp-erase");
	settings[QStringLiteral("blenderasealpha")] = false;

	QString engineLower = engine.toLower();
	QDomElement root = doc.documentElement();
	QDomElement settingsElem = root.firstChildElement(QStringLiteral("settings"));

	if(!settingsElem.isNull()) {
		QString blend = settingsElem.attribute(QStringLiteral("blend"));
		if(!blend.isEmpty()) {
			settings[QStringLiteral("blend")] = blend;
		}
		settings[QStringLiteral("erase")] =
			settingsElem.attribute(QStringLiteral("erase")) ==
			QLatin1String("true");
		QString paintMode = settingsElem.attribute(QStringLiteral("paintmode"));
		if(!paintMode.isEmpty()) {
			settings[QStringLiteral("paintmode")] = paintMode;
		}
		bool pixel = settingsElem.attribute(QStringLiteral("pixel")) ==
					 QLatin1String("true") ||
				 engineLower.contains(QStringLiteral("pixel"));
		settings[QStringLiteral("pixelperfect")] = pixel;
		settings[QStringLiteral("shape")] =
			pixel ? QStringLiteral("round-pixel")
				  : QStringLiteral("round-soft");

		QDomNodeList settingNodes = settingsElem.childNodes();
		for(int i = 0; i < settingNodes.size(); ++i) {
			QDomElement settingElem = settingNodes.at(i).toElement();
			if(settingElem.isNull()) {
				continue;
			}
			QString id = settingElem.attribute(QStringLiteral("id")).toLower();
			bool ok = false;
			double value = settingElem.text().trimmed().toDouble(&ok);
			if(id.isEmpty() || !ok) {
				continue;
			}

			QString key;
			if(id == QLatin1String("size") || id == QLatin1String("brush_size")) {
				key = QStringLiteral("size");
			} else if(id == QLatin1String("opacity") ||
					  id == QLatin1String("brush_opacity")) {
				key = QStringLiteral("opacity");
			} else if(id == QLatin1String("hard") ||
					  id == QLatin1String("hardness") ||
					  id == QLatin1String("brush_hardness")) {
				key = QStringLiteral("hard");
			} else if(id == QLatin1String("spacing") ||
					  id == QLatin1String("brush_spacing")) {
				key = QStringLiteral("spacing");
			} else if(id == QLatin1String("jitter")) {
				key = QStringLiteral("jitter");
			} else if(id == QLatin1String("smudge")) {
				key = QStringLiteral("smudge");
			} else if(id == QLatin1String("resmudge")) {
				key = QStringLiteral("resmudge");
			}

			if(key.isEmpty()) {
				continue;
			}
			settings[key] = key == QLatin1String("resmudge")
								? QJsonValue(qRound(value))
								: QJsonValue(value);

			QDomElement curveElem = settingElem.firstChildElement(
				QStringLiteral("curve"));
			if(!curveElem.isNull()) {
				settings[key + QStringLiteral("curve")] =
					kritaCurveToCubicCurve(curveElem).toString();
			}

			QString dynamic = settingElem.attribute(QStringLiteral("dynamic"));
			QString dynamicKey;
			if(dynamic == QLatin1String("pressure")) {
				dynamicKey = key + QStringLiteral("p");
			} else if(dynamic == QLatin1String("velocity")) {
				dynamicKey = key + QStringLiteral("v");
			} else if(dynamic == QLatin1String("distance")) {
				dynamicKey = key + QStringLiteral("d");
			}
			if(!dynamicKey.isEmpty()) {
				settings[dynamicKey] = true;
			}
		}

		settings[QStringLiteral("indirect")] =
			settings[QStringLiteral("paintmode")].toString() !=
			QLatin1String("direct");
	}

	if(engineLower == QLatin1String("sketchbrush")) {
		settings[QStringLiteral("jitter")] =
			qMax(settings[QStringLiteral("jitter")].toDouble(), 15.0);
		settings[QStringLiteral("size")] =
			qMin(settings[QStringLiteral("size")].toDouble(), 24.0);
		settings[QStringLiteral("smoothing")] = 1;
	} else if(engineLower == QLatin1String("colorsmudge") ||
			  engineLower == QLatin1String("smudgebrush")) {
		settings[QStringLiteral("smudge")] =
			qMax(settings[QStringLiteral("smudge")].toDouble(), 0.5);
		settings[QStringLiteral("paintmode")] = QStringLiteral("direct");
		settings[QStringLiteral("indirect")] = false;
	} else if(engineLower == QLatin1String("hatchingbrush") ||
			  engineLower == QLatin1String("hairybrush") ||
			  engineLower == QLatin1String("curvebrush")) {
		settings[QStringLiteral("jitter")] =
			qMax(settings[QStringLiteral("jitter")].toDouble(), 10.0);
	} else if(engineLower == QLatin1String("spraybrush") ||
			  settingsElem.attribute(QStringLiteral("airbrush")) ==
				  QLatin1String("true")) {
		settings[QStringLiteral("paintmode")] = QStringLiteral("indirect_wash");
		settings[QStringLiteral("indirect")] = true;
	} else if(engineLower == QLatin1String("filter") ||
			  engineLower == QLatin1String("duplicate")) {
		settings[QStringLiteral("paintmode")] = QStringLiteral("direct");
		settings[QStringLiteral("indirect")] = false;
	}

	int blendMode = canvas::blendmode::fromOraName(
		settings[QStringLiteral("blend")].toString());
	settings[QStringLiteral("blendalpha")] =
		canvas::blendmode::presentsAsAlphaPreserving(blendMode);
	int eraseMode = canvas::blendmode::fromOraName(
		settings[QStringLiteral("blenderase")].toString(),
		DP_BLEND_MODE_ERASE);
	settings[QStringLiteral("blenderasealpha")] =
		canvas::blendmode::presentsAsAlphaPreserving(eraseMode);
	settings[QStringLiteral("syncsamples")] =
		settings[QStringLiteral("smudge")].toDouble() > 0.0;

	return settings;
}

static QJsonArray kritaCurveToMyPaintPoints(const QString &curveString)
{
	KisCubicCurve curve;
	curve.fromString(curveString.isEmpty() ? QStringLiteral("0,0;1,1;")
										   : curveString);
	QList<QPointF> points = curve.points();
	if(points.isEmpty()) {
		return QJsonArray();
	}
	if(points.first().x() > 0.0) {
		points.prepend(QPointF(0.0, points.first().y()));
	}
	if(points.last().x() < 1.0) {
		points.append(QPointF(1.0, points.last().y()));
	}
	QJsonArray result;
	for(const QPointF &point : points) {
		result.append(QJsonArray{
			qBound(0.0, point.x(), 1.0), qBound(0.0, point.y(), 1.0)});
	}
	return result;
}

static void addMyPaintSetting(
	QJsonObject &mapping, const QString &key, double baseValue,
	const QString &input = QString(), const QJsonArray &points = QJsonArray())
{
	QJsonObject inputs;
	if(!input.isEmpty() && !points.isEmpty()) {
		inputs[input] = points;
	}
	mapping[key] = QJsonObject{
		{QStringLiteral("base_value"), baseValue},
		{QStringLiteral("inputs"), inputs},
	};
}

static QString myPaintDynamicInput(const QJsonObject &settings, const QString &key)
{
	if(settings.value(key + QStringLiteral("p")).toBool()) {
		return QStringLiteral("pressure");
	} else if(settings.value(key + QStringLiteral("v")).toBool()) {
		return QStringLiteral("speed1");
	} else if(settings.value(key + QStringLiteral("d")).toBool()) {
		return QStringLiteral("stroke");
	}
	return QString();
}

QJsonObject KritaBrushConverter::presetToMyPaintMapping(
	const QDomDocument &doc, const QString &engine)
{
	QJsonObject classic = presetToClassicSettings(doc, engine);
	QHash<QString, QString> params = readKritaParamAttributes(doc);
	QString engineLower = engine.toLower();

	double size = qBound(1.0, classic[QStringLiteral("size")].toDouble(), 1000.0);
	double opacity = qBound(
		0.0, classic[QStringLiteral("opacity")].toDouble(1.0), 1.0);
	double hard = qBound(0.0, classic[QStringLiteral("hard")].toDouble(), 1.0);
	double spacing = qBound(
		1.0, classic[QStringLiteral("spacing")].toDouble(10.0), 1000.0);
	double jitter = qBound(
		0.0, classic[QStringLiteral("jitter")].toDouble(), 100.0);
	double smudge = qBound(
		0.0, classic[QStringLiteral("smudge")].toDouble(), 1.0);
	int resmudge = classic[QStringLiteral("resmudge")].toInt();
	bool pixel = classic[QStringLiteral("pixelperfect")].toBool() ||
				 engineLower.contains(QStringLiteral("pixel"));

	QString sizeInput = myPaintDynamicInput(classic, QStringLiteral("size"));
	QString opacityInput =
		myPaintDynamicInput(classic, QStringLiteral("opacity"));
	QString hardInput = myPaintDynamicInput(classic, QStringLiteral("hard"));
	QString smudgeInput =
		myPaintDynamicInput(classic, QStringLiteral("smudge"));

	QJsonObject mapping;
	double radius = qBound(-2.0, std::log2(size) - 1.0, 6.0);
	addMyPaintSetting(
		mapping, QStringLiteral("radius_logarithmic"), radius, sizeInput,
		kritaCurveToMyPaintPoints(
			classic[QStringLiteral("sizecurve")].toString()));
	addMyPaintSetting(
		mapping, QStringLiteral("opaque"), opacity, opacityInput,
		kritaCurveToMyPaintPoints(
			classic[QStringLiteral("opacitycurve")].toString()));
	addMyPaintSetting(
		mapping, QStringLiteral("opaque_multiply"), 0.0,
		QStringLiteral("pressure"),
		QJsonArray({QJsonArray({0.0, 0.0}), QJsonArray({1.0, 1.0})}));
	addMyPaintSetting(
		mapping, QStringLiteral("hardness"), hard, hardInput,
		kritaCurveToMyPaintPoints(
			classic[QStringLiteral("hardcurve")].toString()));
	addMyPaintSetting(
		mapping, QStringLiteral("anti_aliasing"), pixel ? 0.0 : 1.0);
	addMyPaintSetting(
		mapping, QStringLiteral("snap_to_pixel"), pixel ? 1.0 : 0.0);
	addMyPaintSetting(
		mapping, QStringLiteral("dabs_per_basic_radius"),
		qBound(0.0, 100.0 / spacing, 20.0));
	addMyPaintSetting(
		mapping, QStringLiteral("dabs_per_actual_radius"), 2.0);
	addMyPaintSetting(
		mapping, QStringLiteral("offset_by_random"),
		qBound(0.0, jitter / 100.0, 25.0));
	addMyPaintSetting(
		mapping, QStringLiteral("smudge"), smudge, smudgeInput,
		kritaCurveToMyPaintPoints(
			classic[QStringLiteral("smudgecurve")].toString()));
	if(smudge > 0.0 || resmudge > 0) {
		addMyPaintSetting(
			mapping, QStringLiteral("smudge_length"),
			resmudge > 0 ? 1.0 / (resmudge + 1) : 0.5);
		addMyPaintSetting(mapping, QStringLiteral("smudge_radius_log"), 0.0);
	}

	bool airbrush =
		kritaParamBool(params, QStringLiteral("airbrushoption/isairbrushing"));
	if(airbrush || engineLower == QLatin1String("spraybrush")) {
		double rate = kritaParamDouble(
			params, QStringLiteral("airbrushoption/rate"),
			kritaParamDouble(
				params, QStringLiteral("spray/particlecount"), 20.0));
		addMyPaintSetting(
			mapping, QStringLiteral("dabs_per_second"),
			qBound(0.0, rate, 200.0));
	}
	if(engineLower == QLatin1String("colorsmudge") ||
	   engineLower == QLatin1String("hatchingbrush") ||
	   engineLower == QLatin1String("hairybrush") ||
	   engineLower == QLatin1String("curvebrush")) {
		addMyPaintSetting(mapping, QStringLiteral("paint_mode"), 0.25);
	}
	if(engineLower == QLatin1String("sketchbrush") ||
	   engineLower == QLatin1String("hairybrush") ||
	   engineLower == QLatin1String("curvebrush")) {
		addMyPaintSetting(mapping, QStringLiteral("slow_tracking"), 1.5);
		addMyPaintSetting(
			mapping, QStringLiteral("tracking_noise"),
			qBound(0.0, jitter / 50.0, 12.0));
	}
	if(engineLower == QLatin1String("hairybrush")) {
		double shear = kritaParamDouble(
			params, QStringLiteral("hairybristle/shear"), 0.0);
		addMyPaintSetting(
			mapping, QStringLiteral("elliptical_dab_ratio"),
			qBound(1.0, 1.0 + qAbs(shear) * 10.0, 10.0));
		addMyPaintSetting(
			mapping, QStringLiteral("elliptical_dab_angle"),
			kritaParamDouble(
				params, QStringLiteral("hairybristle/scale"), 0.0));
	}
	if(kritaParamBool(params, QStringLiteral("coloroption/userandomhsv"))) {
		addMyPaintSetting(
			mapping, QStringLiteral("color_h"),
			kritaParamDouble(params, QStringLiteral("coloroption/hue"), 0.0) /
				360.0);
		addMyPaintSetting(
			mapping, QStringLiteral("color_s"),
			kritaParamDouble(
				params, QStringLiteral("coloroption/saturation"), 0.0) /
				100.0);
		addMyPaintSetting(
			mapping, QStringLiteral("color_v"),
			kritaParamDouble(params, QStringLiteral("coloroption/value"), 0.0) /
				100.0);
	}

	return mapping;
}

QJsonObject KritaBrushConverter::presetToActiveBrushJson(
	const QDomDocument &doc, const QString &engine)
{
	if(shouldUseClassic(engine, doc)) {
		return QJsonObject{
			{QStringLiteral("type"), QStringLiteral("dp-classic")},
			{QStringLiteral("version"), 1},
			{QStringLiteral("settings"), presetToClassicSettings(doc, engine)},
		};
	}

	QJsonObject classic = presetToClassicSettings(doc, engine);
	QString blend = classic[QStringLiteral("blend")].toString();
	int blendMode = canvas::blendmode::fromOraName(blend);
	if(!canvas::blendmode::isValidBrushMode(blendMode) ||
	   !DP_blend_mode_compatible(blendMode, true)) {
		blend = QStringLiteral("svg:src-over");
		blendMode = DP_BLEND_MODE_NORMAL;
	}
	bool erase = classic[QStringLiteral("erase")].toBool();
	QString indirect = classic[QStringLiteral("paintmode")].toString();

	QJsonObject settings{
		{QStringLiteral("blend"), blend},
		{QStringLiteral("blenderase"), QStringLiteral("-dp-erase")},
		{QStringLiteral("blendalpha"),
		 canvas::blendmode::presentsAsAlphaPreserving(blendMode)},
		{QStringLiteral("blenderasealpha"), false},
		{QStringLiteral("erase"), erase},
		{QStringLiteral("stabilizationmode"), 0},
		{QStringLiteral("stabilizer"), 0},
		{QStringLiteral("smoothing"), 0},
		{QStringLiteral("syncsamples"),
		 classic[QStringLiteral("smudge")].toDouble() > 0.0},
		{QStringLiteral("confidential"), false},
		{QStringLiteral("pixelperfect"),
		 classic[QStringLiteral("pixelperfect")].toBool()},
		{QStringLiteral("mapping"), presetToMyPaintMapping(doc, engine)},
		{QStringLiteral("lock_alpha"), false},
		{QStringLiteral("indirect"), indirect != QLatin1String("direct")},
	};
	return QJsonObject{
		{QStringLiteral("type"), QStringLiteral("dp-mypaint")},
		{QStringLiteral("version"), 1},
		{QStringLiteral("settings"), settings},
	};
}

bool KritaBrushConverter::shouldUseClassic(
	const QString &engine, const QDomDocument &doc)
{
	QString engineLower = engine.toLower();
	static const QSet<QString> classicEngines{
		QStringLiteral("paintbrush"),
		QStringLiteral("pixelbrush"),
		QStringLiteral("sketchbrush"),
		QStringLiteral("roundmarker"),
		QStringLiteral("tangentnormal"),
		QStringLiteral("duplicate"),
		QStringLiteral("filter"),
		QStringLiteral("deformbrush"),
	};
	static const QSet<QString> myPaintEngines{
		QStringLiteral("colorsmudge"),
		QStringLiteral("hatchingbrush"),
		QStringLiteral("hairybrush"),
		QStringLiteral("curvebrush"),
		QStringLiteral("spraybrush"),
		QStringLiteral("dynabrush"),
		QStringLiteral("particlebrush"),
		QStringLiteral("gridbrush"),
		QStringLiteral("experimentbrush"),
	};

	if(classicEngines.contains(engineLower)) {
		return true;
	}
	if(myPaintEngines.contains(engineLower)) {
		return false;
	}
	if(engineLower.contains(QStringLiteral("pixel")) ||
	   (engineLower.contains(QStringLiteral("smudge")) &&
		!engineLower.contains(QStringLiteral("color")))) {
		return true;
	}
	if(engineLower.contains(QStringLiteral("watercolor")) ||
	   engineLower.contains(QStringLiteral("color")) ||
	   engineLower.contains(QStringLiteral("marker"))) {
		return false;
	}

	QHash<QString, QString> params = readKritaParamAttributes(doc);
	if(kritaParamBool(params, QStringLiteral("airbrushoption/isairbrushing"))) {
		return false;
	}

	QDomElement settingsElem =
		doc.documentElement().firstChildElement(QStringLiteral("settings"));
	if(!settingsElem.isNull()) {
		int settingCount = 0;
		QDomNodeList settingNodes = settingsElem.childNodes();
		for(int i = 0; i < settingNodes.size(); ++i) {
			if(!settingNodes.at(i).toElement().isNull()) {
				++settingCount;
			}
		}
		return settingCount <= 7;
	}

	return true;
}

namespace {
static QRegularExpression newlineRe{"[\r\n]+"};
}

QVector<BrushPresetTagModel::ImportBrushGroup>
BrushPresetTagModel::readOrderConf(
	BrushImportResult &result, const QString &file,
	const drawdance::ZipReader &zr)
{
	static QRegularExpression groupRe{"^Group:\\s*(.+)$"};

	drawdance::ZipReader::File orderFile =
		zr.readFile(QStringLiteral("order.conf"));
	if(orderFile.isNull()) {
		qWarning("Error reading order.conf from zip: %s", DP_error());
		result.errors.append(
			tr("Invalid brush pack: order.conf not found inside"));
		return {};
	}

	QFileInfo info{file};
	QVector<BrushPresetTagModel::ImportBrushGroup> groups;
	groups.append({tr("Uncategorized %1").arg(info.baseName()), {}});

	int current = 0;
	bool haveBrush = false;
	QStringList order =
		orderFile.readUtf8().split(newlineRe, compat::SkipEmptyParts);
	for(const QString &line : order) {
		QString trimmed = line.trimmed();
		if(!trimmed.isEmpty() && !trimmed.startsWith("#")) {
			QRegularExpressionMatch match = groupRe.match(trimmed);
			if(match.hasMatch()) {
				current = addOrderConfGroup(groups, match.captured(1));
			} else {
				haveBrush = true;
				addOrderConfBrush(groups[current].brushes, trimmed);
			}
		}
	}

	if(haveBrush) {
		return groups;
	} else {
		result.errors.append(
			tr("Invalid brush pack: order.conf contains no brushes"));
		return {};
	}
}

int BrushPresetTagModel::addOrderConfGroup(
	QVector<ImportBrushGroup> &groups, const QString &name)
{
	int count = groups.size();
	for(int i = 0; i < count; ++i) {
		if(groups[i].name == name) {
			return i;
		}
	}
	groups.append({name, {}});
	return count;
}

void BrushPresetTagModel::addOrderConfBrush(
	QStringList &brushes, const QString &brush)
{
	if(!brushes.contains(brush)) {
		brushes.append(brush);
	}
}

void BrushPresetTagModel::readImportBrushes(
	BrushImportResult &result, const drawdance::ZipReader &zr,
	const QString &groupName, const QStringList &brushes)
{
	QString tagName = groupName;
	if(d->readTagCountByName(groupName) != 0) {
		for(int i = 1; i < 100; ++i) {
			QString candidate = QStringLiteral("%1 (%2)").arg(groupName).arg(i);
			if(d->readTagCountByName(candidate) == 0) {
				tagName = candidate;
				break;
			}
		}
	}

	int tagId = -1;
	int count = brushes.size();
	for(int i = 0; i < count; ++i) {
		const QString &prefix = brushes[i];
		ActiveBrush brush;
		QString description;
		QPixmap thumbnail;
		if(readImportBrush(result, zr, prefix, brush, description, thumbnail)) {
			if(tagId == -1) {
				tagId = d->createTag(tagName);
				if(tagId < 1) {
					result.errors.append(
						tr("Could not create tag '%1'.").arg(tagName));
					break;
				}
				result.importedTags.append({tagId, tagName});
			}

			int slashIndex = prefix.indexOf("/");
			QString name =
				QStringLiteral("%1/%2-%3")
					.arg(tagName)
					.arg(i * 100, 4, 10, QLatin1Char('0'))
					.arg(slashIndex < 0 ? prefix : prefix.mid(slashIndex + 1));

			int presetId = d->createPreset(
				int(PresetState::Normal), name, description,
				LazyThumbnail::toPng(thumbnail), brush.presetType(),
				brush.presetData());
			if(presetId < 1) {
				result.errors.append(
					tr("Could not create brush preset '%1'.").arg(name));
			}

			++result.importedBrushCount;

			if(!d->createPresetTag(presetId, tagId)) {
				result.errors.append(
					tr("Could not assign brush '%1' to tag '%2'.")
						.arg(name)
						.arg(tagName));
			}
		}
	}
}

bool BrushPresetTagModel::readImportBrush(
	BrushImportResult &result, const drawdance::ZipReader &zr,
	const QString &prefix, ActiveBrush &outBrush, QString &outDescription,
	QPixmap &outThumbnail)
{
	QString mybPath = QStringLiteral("%1.myb").arg(prefix);
	drawdance::ZipReader::File mybFile = zr.readFile(mybPath);
	if(mybFile.isNull()) {
		qWarning(
			"Error reading '%s' from zip: %s", qUtf8Printable(mybPath),
			DP_error());
		result.errors.append(tr("Can't read brush file '%1'").arg(mybPath));
		return false;
	}

	QByteArray mybBytes = mybFile.readBytes();
	QJsonValue value;
	switch(guessImportBrushType(mybBytes)) {
	case ImportBrushType::Json:
		value = readImportBrushJson(result, mybPath, mybBytes);
		break;
	case ImportBrushType::Old:
		value = readImportBrushOld(result, mybPath, mybBytes);
		break;
	default:
		result.errors.append(
			tr("Unknown brush format in file '%1'").arg(mybPath));
		return false;
	}

	if(!value.isObject()) {
		return false;
	}

	QJsonObject object = value.toObject();
	if(!outBrush.fromExportJson(object)) {
		result.errors.append(
			tr("Can't load brush from brush file '%1'").arg(mybPath));
		return false;
	}

	outDescription = object["notes"].toString();
	if(outDescription.isNull()) {
		outDescription = object["description"].toString();
		if(outDescription.isNull()) {
			outDescription = "";
		}
	}

	QString prevPath = QStringLiteral("%1_prev.png").arg(prefix);
	drawdance::ZipReader::File prevFile = zr.readFile(prevPath);
	if(!prevFile.isNull()) {
		outThumbnail.loadFromData(prevFile.readBytes());
	}

	if(outThumbnail.isNull()) {
		outThumbnail = outBrush.presetThumbnail();
	}

	if(outThumbnail.width() != BrushPresetModel::THUMBNAIL_SIZE ||
	   outThumbnail.height() != BrushPresetModel::THUMBNAIL_SIZE) {
		outThumbnail = outThumbnail.scaled(
			BrushPresetModel::THUMBNAIL_SIZE, BrushPresetModel::THUMBNAIL_SIZE,
			Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	}

	return true;
}

BrushPresetTagModel::ImportBrushType
BrushPresetTagModel::guessImportBrushType(const QByteArray &mybBytes)
{
	for(char c : mybBytes) {
		if(c == '{') {
			return ImportBrushType::Json;
		} else if(c == '#') {
			return ImportBrushType::Old;
		} else if(!std::isspace(c)) {
			break;
		}
	}
	return ImportBrushType::Unknown;
}

QJsonValue BrushPresetTagModel::readImportBrushJson(
	BrushImportResult &result, const QString &mybPath,
	const QByteArray &mybBytes)
{
	QJsonParseError error;
	QJsonDocument json = QJsonDocument::fromJson(mybBytes, &error);
	if(error.error != QJsonParseError::NoError) {
		result.errors.append(
			tr("Brush file '%1' does not contain valid JSON: %1")
				.arg(mybPath)
				.arg(error.errorString()));
		return QJsonValue();
	} else if(!json.isObject()) {
		result.errors.append(
			tr("Brush file '%1' does not contain a JSON object").arg(mybPath));
		return QJsonValue();
	} else {
		return json.object();
	}
}

// SPDX-SnippetBegin
// SPDX-License-Identifier: GPL-2.0-or-later
// SDPX—SnippetName: old format parsing from MyPaint
QJsonValue BrushPresetTagModel::readImportBrushOld(
	BrushImportResult &result, const QString &mybPath,
	const QByteArray &mybBytes)
{
	QVector<QPair<QString, QString>> rawSettings;
	int version = 1;
	for(const QString &rawLine :
		QString::fromUtf8(mybBytes).split(newlineRe, compat::SkipEmptyParts)) {
		QString line = rawLine.trimmed();
		if(line.isEmpty() || line.startsWith(QChar('#'))) {
			continue;
		}

		int spaceIndex = line.indexOf(QChar(' '));
		if(spaceIndex <= 0) {
			continue;
		}

		QString rawCname = line.left(spaceIndex);
		QString rawValue = line.mid(spaceIndex + 1);
		if(rawCname == QStringLiteral("version")) {
			version = rawValue.toInt();
			if(version != 1 && version != 2) {
				result.errors.append(
					tr("Brush file '%1' has invalid version %d")
						.arg(mybPath)
						.arg(version));
				return QJsonValue();
			}
		} else {
			rawSettings.append({rawCname, rawValue});
		}
	}

	QJsonObject settings = {
		{QStringLiteral("anti_aliasing"),
		 QJsonObject({
			 {QStringLiteral("base_value"), 0.0},
			 {QStringLiteral("inputs"), QJsonObject()},
		 })},
	};
	for(const QPair<QString, QString> &rawSetting : rawSettings) {
		const QString &rawCname = rawSetting.first;
		const QString &rawValue = rawSetting.second;

		if(rawCname == QStringLiteral("parent_brush_name") ||
		   rawCname == QStringLiteral("group") ||
		   rawCname == QStringLiteral("comment") ||
		   rawCname == QStringLiteral("notes") ||
		   rawCname == QStringLiteral("description")) {
			settings[rawCname] = QUrl::fromPercentEncoding(rawValue.toUtf8());
		} else if(version <= 1 && rawCname == QStringLiteral("color")) {
			QStringList rawColor =
				rawValue.split(QChar(' '), compat::SkipEmptyParts);
			if(rawColor.size() == 3) {
				QColor color(
					qBound(0, rawColor[0].toInt(), 255),
					qBound(0, rawColor[1].toInt(), 255),
					qBound(0, rawColor[2].toInt(), 255));
				if(color.isValid()) {
					settings[QStringLiteral("color_h")] = QJsonObject(
						{{QStringLiteral("base_value"), color.hueF()},
						 {QStringLiteral("inputs"), QJsonObject()}});
					settings[QStringLiteral("color_s")] = QJsonObject(
						{{QStringLiteral("base_value"), color.saturationF()},
						 {QStringLiteral("inputs"), QJsonObject()}});
					settings[QStringLiteral("color_v")] = QJsonObject(
						{{QStringLiteral("base_value"), color.valueF()},
						 {QStringLiteral("inputs"), QJsonObject()}});
					continue;
				}
			}
			result.errors.append(
				tr("Brush file '%1' contains invalid 'color' setting")
					.arg(mybPath));
		} else if(
			(version <= 1 && (rawCname == QStringLiteral("change_radius") ||
							  rawCname == QStringLiteral("painting_time"))) ||
			(version <= 2 &&
			 rawCname == QStringLiteral("adapt_color_from_image"))) {
			result.errors.append(
				tr("Brush file '%1' contains obsolete '%2' setting")
					.arg(mybPath));
		} else {
			QString cname;
			double (*scale)(double) = nullptr;
			if(version <= 1 && rawCname == QStringLiteral("speed")) {
				cname = QStringLiteral("speed1");
			} else if(rawCname == QStringLiteral("color_hue")) {
				cname = QStringLiteral("change_color_h");
				scale = scaleOldColorHue;
			} else if(rawCname == QStringLiteral("color_saturation")) {
				cname = QStringLiteral("change_color_hsv_s");
				scale = scaleOldColorSaturationOrValue;
			} else if(rawCname == QStringLiteral("color_value")) {
				cname = QStringLiteral("change_color_v");
				scale = scaleOldColorSaturationOrValue;
			} else if(rawCname == QStringLiteral("speed_slowness")) {
				cname = QStringLiteral("speed1_slowness");
			} else if(rawCname == QStringLiteral("change_color_s")) {
				cname = QStringLiteral("change_color_hsv_s");
			} else if(rawCname == QStringLiteral("stroke_thresold")) {
				cname = QStringLiteral("stroke_threshold");
			} else {
				cname = rawCname;
			}

			QStringList parts = rawValue.split(QChar('|'));
			if(parts.size() >= 1) {
				double baseValue = parts.takeFirst().toDouble();
				if(scale) {
					baseValue = scale(baseValue);
				}

				QJsonObject inputs;
				for(const QString &part : parts) {
					QString trimmedPart = part.trimmed();
					int partSpaceIndex = trimmedPart.indexOf(QChar(' '));
					if(partSpaceIndex > 0) {
						QJsonArray points;
						if(version <= 1) {
							static QRegularExpression whitespaceRe(
								QStringLiteral("\\s"));
							QStringList rawPoints =
								trimmedPart.mid(partSpaceIndex + 1)
									.split(
										whitespaceRe, compat::SkipEmptyParts);
							points.append(QJsonArray({0.0, 0.0}));
							while(rawPoints.size() >= 2) {
								double x = rawPoints.takeFirst().toDouble();
								if(x == 0.0) {
									break;
								} else {
									double y = rawPoints.takeFirst().toDouble();
									points.append(
										QJsonArray({x, scale ? scale(y) : y}));
								}
							}
						} else {
							for(const QString &rawPoint :
								trimmedPart.mid(partSpaceIndex + 1)
									.split(
										QStringLiteral(", "),
										compat::SkipEmptyParts)) {
								static QRegularExpression pointRe(
									QStringLiteral(
										"\\A\\s*\\((\\S+)\\s+(\\S+)"
										"\\)\\s*\\z"));
								QRegularExpressionMatch match =
									pointRe.match(rawPoint);
								if(match.hasMatch()) {
									double x = match.captured(1).toDouble();
									double y = match.captured(2).toDouble();
									points.append(
										QJsonArray({x, scale ? scale(y) : y}));
								} else {
									qWarning(
										"Invalid point '%s'",
										qUtf8Printable(rawPoint));
								}
							}
						}

						inputs[trimmedPart.left(partSpaceIndex)] = points;
					}
				}

				settings[cname] = QJsonObject{
					{QStringLiteral("base_value"), baseValue},
					{QStringLiteral("inputs"), inputs},
				};
			}
		}
	}

	return QJsonObject({
		{QStringLiteral("version"), 3},
		{QStringLiteral("settings"), settings},
	});
}

double BrushPresetTagModel::scaleOldColorHue(double y)
{
	return y * 64.0 / 360.0;
}

double BrushPresetTagModel::scaleOldColorSaturationOrValue(double y)
{
	return y * 128.0 / 256.0;
}
// SDPX—SnippetEnd


BrushExportResult BrushPresetTagModel::exportBrushPack(
	const QString &file, const QVector<BrushExportTag> &exportTags) const
{
	BrushExportResult result = {false, {}, 0};

	drawdance::ZipWriter zw{file};
	if(zw.isNull()) {
		qWarning("Error opening '%s': %s", qUtf8Printable(file), DP_error());
		result.errors.append(tr("Can't open '%1'.").arg(file));
		return result;
	}

	QStringList order;
	{
		QSet<QString> tagPaths;
		QSet<QString> tagNames;
		for(const BrushExportTag &tag : exportTags) {
			exportTag(result, order, tagPaths, tagNames, zw, tag);
		}
	}

	if(result.exportedBrushCount == 0) {
		result.errors.append(tr("No brushes exported"));
		return result;
	}

	if(!zw.addFile(QStringLiteral("order.conf"), order.join("\n").toUtf8())) {
		qWarning("Error exporting order.conf: %s", DP_error());
		result.errors.append(tr("Can't export order.conf"));
		return result;
	}

	if(zw.finish()) {
		result.ok = true;
	} else {
		result.errors.append(
			tr("Error writing '%1': %2").arg(file).arg(DP_error()));
	}
	return result;
}

void BrushPresetTagModel::exportTag(
	BrushExportResult &result, QStringList &order, QSet<QString> &tagPaths,
	QSet<QString> &tagNames, drawdance::ZipWriter &zw,
	const BrushExportTag &tag) const
{
	QString tagPath = makeExportNameUnique(tagPaths, makeExportSafe(tag.name));
	if(!zw.addDir(tagPath)) {
		qWarning(
			"Error creating tag directory '%s': %s", qUtf8Printable(tagPath),
			DP_error());
		result.errors.append(tr("Can't export tag '%1'").arg(tag.name));
		return;
	}

	order.append(QStringLiteral("Group: %1")
					 .arg(makeExportNameUnique(tagNames, tag.name)));
	QSet<QString> presetNames;
	for(int presetId : tag.presetIds) {
		exportPreset(result, order, presetNames, zw, tagPath, presetId);
	}
	order.append(QString{});
}

void BrushPresetTagModel::exportPreset(
	BrushExportResult &result, QStringList &order, QSet<QString> &presetNames,
	drawdance::ZipWriter &zw, const QString &tagPath, int presetId) const
{
	std::optional<Preset> opt = d->readPresetById(presetId);
	if(!opt.has_value()) {
		qWarning("Preset %d not found", presetId);
		result.errors.append(tr("Missing preset %1").arg(presetId));
		return;
	}

	const Preset &preset = opt.value();
	QByteArray exportData = preset.originalConstBrushLoad().toExportJson(
		preset.originalDescription);

	QString presetName = makeExportNameUnique(
		presetNames, getExportName(presetId, preset.originalName));
	QString presetPath = QStringLiteral("%1/%2").arg(tagPath, presetName);

	QString dataPath = QStringLiteral("%1.myb").arg(presetPath);
	if(!zw.addFile(dataPath, exportData)) {
		qWarning(
			"Error exporting preset '%s': %s", qUtf8Printable(dataPath),
			DP_error());
		result.errors.append(
			tr("Can't export preset '%1'").arg(preset.originalName));
		return;
	}

	QString thumbnailPath = QStringLiteral("%1_prev.png").arg(presetPath);
	if(!zw.addFile(
		   thumbnailPath,
		   LazyThumbnail::toPng(preset.originalThumbnailPixmap()))) {
		qWarning(
			"Error exporting preset thumbnail '%s': %s",
			qUtf8Printable(thumbnailPath), DP_error());
		result.errors.append(
			tr("Can't export preset thumbnail '%1'").arg(preset.originalName));
		return;
	}

	order.append(presetPath);
	++result.exportedBrushCount;
}

QString BrushPresetTagModel::makeExportNameUnique(
	QSet<QString> &names, const QString &name)
{
	if(names.contains(name)) {
		QString newName;
		int i = 2;
		do {
			newName = name + QString::number(i++);
		} while(names.contains(newName));
		names.insert(newName);
		return newName;
	} else {
		names.insert(name);
		return name;
	}
}

QString
BrushPresetTagModel::getExportName(int presetId, const QString presetName)
{
	static QRegularExpression baseNameRe{"(?:[0-9]+-)?([^/\\\\]+)$"};
	QRegularExpressionMatch match = baseNameRe.match(presetName);
	if(match.hasMatch()) {
		return makeExportSafe(match.captured(1));
	} else {
		return QStringLiteral("Brush%1").arg(presetId);
	}
}

QString BrushPresetTagModel::makeExportSafe(const QString &s)
{
	static QRegularExpression safeRe{"[^a-zA-Z0-9_ -]"};
	QString t = s.trimmed();
	t.replace(safeRe, QStringLiteral("_"));
	return t;
}


BrushPresetModel::BrushPresetModel(BrushPresetTagModel *tagModel)
	: QAbstractItemModel(tagModel)
	, d(tagModel->d)
{
}

bool BrushPresetModel::isPresetRowInBounds(int row) const
{
	return row >= 0 && row < d->presetCacheSize();
}

int BrushPresetModel::rowCount(const QModelIndex &parent) const
{
	if(parent.isValid()) {
		return 0;
	} else {
		return d->presetCacheSize();
	}
}

int BrushPresetModel::rowCountForTagId(int tagId) const
{
	switch(tagId) {
	case ALL_ID:
		return d->readPresetCountAll();
	case UNTAGGED_ID:
		return d->readPresetCountByUntagged();
	case HISTORY_ID:
		return d->readPresetCountHistory();
	default:
		return d->readPresetCountByTagId(tagId);
	}
}

int BrushPresetModel::columnCount(const QModelIndex &parent) const
{
	return parent.isValid() ? 0 : 1;
}

QModelIndex BrushPresetModel::parent(const QModelIndex &) const
{
	return QModelIndex();
}

QModelIndex
BrushPresetModel::index(int row, int column, const QModelIndex &parent) const
{
	if(parent.isValid() || row < 0 || row >= d->presetCacheSize() ||
	   column != 0) {
		return QModelIndex();
	} else {
		return createIndex(row, column, quintptr(0));
	}
}

QModelIndex BrushPresetModel::cachedIndexForId(int presetId)
{
	int i = d->getCachedPresetIndexById(presetId);
	return i == -1 ? QModelIndex() : createIndex(i, 0, quintptr(0));
}

QModelIndex BrushPresetModel::indexForTagId(int tagId, int row) const
{
	int presetId;
	switch(tagId) {
	case ALL_ID:
		presetId = d->readPresetIdAtIndexAll(row);
		break;
	case UNTAGGED_ID:
		presetId = d->readPresetIdAtIndexByUntagged(row);
		break;
	default:
		presetId = d->readPresetIdAtIndexByTagId(row, tagId);
		break;
	}
	if(presetId > 0) {
		return createIndex(row, 0, quintptr(presetId));
	} else {
		return QModelIndex();
	}
}

QVariant BrushPresetModel::data(const QModelIndex &index, int role) const
{
	if(role == Qt::SizeHintRole) {
		return iconSize();
	}

	bool cached = index.internalId() == 0;
	if(cached && (index.column() != 0 || index.row() >= d->presetCacheSize())) {
		return QVariant();
	}

	switch(role) {
	case IdRole:
		if(cached) {
			return d->getCachedPreset(index.row()).id;
		} else {
			return int(index.internalId());
		}
	case Qt::ToolTipRole:
	case FilterRole:
	case EffectiveTitleRole: {
		QString s;
		if(cached) {
			s = d->getCachedPreset(index.row()).effectiveName();
		} else {
			s = d->readPresetEffectiveNameById(index.internalId());
		}
		if(role == FilterRole) {
			QString previewTitle = Preset::makeEffectivePreviewTitle(s);
			if(previewTitle != s) {
				s.append(QStringLiteral(" "));
				s.append(previewTitle);
			}
		}
		return s;
	}
	case EffectiveDescriptionRole:
		if(cached) {
			return d->getCachedPreset(index.row()).effectiveDescription();
		} else {
			return d->readPresetEffectiveDescriptionById(index.internalId());
		}
	case EffectiveThumbnailRole: {
		if(cached) {
			return d->getCachedPreset(index.row()).effectiveThumbnailPixmap();
		} else {
			QPixmap pixmap;
			if(pixmap.loadFromData(
				   d->readPresetEffectiveThumbnailById(index.internalId()))) {
				return pixmap;
			} else {
				return QVariant();
			}
		}
	}
	case HasChangesRole:
		if(cached) {
			return d->getCachedPreset(index.row()).hasChanges();
		} else {
			return d->readPresetHasChangesById(index.internalId());
		}
	case PresetRole:
		if(cached) {
			return QVariant::fromValue<Preset>(d->getCachedPreset(index.row()));
		} else {
			std::optional<Preset> opt = d->readPresetById(index.internalId());
			return opt.has_value() ? QVariant::fromValue<Preset>(opt.value())
								   : QVariant();
		}
	case StateRole:
		if(cached) {
			return d->getCachedPreset(index.row()).state;
		} else {
			return d->readPresetStateById(index.internalId());
		}
	default:
		return QVariant();
	}
}

int BrushPresetModel::getIdFromIndex(const QModelIndex &index)
{
	if(index.isValid()) {
		if(index.internalId() == 0) {
			int row = index.row();
			return isPresetRowInBounds(row) ? d->getCachedPreset(row).id : 0;
		} else {
			return index.internalId();
		}
	} else {
		return 0;
	}
}

void BrushPresetModel::setTagIdToFilter(int tagId)
{
	beginResetModel();
	if(tagId == HISTORY_ID) {
		d->writePresetChanges();
	}
	d->setTagIdToFilter(tagId);
	endResetModel();
}

QList<TagAssignment> BrushPresetModel::getTagAssignments(int presetId)
{
	int i = d->getCachedPresetIndexById(presetId);
	if(i == -1) {
		return d->readTagAssignmentsByPresetId(presetId);
	} else {
		const QSet<int> assignedTagIds = d->getCachedPreset(i).tagIds;
		QList<TagAssignment> tagAssignments;
		int tagCount = d->tagCacheSize();
		for(int j = 0; j < tagCount; ++j) {
			const CachedTag &ct = d->getCachedTag(j);
			tagAssignments.append(
				{ct.id, ct.name, assignedTagIds.contains(ct.id)});
		}
		return tagAssignments;
	}
}

std::optional<Preset> BrushPresetModel::searchPresetBrushData(int presetId)
{
	int i = d->getCachedPresetIndexById(presetId);
	if(i == -1) {
		return d->readPresetById(presetId);
	} else {
		return d->getCachedPreset(i);
	}
}

QPixmap BrushPresetModel::searchPresetThumbnail(int presetId)
{
	int i = d->getCachedPresetIndexById(presetId);
	if(i == -1) {
		QPixmap pixmap;
		if(pixmap.loadFromData(d->readPresetEffectiveThumbnailById(presetId))) {
			return pixmap;
		} else {
			return QPixmap();
		}
	} else {
		return d->getCachedPreset(i).effectiveThumbnailPixmap();
	}
}

bool BrushPresetModel::changeTagAssignment(
	int presetId, int tagId, bool assigned)
{
	bool ok = assigned ? d->createPresetTag(presetId, tagId)
					   : d->deletePresetTag(presetId, tagId);

	int tagIdToFilter = d->tagIdToFilter();
	switch(tagIdToFilter) {
	case ALL_ID:
	case HISTORY_ID:
		if(int i = d->getCachedPresetIndexById(presetId); i != -1) {
			CachedPreset &cp = d->getCachedPreset(i);
			if(assigned) {
				cp.tagIds.insert(tagId);
			} else {
				cp.tagIds.remove(tagId);
			}
			QModelIndex idx = index(i);
			emit dataChanged(idx, idx);
		}
		break;
	case UNTAGGED_ID:
		if(assigned) {
			int i = d->getCachedPresetIndexById(presetId);
			if(i != -1) {
				beginRemoveRows(QModelIndex(), i, i);
				d->removeCachedPreset(i);
				endRemoveRows();
			}
		}
		break;
	default:
		if(tagId == tagIdToFilter) {
			int i = d->getCachedPresetIndexById(presetId);
			if(assigned && i == -1) {
				beginResetModel();
				d->refreshPresetCache();
				endResetModel();
			} else if(!assigned && i != -1) {
				beginRemoveRows(QModelIndex(), i, i);
				d->removeCachedPreset(i);
				endRemoveRows();
			} else {
				CachedPreset &cp = d->getCachedPreset(i);
				if(assigned) {
					cp.tagIds.insert(tagId);
				} else {
					cp.tagIds.remove(tagId);
				}
				QModelIndex idx = index(i);
				emit dataChanged(idx, idx);
			}
		}
		break;
	}

	return ok;
}

std::optional<Preset> BrushPresetModel::newPreset(
	QString name, QString description, QPixmap thumbnail, ActiveBrush brush,
	int tagId)
{
	beginResetModel();
	LazyThumbnail lt = LazyThumbnail::fromPixmap(thumbnail);
	int presetId = d->createPreset(
		int(PresetState::Normal), name, description, lt.bytes(),
		brush.presetType(), brush.presetData());
	if(presetId > 0 && tagId > 0) {
		d->createPresetTag(presetId, tagId);
	}
	d->refreshPresetCache();
	endResetModel();
	d->refreshShortcuts();
	if(presetId > 0) {
		return Preset{
			presetId, int(PresetState::Normal),
			name,	  description,
			lt,		  LazyBrush::fromBrush(brush),
			{},		  {},
			{},		  {},
		};
	} else {
		return {};
	}
}

bool BrushPresetModel::updatePreset(
	int presetId, QString name, QString description, QPixmap thumbnail,
	ActiveBrush brush)
{
	beginResetModel();
	d->removePresetChange(presetId);
	bool ok = d->updatePreset(
		presetId, name, description, LazyThumbnail::toPng(thumbnail),
		brush.presetType(), brush.presetData());
	d->refreshPresetCache();
	endResetModel();
	d->refreshShortcuts();
	emit presetChanged(presetId, name, description, thumbnail, brush);
	return ok;
}

bool BrushPresetModel::updatePresetBrush(int presetId, ActiveBrush brush)
{
	beginResetModel();
	d->removePresetChange(presetId);
	bool ok =
		d->updatePresetBrush(presetId, brush.presetType(), brush.presetData());
	d->refreshPresetCache();
	endResetModel();
	return ok;
}

bool BrushPresetModel::updatePresetShortcut(int presetId, QKeySequence shortcut)
{
	bool ok = d->updatePresetShortcut(
		presetId, shortcut.toString(QKeySequence::PortableText));
	d->refreshShortcuts();
	emit presetShortcutChanged(presetId, shortcut);
	return ok;
}

bool BrushPresetModel::deletePreset(int presetId)
{
	int cachedIndex = d->getCachedPresetIndexById(presetId);
	int state;
	if(cachedIndex == -1) {
		state = d->readPresetStateById(presetId);
	} else {
		state = d->getCachedPreset(cachedIndex).state;
	}

	bool inHistoryTag = d->tagIdToFilter() == HISTORY_ID;
	bool fullyDelete = state != int(PresetState::Normal);
	bool ok;
	if(fullyDelete) {
		ok = d->deletePresetById(presetId);
	} else {
		addPresetIdToHistory(presetId);
		if(inHistoryTag) {
			// Manipulating the preset history can change the cached index.
			cachedIndex = d->getCachedPresetIndexById(presetId);
		}
		ok = d->updatePresetState(presetId, int(PresetState::Deleted));
	}

	if(cachedIndex != -1) {
		if(fullyDelete || !inHistoryTag) {
			beginRemoveRows(QModelIndex(), cachedIndex, cachedIndex);
			d->removeCachedPreset(cachedIndex);
			endRemoveRows();
		} else {
			d->getCachedPreset(cachedIndex).state = int(PresetState::Deleted);
			QModelIndex idx = createIndex(cachedIndex, 0, quintptr(0));
			Q_EMIT dataChanged(idx, idx, {StateRole});
		}
	}

	d->refreshShortcuts();
	if(fullyDelete) {
		Q_EMIT presetRemoved(presetId);
	} else {
		Q_EMIT presetStateChanged(presetId, int(PresetState::Deleted));
	}
	return ok;
}

bool BrushPresetModel::undeletePreset(int presetId)
{
	int cachedIndex = d->getCachedPresetIndexById(presetId);
	int state;
	if(cachedIndex == -1) {
		state = d->readPresetStateById(presetId);
	} else {
		state = d->getCachedPreset(cachedIndex).state;
	}

	if(state != int(PresetState::Deleted)) {
		qWarning("Can't undelete preset %d in state %d", presetId, state);
		return false;
	}

	bool ok = d->updatePresetState(presetId, int(PresetState::Normal));

	if(cachedIndex != -1) {
		d->getCachedPreset(cachedIndex).state = int(PresetState::Normal);
		QModelIndex idx = createIndex(cachedIndex, 0, quintptr(0));
		Q_EMIT dataChanged(idx, idx, {StateRole});
	}

	Q_EMIT presetStateChanged(presetId, int(PresetState::Normal));
	return ok;
}

void BrushPresetModel::changePreset(
	int presetId, const std::optional<QString> &name,
	const std::optional<QString> &description,
	const std::optional<LazyThumbnail> &thumbnail,
	const std::optional<LazyBrush> &brush, bool inEraserSlot)
{
	int i = d->getCachedPresetIndexById(presetId);
	if(i != -1) {
		CachedPreset &cp = d->getCachedPreset(i);
		cp.changedName = name;
		cp.changedDescription = description;
		cp.changedThumbnail = thumbnail;
		cp.changedBrush = brush;
		if(brush.has_value() && inEraserSlot) {
			cp.changedBrush->brush(cp.id).setEraser(
				cp.originalConstBrushLoad().isEraser());
		}
		QModelIndex idx = index(i);
		emit dataChanged(idx, idx);
	}
	d->enqueuePresetChange(presetId, {name, description, thumbnail, brush});
}

void BrushPresetModel::resetAllPresetChanges()
{
	beginResetModel();
	d->discardPresetChanges();
	d->resetAllPresetChanges();
	d->resetAllPresetsInCache();
	endResetModel();
	d->refreshShortcuts();
}

void BrushPresetModel::writePresetChanges()
{
	d->writePresetChanges();
}

bool BrushPresetModel::saveTransientPreset(
	int presetId, const QString &name, const QString &description,
	const QPixmap &thumbnail, const QSet<int> &tagIds)
{
	int state = getPresetState(presetId);
	if(state != int(PresetState::Transient)) {
		qWarning("Can't save transient preset %d in state %d", presetId, state);
		return false;
	}

	if(d->hasPresetChange(presetId)) {
		writePresetChanges();
	}

	bool ok = d->updateTransientPreset(
		presetId, int(PresetState::Normal), name, description,
		LazyThumbnail::fromPixmap(thumbnail).bytes(), tagIds);

	beginResetModel();
	d->refreshPresetCache();
	endResetModel();

	Q_EMIT transientPresetChanged(
		presetId, int(PresetState::Normal), name, description, thumbnail);

	return ok;
}

void BrushPresetModel::addPresetIdToHistory(int presetId)
{
	if(d->tagIdToFilter() == HISTORY_ID) {
		int cachedIndex = d->getCachedPresetIndexById(presetId);
		if(cachedIndex == -1) {
			CachedPreset cp;
			if(d->loadCachedPresetById(presetId, cp)) {
				beginInsertRows(QModelIndex(), 0, 0);
				d->prependCachedPreset(std::move(cp));
				endInsertRows();
				Q_EMIT presetPrepended(presetId, true);
			}
		} else if(cachedIndex != 0) {
			beginMoveRows(
				QModelIndex(), cachedIndex, cachedIndex, QModelIndex(), 0);
			d->prependCachedIndex(cachedIndex);
			endMoveRows();
			Q_EMIT presetPrepended(presetId, false);
		}
	}
	d->enqueuePresetHistoryChange(
		presetId, {drawdance::Database::currentTimeSubsec()});
}

void BrushPresetModel::removePresetFromHistory(int presetId)
{
	int state = getPresetState(presetId);
	if(d->tagIdToFilter() == HISTORY_ID) {
		int cachedIndex = d->getCachedPresetIndexById(presetId);
		if(cachedIndex != -1) {
			beginRemoveRows(QModelIndex(), cachedIndex, cachedIndex);
			d->removeCachedPreset(cachedIndex);
			endRemoveRows();
		}
	}
	d->removePresetHistoryChange(presetId);
	d->deletePresetHistoryById(presetId);
	if(state != int(PresetState::Normal)) {
		Q_EMIT presetRemoved(presetId);
	}
}

void BrushPresetModel::clearHistory()
{
	if(d->tagIdToFilter() == HISTORY_ID) {
		int cacheSize = d->presetCacheSize();
		if(cacheSize != 0) {
			beginRemoveRows(QModelIndex(), 0, cacheSize - 1);
			d->clearCachedPresets();
			endRemoveRows();
		}
	}

	d->clearPresetHistoryChanges();
	d->deleteAllPresetHistory();

	QVector<int> orphanedPresetIds = d->readOrphanedDeletedPresets();
	d->deleteOrphanedDeletedPresets();
	for(int presetId : orphanedPresetIds) {
		Q_EMIT presetRemoved(presetId);
	}
}

int BrushPresetModel::countHistoryPresetsPendingRemoval()
{
	return d->readPresetHistoryCountPendingRemoval();
}

int BrushPresetModel::getPresetState(int presetId)
{
	int cachedIndex = d->getCachedPresetIndexById(presetId);
	if(cachedIndex == -1) {
		return d->readPresetStateById(presetId);
	} else {
		return d->getCachedPreset(cachedIndex).state;
	}
}

int BrushPresetModel::countNames(const QString &name) const
{
	return d->readPresetCountByName(name);
}

int BrushPresetModel::handleReceivedBrush(
	const QString &username, const ActiveBrush &brush)
{
	d->writePresetChanges();

	int presetId =
		d->readPresetIdByContent(brush.presetType(), brush.presetData());
	if(presetId > 0) {
		return presetId;
	}

	{
		QLocale locale;
		QDateTime now = QDateTime::currentDateTime();
		QString name =
			QStringLiteral("Request %1").arg(now.toString("yyyyMMddhhmm"));
		QString description =
			//: %1 is a username, %2 is a date, %3 is a time.
			tr("Brush requested from user \"%1\" on %2 at %3.")
				.arg(
					username, now.toString(locale.dateFormat()),
					now.toString(locale.timeFormat()));
		LazyThumbnail lt = LazyThumbnail::fromPixmap(
			QIcon::fromTheme(QStringLiteral("draw-brush"))
				.pixmap(THUMBNAIL_SIZE, THUMBNAIL_SIZE));
		presetId = d->createPreset(
			int(PresetState::Transient), name, description, lt.bytes(),
			brush.presetType(), brush.presetData());
	}

	if(presetId > 0) {
		addPresetIdToHistory(presetId);
	}

	return presetId;
}

void BrushPresetModel::getShortcutActions(
	const std::function<
		void(const QString &, const QString &, const QKeySequence &)> &fn) const
{
	d->getShortcutActions(fn);
}

QKeySequence BrushPresetModel::getShortcutForPresetId(int presetId)
{
	return d->getShortcutForPresetId(presetId);
}

QVector<int>
BrushPresetModel::getPresetIdsForShortcut(const QKeySequence &shortcut) const
{
	return d->getPresetIdsForShortcut(shortcut);
}

QVector<ShortcutPreset> BrushPresetModel::getShortcutPresets() const
{
	return d->getShortcutPresets();
}

QSize BrushPresetModel::iconSize() const
{
	int dimension = iconDimension();
	return QSize(dimension, dimension);
}

int BrushPresetModel::iconDimension() const
{
	int dimension = d->presetIconSize();
	return dimension <= 0 ? THUMBNAIL_SIZE : dimension;
}

void BrushPresetModel::setIconDimension(int dimension)
{
	beginResetModel();
	d->setPresetIconSize(dimension);
	endResetModel();
}

bool BrushPresetModel::looksLikeBrushShortcutObjectName(
	const QString &objectName)
{
	return objectName.startsWith(
		BrushPresetTagModel::Private::brushShortcutPrefix());
}

void BrushPresetModel::tagsAboutToBeReset()
{
	beginResetModel();
}

void BrushPresetModel::tagsReset()
{
	endResetModel();
}

}
