#pragma once

#include <QString>
#include <QVariantMap>

#include "modules/presentation/PresentationTypes.hpp"

namespace services::preview {
bps::presentation::OutputStyleSpec outputStyleForPreview(const QString &styleId,
                                                         const QString &slideTag = QString());

// Map the engine's slide composition to the QML preview shape.
QVariantMap slideToVariantMap(const bps::presentation::Slide *slide,
                              const bps::presentation::OutputStyleSpec &style);

// Same, against the engine's active output style.
QVariantMap slideToVariantMap(const bps::presentation::Slide *slide);

void setSlideReadingFlow(QVariantMap &slide, bool enabled, const QString &flowText, qreal flowChar);
} // namespace services::preview