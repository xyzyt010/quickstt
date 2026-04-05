#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

int main(int argc, char *argv[]) {
  QApplication a(argc, argv);
  QString appPath = QCoreApplication::applicationDirPath();

  QString svgFile = appPath + "/../QuickSTT_App/Untitled-1.svg";
  QString pngFile = appPath + "/../QuickSTT_App/app_icon.png";

  QSvgRenderer renderer(svgFile);
  if (!renderer.isValid()) {
    qDebug() << "Failed:" << svgFile;
    return 1;
  }

  int s = 256;
  QImage img(s, s, QImage::Format_ARGB32);
  img.fill(Qt::transparent);
  QPainter painter(&img);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);

  // 30% smaller + preserve aspect ratio (no stretch)
  double innerSize = s * 0.67;
  QSizeF viewSize = renderer.defaultSize();
  QSizeF scaled = viewSize.scaled(innerSize, innerSize, Qt::KeepAspectRatio);
  double x = (s - scaled.width()) / 2.0;
  double y = (s - scaled.height()) / 2.0;
  renderer.render(&painter, QRectF(x, y, scaled.width(), scaled.height()));
  painter.end();

  img.save(pngFile, "PNG");
  qDebug() << "Saved:" << pngFile << "original:" << viewSize
           << "rendered:" << scaled;
  return 0;
}
