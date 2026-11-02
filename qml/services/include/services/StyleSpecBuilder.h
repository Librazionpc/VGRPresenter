#pragma once

#include <QString>

#include "modules/presentation/PresentationTypes.hpp"

class StyleListModel;

namespace StyleSpecBuilder {
StyleListModel *uiStyleRoster();

bps::presentation::OutputStyleSpec specForStyleRow(int row, const StyleListModel *styles,
                                                   const QString &slideTag = QString());

bps::presentation::OutputStyleSpec specForStyleId(const QString &styleId,
                                                  const QString &slideTag = QString());
} // namespace StyleSpecBuilder