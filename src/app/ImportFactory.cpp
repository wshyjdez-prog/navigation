#include "ImportFactory.h"
#include "ImportPage.h"

namespace p1::app
{
QWidget* createImportPage(DataRepository* repo, QWidget* parent)
{
    return new ImportPage(repo, parent);
}
}
