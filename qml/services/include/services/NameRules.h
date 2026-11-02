#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

namespace NameRules {
// "" when `name` is usable; else the sentence that says why not. `taken` is every
// name already in the roster EXCEPT the row being renamed (compared without case).
inline QString problem(const QString &name, const QStringList &taken, const QString &noun)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return QCoreApplication::translate("NameRules", "Give the %1 a name.").arg(noun);
    for (const QString &other : taken)
        if (other.compare(trimmed, Qt::CaseInsensitive) == 0)
            return QCoreApplication::translate("NameRules", "There is already a %1 called “%2”.")
                       .arg(noun, trimmed);
    return {};
}
} // namespace NameRules