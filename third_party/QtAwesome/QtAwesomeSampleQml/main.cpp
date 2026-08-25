/**
 * MIT Licensed
 *
 * Copyright 2011-2025 - Ribit Software by Blommers IT. All Rights Reserved.
 * Author Rick Blommers
 */

#include "QtAwesome.h"

#include <QApplication>
#include <QQmlApplicationEngine>

#include "QtAwesome.h"
#include "QtAwesomeQuickImageProvider.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    fa::QtAwesome* awesome = new fa::QtAwesome(qApp);
    awesome->initFontAwesome();

    QQmlApplicationEngine engine;
    engine.addImageProvider("fa", new QtAwesomeQuickImageProvider(awesome));
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));

    int result = app.exec();
    delete awesome;
    return result;
}

