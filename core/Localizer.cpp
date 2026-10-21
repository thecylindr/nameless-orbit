#include "Localizer.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

bool DictTranslator::loadJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty()) return false;
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) m_dict.insert(it.key(), it.value().toString());
    return true;
}

QString DictTranslator::translate(const char *, const char *sourceText, const char *, int) const
{
    const auto it = m_dict.constFind(QString::fromUtf8(sourceText));
    return it == m_dict.constEnd() ? QString() : it.value();          // пустая строка — «перевода нет», Qt покажет исходный текст
}
