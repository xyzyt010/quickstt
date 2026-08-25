#pragma once

#include <QQuickImageProvider>
#include "QtAwesome.h"

class QtAwesomeQuickImageProvider : public QQuickImageProvider
{
public:
    QtAwesomeQuickImageProvider(fa::QtAwesome* awesome)
      : QQuickImageProvider(QQuickImageProvider::ImageType::Pixmap)
      , _awesomeRef(awesome)
    {}

    virtual QPixmap requestPixmap(const QString &id, QSize *size, const QSize &requestedSize) {
      return _awesomeRef->requestPixmap(id, size, requestedSize);
    }

private:
    fa::QtAwesome* _awesomeRef;
};
