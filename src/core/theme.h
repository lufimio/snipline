#pragma once

#include <QObject>
#include <QVariantMap>

namespace snip {

class Theme final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap tokens READ tokens NOTIFY changed)
public:
  explicit Theme(QObject *parent = nullptr);
  QVariantMap tokens() const { return m_tokens; }
  // Transactional: a rejected document never changes the current theme.
  bool load(const QByteArray &json, QString *error);
signals:
  void changed();

private:
  QVariantMap m_tokens;
};

} // namespace snip
