#pragma once

#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleOption>

// Pure-code checkbox painting — no SVGs, no images, no data URIs.
// Draws a dark rounded box with a grey/blue border and a white checkmark.
class DarkCheckBoxStyle : public QProxyStyle {
public:
  using QProxyStyle::QProxyStyle;

  void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                     QPainter *painter,
                     const QWidget *widget = nullptr) const override {
    if (element == PE_IndicatorCheckBox ||
        element == PE_IndicatorItemViewItemCheck) {
      painter->save();
      painter->setRenderHint(QPainter::Antialiasing, true);

      const QRect r = option->rect;
      const bool checked = option->state & State_On;
      const bool hovered = option->state & State_MouseOver;

      // --- Border colour ---
      QColor borderColor("#555555");
      if (checked)
        borderColor = QColor("#00AAFF");
      else if (hovered)
        borderColor = QColor("#00AAFF");

      // --- Draw rounded background box ---
      painter->setPen(QPen(borderColor, 2));
      painter->setBrush(QColor("#1A1A1A")); // dark fill, never blue
      painter->drawRoundedRect(r.adjusted(1, 1, -1, -1), 3, 3);

      // --- Draw white tick when checked ---
      if (checked) {
        QPen pen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);

        const qreal x = r.x();
        const qreal y = r.y();
        const qreal w = r.width();
        const qreal h = r.height();

        QPainterPath path;
        path.moveTo(x + w * 0.20, y + h * 0.52);
        path.lineTo(x + w * 0.42, y + h * 0.72);
        path.lineTo(x + w * 0.80, y + h * 0.28);
        painter->drawPath(path);
      }

      painter->restore();
      return; // fully handled — skip default
    }

    // Everything else: delegate to the base style
    QProxyStyle::drawPrimitive(element, option, painter, widget);
  }
};
