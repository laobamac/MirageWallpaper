#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

// Paints an OpenType glyph by its glyph ID because font glyph names are not Unicode text.
class GlyphPreview : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString fontSource READ fontSource WRITE setFontSource NOTIFY fontSourceChanged)
    Q_PROPERTY(int glyphId READ glyphId WRITE setGlyphId NOTIFY glyphIdChanged)
    Q_PROPERTY(QColor ink READ ink WRITE setInk NOTIFY inkChanged)

public:
    // Creates a transparent preview item used both in grid cells and the inspector.
    explicit GlyphPreview(QQuickItem *parent = nullptr);

    // Returns the font file used to resolve the current glyph ID.
    QString fontSource() const;

    // Returns the OpenType glyph ID to paint.
    int glyphId() const;

    // Returns the fill colour used for the glyph outline.
    QColor ink() const;

    // Sets the font file while keeping glyph IDs tied to that font's own table.
    void setFontSource(const QString &source);

    // Sets the selected glyph ID and requests a repaint.
    void setGlyphId(int glyphId);

    // Sets the glyph colour without changing the font outline.
    void setInk(const QColor &color);

    // Draws the glyph outline centered and scaled within the fixed item bounds.
    void paint(QPainter *painter) override;

signals:
    void fontSourceChanged();
    void glyphIdChanged();
    void inkChanged();

private:
    QString m_fontSource;
    int m_glyphId;
    QColor m_ink;
};
