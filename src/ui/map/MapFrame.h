#pragma once
#include <QImage>
#include <QPainter>
#include <QRectF>
namespace p1::ui {
inline QRectF mapFrameTargetRect(QRectF previousView,QRectF currentView,QSize size) {
    if(!previousView.isValid()||!currentView.isValid())return {};
    return {(previousView.left()-currentView.left())/currentView.width()*size.width(),
        (currentView.bottom()-previousView.bottom())/currentView.height()*size.height(),
        previousView.width()/currentView.width()*size.width(),previousView.height()/currentView.height()*size.height()};
}
// Keep the previous complete base map registered to its geographic viewport.
// This image contains no routes/markers, which are drawn in the current view.
inline QImage reprojectMapFrame(const QImage& previous,QRectF previousView,
    QRectF currentView,QSize size,QColor background) {
    QImage image(size,QImage::Format_ARGB32_Premultiplied);image.fill(background);
    if(previous.isNull()||!previousView.isValid()||!currentView.isValid())return image;
    QPainter painter(&image);
    const auto target=mapFrameTargetRect(previousView,currentView,size);
    painter.drawImage(target,previous);
    return image;
}
}
