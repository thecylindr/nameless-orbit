#ifndef LOCALIZER_H
#define LOCALIZER_H

#include <QHash>
#include <QString>
#include <QTranslator>

// Перевод интерфейса: тексты в коде написаны по-русски (qsTr / QCoreApplication::translate), английский берётся из словаря i18n/en.json
// («русская строка» → «английская»). Словарь подключается как обычный QTranslator, поэтому engine.retranslate() переключает язык на лету.
// Нет строки в словаре — остаётся исходная русская (интерфейс не пустеет)
class DictTranslator : public QTranslator
{
public:
    explicit DictTranslator(QObject *parent = nullptr) : QTranslator(parent) {}
    bool loadJson(const QString &path);                                   // false — файла нет или он испорчен
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override { return m_dict.isEmpty(); }

private:
    QHash<QString, QString> m_dict;
};

#endif
