#ifndef IMPORTFACTORY_H
#define IMPORTFACTORY_H
#include <QWidget>
class DataRepository;
#include "ImportPage.h"

namespace p1::app
{
QWidget* createImportPage(DataRepository* repo, QWidget* parent);
}

#endif // IMPORTFACTORY_H
