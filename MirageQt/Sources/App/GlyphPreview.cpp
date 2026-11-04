#include "GlyphPreview.h"

#include <QHash>
#include <QFont>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QRectF>
#include <QTransform>
#include <QtGlobal>

GlyphPreview::GlyphPreview(QQuickItem *parent)
    : QQuickPaintedItem(parent),
      m_glyphId(-1),
      m_ink(Qt::blue)
{
    setAntialiasing(true);
}

QString GlyphPreview::fontSource() const
{
    return m_fontSource;
}

int GlyphPreview::glyphId() const
{
    return m_glyphId;
}

QColor GlyphPreview::ink() const
{
    return m_ink;
}

void GlyphPreview::setFontSource(const QString &source)
{
    if (m_fontSource == source)
        return;
    m_fontSource = source;
    emit fontSourceChanged();
    update();
}

void GlyphPreview::setGlyphId(int glyphId)
{
    if (m_glyphId == glyphId)
        return;
    m_glyphId = glyphId;
    emit glyphIdChanged();
    update();
}

void GlyphPreview::setInk(const QColor &color)
{
    if (m_ink == color)
        return;
    m_ink = color;
    emit inkChanged();
    update();
}

void GlyphPreview::paint(QPainter *painter)
{
    if (m_fontSource.isEmpty() || m_glyphId < 0 || width() <= 0 || height() <= 0)
        return;

    static QHash<QString, QRawFont> fontCache;
    static QMutex fontCacheMutex;
    QRawFont rawFont;
    {
        QMutexLocker locker(&fontCacheMutex);
        const auto cachedFont = fontCache.constFind(m_fontSource);
        if (cachedFont == fontCache.cend()) {
            const QRawFont loadedFont(m_fontSource, 96.0, QFont::PreferNoHinting);
            if (!loadedFont.isValid())
                return;
            fontCache.insert(m_fontSource, loadedFont);
            rawFont = loadedFont;
        } else {
            rawFont = cachedFont.value();
        }
    }

    const QPainterPath glyphPath = rawFont.pathForGlyph(static_cast<quint32>(m_glyphId));
    const QRectF bounds = glyphPath.boundingRect();
    if (bounds.isEmpty())
        return;

    const qreal availableWidth = qMax<qreal>(1.0, width() - 16.0);
    const qreal availableHeight = qMax<qreal>(1.0, height() - 16.0);
    const qreal scale = qMin(availableWidth / bounds.width(), availableHeight / bounds.height());
    QTransform transform;
    transform.translate((width() - bounds.width() * scale) / 2.0,
                        (height() - bounds.height() * scale) / 2.0);
    transform.scale(scale, scale);
    transform.translate(-bounds.left(), -bounds.top());

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(m_ink);
    painter->drawPath(transform.map(glyphPath));
}
